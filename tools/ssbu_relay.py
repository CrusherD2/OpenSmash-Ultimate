#!/usr/bin/env python3
"""Netplay lobby for the SSBU native port: the server every player's game joins to play online.

Players' games join it at start (Play SSBU.bat). A game that isn't version 13.0.5 is refused, with a
message the game shows (WindowSmash plays 13.0.5 only: REQUIRED_BUILD). A room anyone creates in
Local Wireless is listed to every player of the same lobby code, who join it from the game's Rooms List;
the lobby then carries the room's traffic between its players only. Players only send to the
lobby, so nobody needs port forwarding; the lobby's machine needs its UDP port open. Players in
the same room whose games can do it are also introduced to each other, so their games can send the
room's traffic straight to each other (UDP hole punching) and use the lobby only when that fails.
The protocol is runtime/net_relay.c's (version 2); the host player's game can run a smaller relay
of its own for a private room (launchers\\Host SSBU Netplay.bat).

ssbu-recomp's games join the same lobby on a port of their own (--su-port, 39500): their protocol
is "SU" version 1 (ssbu-recomp/runtime/net_relay.c), with its own numbers and layouts, and the
same rooms, limits, rates and timeouts. Players of the two protocols never share a room.

    python3 ssbu_relay.py [--port 39420] [--su-port 39500] [--bind 0.0.0.0] [--verbose] [--banlist FILE]
    python3 ssbu_relay.py --probe HOST[:PORT] [--room CODE] [--build ID]   (is a lobby there?)

Python 3.8+, standard library only."""
import argparse
import asyncio
import collections
import ipaddress
import os
import secrets
import signal
import socket
import struct
import sys
import time

PROTOCOL = 2
JOIN, WELCOME, REJECT, PING, PONG, DATA, MEMBERS, LEAVE, LIST, LISTING, LIST_END = range(1, 12)
PEERS, BCAST, PUNCH, DIRECT = range(12, 16)   # (direct connections; PUNCH and DIRECT go between players)
NAME_LEN = 32
BUILD_LEN = 40
# 13.0.5's main module build id (tools/game_version.py MAIN_BUILD_ID; this file is deployed on its own)
REQUIRED_BUILD = b"21450C647B8C5940ED5B68218EA128FD3C5D597D"
NOT_13 = "WindowSmash needs game 13.0.5: run the installer again"   # (a REJECT's text: 63 bytes at most)
JOIN_SIZE = 4 + 8 + 2 * NAME_LEN + BUILD_LEN
V1_JOIN_SIZE = 4 + 8 + 2 * NAME_LEN
PING_SIZE = 4 + 8 + 8 + 8 + 2
PING_EXT_SIZE = PING_SIZE + 4 + 4 + 2 + 1 + 1   # (a game that can connect directly says so in its PINGs)
PEER = struct.Struct(">I32sBIHIHQ")   # a PEERS entry
ROOM_MAX = 8
INTRO_ROOMS = 20      # rooms a player is introduced in an hour, at most
PEERS_GAP = 0.25      # seconds between two PEERS to a player, at least
SUBNET = 0x0A0D0000
BROADCASTS = (0x0A0DFFFF, 0xFFFFFFFF)
DEFAULT_PORT = 39420
LIST_MAX = 24

# Local wireless's own messages (runtime/hle_net.c), carried in DATA between port 65511s:
# 'L', type, 0, 0, payload.
LDN_PORT = 65511
LM_ADVERTISE, LM_JOIN, LM_ACCEPT, LM_REJECT, LM_KEEP, LM_LEAVE, LM_GONE, LM_KICK = range(1, 9)
# A network's NetworkInfo, as the console lays it out (little-endian).
NI_SIZE = 0x480
NI_SESSION = 0x10
NI_POLICY = 0x62
NI_NODE_MAX = 0x66
NI_NODE_COUNT = 0x67
NI_NODES = 0x68
NI_ADV_SIZE = 0x26A
NODE_SIZE = 0x40

# ssbu-recomp's games speak a protocol of their own, "SU" version 1 (ssbu-recomp/runtime/net_relay.c
# describes it), on a port of its own: the same lobby, other numbers and layouts.
SU_PROTOCOL = 1
SU_PORT = 39500
(SU_JOIN, SU_WELCOME, SU_REJECT, SU_PING, SU_PONG, SU_DATA, SU_LEAVE, SU_LIST, SU_LISTING, SU_LIST_END,
 SU_PEERS, SU_PUNCH, SU_DIRECT, SU_BCAST) = range(1, 15)
SU_JOIN_SIZE = 4 + 8 + 2 * NAME_LEN + BUILD_LEN + 4 + 2 + 1
SU_PING_SIZE = 4 + 8 + 8 + 1
SU_PEER = struct.Struct(">I32sIHIHQ")   # a PEERS entry (only the players introduced)
SU_PEERS_AGAIN = 5.0   # seconds: its PINGs don't say which PEERS came last, so the last is sent again this often
# The room browser: every room of the lobby code, whatever its game's version or mods. A game's build
# id has its version and a hash of its mods, so players of another version or mod set never share a
# room; their rooms are listed all the same, with the host's build and mods, and the game says why
# it can't join one (ssbu-recomp/runtime/net_relay.c room_why).
SU_ROOMS, SU_ROOM_INFO, SU_ROOMS_END = 17, 18, 19
ROOMS_MAX = 32        # rooms in a ROOMS answer, at most
MODS_MAX = 32         # a game's mod set (its JOIN ends with "MS" and it): mods at most...
MOD_NAME = 31         # ...and a mod's name, bytes at most


def header(kind, version=PROTOCOL):
    return b"SN" + bytes((version, kind))


def su_header(kind):
    return b"SU" + bytes((SU_PROTOCOL, kind))


def mods_block(raw):
    """A game's mod set (count u8, count x (hash u32, name length u8, name)) cut to its own length,
    or None when it is not one."""
    if not raw or raw[0] > MODS_MAX:
        return None
    o = 1
    for _ in range(raw[0]):
        if o + 5 > len(raw) or raw[o + 4] > MOD_NAME or o + 5 + raw[o + 4] > len(raw):
            return None
        o += 5 + raw[o + 4]
    return bytes(raw[:o])


class Wire:
    """A protocol the lobby speaks, on a UDP port of its own."""
    def __init__(self, name, version, data, bcast):
        self.name, self.magic, self.version = name, name.encode(), version
        self.data, self.bcast = data, bcast   # (its DATA and BCAST types)
        self.transport = None

    def header(self, kind, version=None):
        return self.magic + bytes((self.version if version is None else version, kind))


class Endpoint(asyncio.DatagramProtocol):
    """A wire's UDP port: what comes in goes to the lobby."""
    def __init__(self, lobby, wire):
        self.lobby, self.wire = lobby, wire

    def connection_made(self, transport):
        self.wire.transport = transport

    def error_received(self, exc):
        self.lobby.drops["socket error"] += 1

    def datagram_received(self, p, at):
        self.lobby.received(self.wire, p, at)


def text(raw):
    return raw.split(b"\0", 1)[0].decode("utf-8", "replace")


def printable(s):
    return "".join(ch if ch.isprintable() else "?" for ch in s)


def ip_text(ip):
    return f"10.13.{ip >> 8 & 255}.{ip & 255}"


def address_number(host):
    return struct.unpack(">I", socket.inet_aton(host))[0]


def ldn_packet(src, dst, kind, payload, head=None):
    """A local wireless message as a player's game would send it (head: the DATA header of its wire)."""
    return ((head or header(DATA)) + struct.pack(">IIHH", src, dst, LDN_PORT, LDN_PORT) +
            bytes((ord("L"), kind, 0, 0)) + payload)


def node_name(info, i=0):
    o = NI_NODES + NODE_SIZE * i + 0xC
    return printable(text(info[o:o + NAME_LEN]))


def network_nodes(info, owner_ip):
    """The addresses of a network's stations (node 0, the host, left out), or None when the
    NetworkInfo is not one a game makes or its host is not owner_ip."""
    count, most = info[NI_NODE_COUNT], info[NI_NODE_MAX]
    if not 1 <= count <= most <= 8 or struct.unpack_from("<H", info, NI_ADV_SIZE)[0] > 0x180:
        return None
    nodes = []
    for i in range(8):
        o = NI_NODES + NODE_SIZE * i
        if not info[o + 0xB]:
            if i == 0:
                return None
            continue
        ip = struct.unpack_from("<I", info, o)[0]
        if ip & 0xFFFF0000 != SUBNET or (i == 0 and ip != owner_ip) or ip in nodes:
            return None
        nodes.append(ip)
    return frozenset(nodes[1:]) if len(nodes) == count else None


class Bucket:
    """A token bucket: rate a second, up to burst saved."""
    __slots__ = ("rate", "burst", "level", "at")

    def __init__(self, rate, burst, now):
        self.rate, self.burst, self.level, self.at = rate, burst, burst, now

    def take(self, now, n=1.0):
        self.level = min(self.burst, self.level + (now - self.at) * self.rate)
        self.at = now
        if self.level < n:
            return False
        self.level -= n
        return True


class Client:
    def __init__(self, at, nonce, ip, name, part, now, o, wire=None):
        self.at, self.nonce, self.ip, self.name, self.part = at, nonce, ip, name, part
        self.wire = wire       # the protocol it speaks (None: SN)
        self.build, self.code = b"", ""   # (its part: wire, build and lobby code)
        self.code_key = None   # (wire, lobby code): its part and the parts of other builds with that code
        self.mods = None       # (SU) its mod set (None: its game does not say)
        self.cookie = secrets.randbits(64)   # proves the address is real (it came back in a PING)
        self.verified = False
        self.seen = now
        self.ping = 0xFFFF
        self.claim = None      # the session this player asked to join (its LM_JOIN)
        self.listing = None    # the network this player hosts
        # direct connections (from its extended PINGs)
        self.ext = False           # its game can connect directly
        self.direct_ok = False     # ...and its player allows it (it shows the player's address)
        self.lan = None            # (address, port) on its own network
        self.direct_count = 0      # the players it reaches directly now
        self.peers_version = 0     # of the last PEERS it was sent
        self.peers_sent = b""      # ...and its entries
        self.peers_at = -1e9
        self.introduced_count = 0
        self.intro_rooms = {}      # session -> when it was first introduced in that room (the last hour's)
        self.packets = Bucket(o.packet_rate, o.packet_rate * 2, now)
        self.bytes = Bucket(o.byte_rate, o.byte_rate * 2, now)
        self.joins = Bucket(2, 8, now)
        self.lists = Bucket(4, 8, now)


class Listing:
    """A network a player hosts, from its latest advert."""
    def __init__(self, owner, session, now):
        self.owner, self.session, self.created = owner, session, now
        self.info, self.nodes, self.updated = b"", frozenset(), 0.0
        self.distance = None   # the host's ping to the lobby in 100 ms steps, when the room opened
        self.pair_secrets = {}   # frozenset of two Clients -> their secret for direct packets
        self.talked = set()      # (sender, receiver): players that sent each other game packets here
        self.introduced = set()  # the players last told about each other

    @property
    def count(self):
        return self.info[NI_NODE_COUNT]

    @property
    def open(self):
        return self.info[NI_POLICY] != 1 and self.info[NI_NODE_COUNT] < self.info[NI_NODE_MAX]


def options(**overrides):
    """The lobby's settings (main's command line; tests override some). su_port: SU's port (None: SU
    not served, 0: any free port)."""
    o = argparse.Namespace(port=DEFAULT_PORT, su_port=None, bind="0.0.0.0", verbose=False, banlist=None,
                           max_clients=2000, max_pending=1000, per_ip=8, gate_rate=5.0, gate_burst=20,
                           packet_rate=600.0, byte_rate=400_000.0, max_data=1500,
                           client_timeout=10.0, pending_timeout=3.0, listed_for=3.5, listing_timeout=10.0,
                           tick=1.0, stats=60.0)
    for k, v in overrides.items():
        setattr(o, k, v)
    return o


class Lobby:
    def __init__(self, o):
        self.o = o
        self.sn = Wire("SN", PROTOCOL, DATA, BCAST)
        self.su = Wire("SU", SU_PROTOCOL, SU_DATA, SU_BCAST)
        self.by_addr = {}       # (wire, (host, port)) -> Client
        self.by_ip = {}         # virtual address -> Client
        self.parts = {}         # (wire name, build, lobby code) -> set of Client
        self.part_rooms = {}    # (wire name, build, lobby code) -> set of Listing
        self.code_rooms = {}    # (wire name, lobby code) -> set of Listing, every build's
        self.listings = {}      # session id -> Listing
        self.per_ip = collections.Counter()   # host -> verified players from there
        self.pending = 0        # players not verified yet
        self.gates = {}         # host -> Bucket: answers to strangers' JOINs
        self.next_ip = SUBNET + 2
        self.dirty = set()           # rooms whose players may need a new PEERS
        self.dirty_clients = set()   # ...and players
        self.banned, self.ban_stamp, self.ban_checked = [], None, 0.0
        self.counts = collections.Counter()
        self.drops = collections.Counter()
        self.max_data = 0

    # ---- plumbing ---------------------------------------------------------------------
    def close_ports(self):
        for wire in (self.sn, self.su):
            if wire.transport is not None:
                wire.transport.close()

    def log(self, *what):
        if self.o.verbose:
            self.say(*what)

    @staticmethod
    def say(*what):
        print(time.strftime("%Y-%m-%d %H:%M:%S"), *what, flush=True)

    def send(self, data, at, wire=None):
        """data to the address at, from the port of wire (SN's by default)."""
        self.counts["out packets"] += 1
        self.counts["out bytes"] += len(data)
        (wire or self.sn).transport.sendto(data, at)

    def to(self, c, data):
        self.send(data, c.at, c.wire)

    def data_header(self, c):
        """The header of a DATA to c."""
        wire = c.wire or self.sn
        return wire.header(wire.data)

    def reject(self, wire, at, nonce, reason, why, version=None):
        """A REJECT (SU's has no reason byte)."""
        reason = b"" if wire is self.su else bytes((reason,))
        self.send(wire.header(REJECT, version) + nonce + reason + why.encode()[:63].ljust(64, b"\0"), at, wire)

    def gate(self, at, now):
        """Whether a stranger at this address may get an answer now (rate-limited: a lobby must not
        answer floods from spoofed addresses)."""
        b = self.gates.get(at[0])
        if b is None:
            b = self.gates[at[0]] = Bucket(self.o.gate_rate, self.o.gate_burst, now)
        return b.take(now)

    def is_banned(self, host):
        if not self.banned:
            return False
        try:
            a = ipaddress.ip_address(host)
        except ValueError:
            return False
        return any(a in net for net in self.banned)

    def load_banlist(self, now):
        self.ban_checked = now
        path = self.o.banlist
        if not path:
            return
        try:
            stamp = os.stat(path).st_mtime
        except OSError:
            stamp = None
        if stamp == self.ban_stamp:
            return
        self.ban_stamp, banned = stamp, []
        if stamp is not None:
            with open(path, encoding="utf-8", errors="replace") as f:
                for line in f:
                    line = line.split("#", 1)[0].strip()
                    if line:
                        try:
                            banned.append(ipaddress.ip_network(line, strict=False))
                        except ValueError:
                            self.say(f"banlist: not an address: {printable(line)}")
        self.banned = banned
        self.say(f"banlist: {len(banned)} entr{'y' if len(banned) == 1 else 'ies'}")
        for c in list(self.by_addr.values()):
            if self.is_banned(c.at[0]):
                self.drop(c, "banned")

    def new_ip(self):
        """A free virtual address, handed out in turn (a freed one is not reused at once)."""
        for _ in range(0x10000):
            ip = self.next_ip
            self.next_ip = SUBNET + 2 if ip >= SUBNET + 0xFFFE else ip + 1
            if ip & 0xFF not in (0, 255) and ip not in self.by_ip:
                return ip
        return None

    # ---- who is in which network -----------------------------------------------------
    def members(self, listing):
        """The players of a network: its host, and every station it lists that asked to join it."""
        out = [listing.owner]
        for ip in listing.nodes:
            c = self.by_ip.get(ip)
            if c is not None and c.claim == listing.session:
                out.append(c)
        return out

    def network_of(self, c):
        if c.listing is not None:
            return c.listing
        listing = self.listings.get(c.claim) if c.claim else None
        return listing if listing is not None and c.ip in listing.nodes else None

    def close(self, listing, packet=None):
        """A network is gone: tell its players (with the host's own LM_GONE if there is one)."""
        if packet is None:
            packet = ldn_packet(listing.owner.ip, BROADCASTS[0], LM_GONE, listing.session, self.data_header(listing.owner))
        self.dirty_clients.update(self.members(listing), listing.introduced)
        self.dirty.discard(listing)
        for m in self.members(listing):
            if m is not listing.owner:
                self.to(m, packet)
                m.claim = None
        self.listings.pop(listing.session, None)
        for index, key in ((self.part_rooms, listing.owner.part), (self.code_rooms, listing.owner.code_key)):
            rooms = index.get(key)
            if rooms is not None:
                rooms.discard(listing)
                if not rooms:
                    del index[key]
        if listing.owner.listing is listing:
            listing.owner.listing = None
        self.log(f"room of {printable(listing.owner.name)} closed")

    def drop(self, c, why):
        if c.listing is not None:
            self.close(c.listing)
        if c.claim:   # tell its host at once rather than after the host's timeout
            listing = self.listings.get(c.claim)
            if listing is not None and c.ip in listing.nodes:
                self.to(listing.owner, ldn_packet(c.ip, listing.owner.ip, LM_LEAVE, c.claim, self.data_header(listing.owner)))
                self.dirty.add(listing)
        self.dirty_clients.discard(c)
        self.by_addr.pop((c.wire, c.at), None)
        self.by_ip.pop(c.ip, None)
        players = self.parts.get(c.part)
        if players is not None:
            players.discard(c)
            if not players:
                del self.parts[c.part]
        if c.verified:
            self.per_ip[c.at[0]] -= 1
            if self.per_ip[c.at[0]] <= 0:
                del self.per_ip[c.at[0]]
        else:
            self.pending -= 1
        self.log(f"{printable(c.name)} ({ip_text(c.ip)}) left: {why}")

    def verify(self, c):
        if not c.verified:
            c.verified = True
            self.pending -= 1
            self.per_ip[c.at[0]] += 1

    # ---- packets ------------------------------------------------------------------------
    def received(self, wire, p, at):
        self.counts["in packets"] += 1
        self.counts["in bytes"] += len(p)
        if len(p) < 4 or p[:2] != wire.magic:
            return
        try:
            now = time.monotonic()
            (self.packet if wire is self.sn else self.su_packet)(p, at, now)
            if self.dirty or self.dirty_clients:
                self.flush(now)
        except Exception as e:   # (one bad packet never stops the lobby)
            self.drops["error"] += 1
            self.say(f"error on a packet from {at[0]}: {e!r}")

    def packet(self, p, at, now):
        """A packet of WindowSmash's games (SN)."""
        kind = p[3]
        if p[2] != PROTOCOL:   # another version of the game's netplay: say so
            if kind == JOIN and len(p) >= V1_JOIN_SIZE and self.gate(at, now):
                self.reject(self.sn, at, p[4:12], 2, f"different netplay version (lobby {PROTOCOL}, you {p[2]})", p[2])
            return
        c = self.by_addr.get((self.sn, at))
        if kind == JOIN:
            self.on_join(p, at, c, now)
        elif c is None:
            return
        elif kind == PING:
            if len(p) < PING_SIZE or p[4:12] != c.nonce or struct.unpack_from(">Q", p, 20)[0] != c.cookie:
                return
            self.verify(c)
            c.seen = now
            c.ping = struct.unpack_from(">H", p, 28)[0]
            if len(p) >= PING_EXT_SIZE:
                self.on_ping_ext(c, p, now)
            self.to(c, header(PONG) + p[12:20] + struct.pack(">HHB", min(len(self.parts.get(c.part, ())), 0xFFFF),
                                                             min(len(self.part_rooms.get(c.part, ())), 0xFFFF), 1))
        elif kind == LEAVE:
            if len(p) >= 12 and p[4:12] == c.nonce:
                self.drop(c, "left")
        elif kind == LIST:
            if len(p) < 16 or struct.unpack_from(">Q", p, 4)[0] != c.cookie:
                return
            self.verify(c)
            c.seen = now
            if not c.lists.take(now):
                self.drops["list rate"] += 1
                return
            self.on_list(c, p[12:16], now)
        elif kind == DATA:
            self.on_data(c, p, now)
        elif kind == BCAST:
            self.on_bcast(c, p, now)

    def on_data(self, c, p, now):
        if len(p) < 16 or not self.admit(c, p, 4, now):
            return
        src, dst, sport, dport = struct.unpack_from(">IIHH", p, 4)
        if src != c.ip:
            self.drops["not its address"] += 1
        elif sport == LDN_PORT or dport == LDN_PORT:
            self.on_ldn(c, p, dst, sport, dport, now)
        else:
            self.on_game(c, p, dst)

    def admit(self, c, p, at, now):
        """Whether a verified player's datagram (its DATA body at offset at) may be carried."""
        if not c.verified:
            return False
        c.seen = now
        size = len(p) - at - 12
        if size > self.o.max_data:
            self.drops["too big"] += 1
            return False
        self.max_data = max(self.max_data, size)
        if not c.packets.take(now) or not c.bytes.take(now, len(p)):
            self.drops["rate"] += 1
            return False
        return True

    def on_join(self, p, at, c, now):
        if len(p) < JOIN_SIZE:
            return
        nonce = p[4:12]
        room, name = text(p[12:12 + NAME_LEN]), text(p[12 + NAME_LEN:12 + 2 * NAME_LEN])
        build = p[12 + 2 * NAME_LEN:JOIN_SIZE].rstrip(b"\0")
        c = self.join(self.sn, at, c, nonce, room, name, build, now)
        if c is not None:
            self.to(c, header(WELCOME) + nonce + struct.pack(">IQB", c.ip, c.cookie, 1 | 2) + b"\0")   # (2: introduces peers)

    def join(self, wire, at, c, nonce, room, name, build, now):
        """The player a JOIN comes from (c: the one at its address, if any): a new one, one back
        from a new port, or the same again; None when it is turned away."""
        if self.is_banned(at[0]):
            self.drops["banned"] += 1
            return None
        if c is not None and c.nonce != nonce:   # a new game from the same place
            self.drop(c, "restarted")
            c = None
        part = (wire.name, build, room)
        if c is None:   # a player back from a new place (its router changed its port)?
            for other in self.parts.get(part, ()):
                if other.nonce == nonce and other.verified:
                    del self.by_addr[wire, other.at]
                    self.per_ip[other.at[0]] -= 1
                    if self.per_ip[other.at[0]] <= 0:
                        del self.per_ip[other.at[0]]
                    self.per_ip[at[0]] += 1
                    other.at = at
                    self.by_addr[wire, at] = c = other
                    self.mark(c)   # (the players it was introduced to need its new port)
                    break
        if c is None:   # a new player
            if not self.gate(at, now):
                self.drops["join rate"] += 1
                return None
            if wire is self.sn and build.upper() != REQUIRED_BUILD:   # (another game version: refused, saying why)
                self.drops["game version"] += 1
                self.log(f"refused {printable(name)} from {at[0]}:{at[1]}: build {build[:8].decode('ascii', 'replace')}, "
                         "not 13.0.5's")
                self.reject(wire, at, nonce, 4, NOT_13)
                return None
            why = ("the lobby is full" if len(self.by_addr) >= self.o.max_clients or self.pending >= self.o.max_pending
                   else f"too many players from your address ({self.o.per_ip})" if self.per_ip[at[0]] >= self.o.per_ip
                   else None)
            ip = self.new_ip() if why is None else None
            if ip is None:
                self.reject(wire, at, nonce, 1, why or "the lobby is full")
                return None
            c = Client(at, nonce, ip, name, part, now, self.o, wire)
            c.build, c.code, c.code_key = build, room, (wire.name, room)
            self.by_addr[wire, at], self.by_ip[ip] = c, c
            self.parts.setdefault(part, set()).add(c)
            self.pending += 1
            self.log(f"{printable(name)} joined lobby {printable(room)!r} ({wire.name}, build "
                     f"{build[:8].decode('ascii', 'replace')}) as {ip_text(ip)} from {at[0]}:{at[1]}")
        c.seen = now
        return c

    # ---- ssbu-recomp's games (SU) -------------------------------------------------------
    # The same lobby in SU's numbers and layouts, with WindowSmash's limits, rates and timeouts:
    # a player is verified (its cookie came back) before anything of its is carried, and players
    # are introduced to each other only once their games talk.
    def su_packet(self, p, at, now):
        """A packet of ssbu-recomp's games (SU)."""
        if p[2] != SU_PROTOCOL:   # (another version: nothing it says is known)
            return
        kind = p[3]
        c = self.by_addr.get((self.su, at))
        if kind == SU_JOIN:
            self.su_join(p, at, c, now)
            return
        if c is None:
            return
        if kind == SU_DATA:
            self.on_data(c, p, now)
            return
        if kind == SU_BCAST:
            self.on_bcast(c, p, now)
            return
        if len(p) < 12 or struct.unpack_from(">Q", p, 4)[0] != c.cookie:   # (the rest starts with the cookie)
            return
        self.verify(c)
        c.seen = now
        if kind == SU_PING:
            if len(p) < SU_PING_SIZE:
                return
            c.direct_count = min(p[20], ROOM_MAX - 1)
            self.to(c, su_header(SU_PONG) + p[12:20] + struct.pack(">HH", min(len(self.parts.get(c.part, ())), 0xFFFF),
                                                                  min(len(self.part_rooms.get(c.part, ())), 0xFFFF)))
            if c.peers_version and now - c.peers_at >= SU_PEERS_AGAIN:   # (in case the last was lost)
                self.sync(c, now, again=True)
        elif kind == SU_LEAVE:
            self.drop(c, "left")
        elif kind in (SU_LIST, SU_ROOMS):
            if len(p) < 16:
                return
            if not c.lists.take(now):
                self.drops["list rate"] += 1
                return
            if kind == SU_LIST:   # (flags 1: the rooms of every build of its lobby code)
                self.on_list(c, p[12:16], now, len(p) > 16 and bool(p[16] & 1))
            else:
                self.on_rooms(c, p[12:16], now)

    def su_join(self, p, at, c, now):
        """JOIN: nonce, lobby code, name, build, LAN address and port, flags (1: direct connections),
        and from a newer game "MS" and its mod set."""
        if len(p) < SU_JOIN_SIZE:
            return
        nonce = p[4:12]
        code, name = text(p[12:12 + NAME_LEN]), text(p[12 + NAME_LEN:12 + 2 * NAME_LEN]) or "Player"
        o = 12 + 2 * NAME_LEN + BUILD_LEN
        build = p[12 + 2 * NAME_LEN:o].rstrip(b"\0")
        lan_ip, lan_port = struct.unpack_from(">IH", p, o)
        c = self.join(self.su, at, c, nonce, code, name, build, now)
        if c is None:
            return
        if p[SU_JOIN_SIZE:SU_JOIN_SIZE + 2] == b"MS":   # (an older game sends no mods)
            c.mods = mods_block(p[SU_JOIN_SIZE + 2:])
        direct_ok, lan = bool(p[o + 6] & 1), ((lan_ip, lan_port) if lan_ip and lan_port else None)
        if not c.ext or direct_ok != c.direct_ok or lan != c.lan:   # (every SU game can connect directly)
            c.ext, c.direct_ok, c.lan = True, direct_ok, lan
            self.mark(c)
        self.to(c, su_header(SU_WELCOME) + nonce + struct.pack(">IQ", c.ip, c.cookie))

    # ---- rooms ----------------------------------------------------------------------------
    def on_list(self, c, seq, now, every=False):
        """The rooms this player's Rooms List shows: open ones first, fuller ones first, then the
        ones whose host is closest to the lobby (in 100 ms steps, taken when the room opened: the
        list must not reorder under the player's cursor from one scan to the next), then the oldest.
        every (SU): the rooms of every build of its lobby code, its own build's first, each LISTING
        ending with its host's build."""
        pool = self.code_rooms.get(c.code_key, ()) if every else self.part_rooms.get(c.part, ())
        rooms = sorted((r for r in pool if r.owner is not c and now - r.updated <= self.o.listed_for),
                       key=lambda r: (r.owner.part != c.part, *self.room_order(r)))[:LIST_MAX]
        su = c.wire is self.su
        for r in rooms:
            self.to(c, (su_header(SU_LISTING) if su else header(LISTING)) + seq + struct.pack(">I", r.owner.ip) + r.info +
                    (r.owner.build[:BUILD_LEN].ljust(BUILD_LEN, b"\0") if every else b""))
        self.to(c, (su_header(SU_LIST_END) if su else header(LIST_END)) + seq + bytes((len(rooms),)))

    @staticmethod
    def room_order(r):
        return not r.open, -r.count, 10 if r.distance is None else r.distance, r.created

    def on_rooms(self, c, seq, now):
        """(SU) The room browser: every room of its lobby code, its own build's first, then as a
        Rooms List; each with its host's address and name, players, build and mods, and flags
        (1 open, 2 its own, 4 its host's game says its mods)."""
        rooms = sorted((r for r in self.code_rooms.get(c.code_key, ()) if now - r.updated <= self.o.listed_for),
                       key=lambda r: (r.owner.part != c.part, *self.room_order(r)))[:ROOMS_MAX]
        for r in rooms:
            h = r.owner
            flags = (1 if r.open else 0) | (2 if h is c else 0) | (4 if h.mods is not None else 0)
            self.to(c, su_header(SU_ROOM_INFO) + seq + struct.pack(">IBBB", h.ip, r.count, r.info[NI_NODE_MAX], flags) +
                    h.name.encode()[:NAME_LEN].ljust(NAME_LEN, b"\0") + h.build[:BUILD_LEN].ljust(BUILD_LEN, b"\0") +
                    (h.mods or b"\0"))
        self.to(c, su_header(SU_ROOMS_END) + seq + bytes((len(rooms),)))

    def on_ldn(self, c, p, dst, sport, dport, now):
        """Local wireless's own messages: only the ones its protocol has, between the players they
        concern."""
        if sport != LDN_PORT or dport != LDN_PORT or len(p) < 20 or p[16] != ord("L"):
            self.drops["ldn"] += 1
            return
        kind, body = p[17], p[20:]
        if dst in BROADCASTS:
            if kind == LM_ADVERTISE:
                self.on_advertise(c, p, body, now)
            elif kind == LM_GONE and c.listing is not None and body[:16] == c.listing.session:
                self.close(c.listing, p)
            else:
                self.drops["ldn"] += 1
            return
        b = self.by_ip.get(dst)
        if b is None or b is c or b.part != c.part or not b.verified:
            self.drops["ldn"] += 1
            return
        if kind == LM_JOIN:   # asking to join b's network
            listing = b.listing
            if listing is None or body[:16] != listing.session:
                self.drops["ldn"] += 1
                return
            if not c.joins.take(now):
                self.drops["join rate"] += 1
                return
            if c.listing is not None:   # (a host cannot be joining: its network is gone)
                self.close(c.listing)
            self.mark(c)
            c.claim = listing.session
            self.dirty.add(listing)
        elif kind in (LM_ACCEPT, LM_REJECT, LM_KICK):   # the host answering one who asked
            if c.listing is None or b.claim != c.listing.session:
                self.drops["ldn"] += 1
                return
        elif kind in (LM_KEEP, LM_LEAVE):   # a station to its host
            if not c.claim or body[:16] != c.claim or b.listing is None or b.listing.session != c.claim:
                self.drops["ldn"] += 1
                return
            if kind == LM_LEAVE:
                self.to(b, p)
                c.claim = None
                self.dirty.add(b.listing)
                self.dirty_clients.add(c)
                return
        else:
            self.drops["ldn"] += 1
            return
        self.to(b, p)

    def on_advertise(self, c, p, body, now):
        if len(body) < NI_SIZE:
            self.drops["bad advert"] += 1
            return
        info = bytes(body[:NI_SIZE])
        nodes = network_nodes(info, c.ip)
        if nodes is None:
            self.drops["bad advert"] += 1
            return
        session = info[NI_SESSION:NI_SESSION + 16]
        listing = self.listings.get(session)
        if listing is not None and listing.owner is not c:
            self.drops["session taken"] += 1
            return
        if listing is None:   # a new network (one at a time: it replaces the host's last)
            if c.listing is not None:
                self.close(c.listing)
            self.mark(c)   # (it was perhaps a station of another)
            c.claim = None
            listing = c.listing = self.listings[session] = Listing(c, session, now)
            self.part_rooms.setdefault(c.part, set()).add(listing)
            self.code_rooms.setdefault(c.code_key, set()).add(listing)
            self.log(f"{printable(c.name)} opened a room ({info[NI_NODE_MAX]} players at most)")
        if listing.distance is None and c.ping != 0xFFFF:
            listing.distance = min(c.ping, 1000) // 100
        before = self.members(listing)
        if nodes != listing.nodes:
            self.dirty.add(listing)
        listing.info, listing.nodes, listing.updated = info, nodes, now
        # (to whoever is in it, and whoever just left it, so they learn it at once)
        for m in {*before, *self.members(listing)}:
            if m is not c:
                self.to(m, p)

    def on_game(self, c, p, dst):
        """The game's own traffic: only within a network, between its players."""
        listing = self.network_of(c)
        if listing is None:
            self.drops["not in a room"] += 1
            return
        members = self.members(listing)
        if dst in BROADCASTS:
            for m in members:
                if m is not c:
                    self.to(m, p)
                    self.counts["game relayed"] += 1
            return
        m = self.by_ip.get(dst)
        if m is None or m is c or m not in members:
            self.drops["not in the room"] += 1
            return
        self.to(m, p)
        self.counts["game relayed"] += 1
        if (c, m) not in listing.talked:   # (both ways: their games really play together - introduce them)
            listing.talked.add((c, m))
            if (m, c) in listing.talked:
                self.dirty.add(listing)

    def on_bcast(self, c, p, now):
        """A broadcast to the room's players other than the ones listed (the sender reaches those
        directly): count u8, count x address u32, then a DATA body."""
        if len(p) < 5:
            return
        n = p[4]
        at = 5 + 4 * n
        if n >= ROOM_MAX or len(p) < at + 12:
            self.drops["bad broadcast"] += 1
            return
        if not self.admit(c, p, at, now):
            return
        skip = set(struct.unpack_from(f">{n}I", p, 5))
        src, dst, sport, dport = struct.unpack_from(">IIHH", p, at)
        if src != c.ip:
            self.drops["not its address"] += 1
            return
        if dst not in BROADCASTS or sport == LDN_PORT or dport == LDN_PORT:
            self.drops["bad broadcast"] += 1
            return
        listing = self.network_of(c)
        if listing is None:
            self.drops["not in a room"] += 1
            return
        out = self.data_header(c) + p[at:]
        for m in self.members(listing):
            if m is not c and m.ip not in skip:
                self.to(m, out)
                self.counts["game relayed"] += 1

    # ---- direct connections -------------------------------------------------------------
    # The players of a room whose games can connect directly are introduced to each other: each
    # learns the other's address as the lobby sees it, and a secret for the two of them, which
    # every direct packet carries. Their games then punch through their routers and send the
    # room's traffic straight to each other, falling back to the lobby when that fails. A pair
    # is introduced only once both allow it and their games have sent each other game packets
    # through the lobby (they really play together: one that only joins rooms learns nobody's
    # address), and a player in INTRO_ROOMS rooms an hour at most.
    def on_ping_ext(self, c, p, now):
        have, lan_ip, lan_port, flags, ndirect = struct.unpack_from(">IIHBB", p, PING_SIZE)
        direct_ok, lan = bool(flags & 1), ((lan_ip, lan_port) if lan_ip and lan_port else None)
        if not c.ext or direct_ok != c.direct_ok or lan != c.lan:
            c.ext, c.direct_ok, c.lan = True, direct_ok, lan
            self.mark(c)
        c.direct_count = min(ndirect, ROOM_MAX - 1)
        if have != c.peers_version:   # (a PEERS lost on the way: again)
            self.sync(c, now, again=True)

    def mark(self, c):
        """c's room (if any) needs its players' PEERS looked at."""
        listing = self.network_of(c)
        if listing is not None:
            self.dirty.add(listing)
        self.dirty_clients.add(c)

    def flush(self, now):
        while self.dirty:
            listing = self.dirty.pop()
            if self.listings.get(listing.session) is not listing:
                continue   # (closed: close() queued its players)
            members = set(self.members(listing))
            gone = listing.introduced - members
            listing.introduced = members
            if gone:   # (one that comes back is introduced anew, with a new secret)
                listing.pair_secrets = {k: v for k, v in listing.pair_secrets.items() if not k & gone}
                listing.talked = {t for t in listing.talked if t[0] not in gone and t[1] not in gone}
            self.dirty_clients |= members | gone
        todo, self.dirty_clients = self.dirty_clients, set()
        for c in todo:
            self.sync(c, now)

    def room_ok(self, c, session, now):
        if c.intro_rooms and now - min(c.intro_rooms.values()) > 3600:
            c.intro_rooms = {k: t for k, t in c.intro_rooms.items() if now - t <= 3600}
        return session in c.intro_rooms or len(c.intro_rooms) < INTRO_ROOMS

    def pair_secret(self, listing, a, b, now):
        """The secret of two players of a room, or 0 when they are not to be introduced."""
        if not (a.direct_ok and b.direct_ok):
            return 0
        key = frozenset((a, b))
        s = listing.pair_secrets.get(key)
        if s is None:
            if not (a.ext and b.ext and (a, b) in listing.talked and (b, a) in listing.talked and
                    self.room_ok(a, listing.session, now) and self.room_ok(b, listing.session, now)):
                return 0
            s = listing.pair_secrets[key] = secrets.randbits(64) | 1
            for x in (a, b):
                x.intro_rooms.setdefault(listing.session, now)
        return s

    def sync(self, c, now, again=False):
        """Send c the players of its room (none: an empty list), when they changed (or again)."""
        if not c.ext or self.by_addr.get((c.wire, c.at)) is not c:
            return
        listing = self.network_of(c)
        peers = sorted((m for m in self.members(listing) if m is not c), key=lambda m: m.ip) if listing else []
        su = c.wire is self.su
        entries, introduced = [], 0
        for m in peers:
            secret = self.pair_secret(listing, c, m, now)
            pub = lan = (0, 0)
            if secret:
                introduced += 1
                pub = (address_number(m.at[0]), m.at[1])
                if m.at[0] == c.at[0] and m.lan and m.lan != pub:   # (behind the same router: its address there too)
                    lan = m.lan
            if su:   # (SU lists only the players introduced)
                if secret:
                    entries.append(SU_PEER.pack(m.ip, m.name.encode()[:NAME_LEN], *pub, *lan, secret))
                continue
            flags = (1 if secret else 0) | (0 if m.direct_ok else 2 if m.ext else 4)   # (2: it turned them off, 4: an older game)
            entries.append(PEER.pack(m.ip, m.name.encode()[:NAME_LEN], flags, *pub, *lan, secret))
        entries = b"".join(entries)
        if entries == c.peers_sent and not again:
            return
        if now - c.peers_at < PEERS_GAP:
            self.dirty_clients.add(c)   # (later: a room changing fast sends no flood)
            return
        if entries != c.peers_sent:
            c.peers_version = (c.peers_version + 1) & 0xFFFFFFFF or 1
            c.peers_sent = entries
        c.peers_at = now
        c.introduced_count = introduced
        if su:
            self.to(c, su_header(SU_PEERS) + struct.pack(">IB", c.peers_version, introduced) + entries)
        else:
            self.to(c, header(PEERS) + struct.pack(">QIB", c.cookie, c.peers_version, len(peers)) + entries)

    # ---- housekeeping -------------------------------------------------------------------
    def sweep(self, now):
        for c in list(self.by_addr.values()):
            if now - c.seen > (self.o.client_timeout if c.verified else self.o.pending_timeout):
                self.drop(c, "timed out")
        for listing in list(self.listings.values()):
            if now - listing.updated > self.o.listing_timeout:
                self.close(listing)
        for host, b in list(self.gates.items()):
            if now - b.at > 60:
                del self.gates[host]
        if now - self.ban_checked >= 5:
            self.load_banlist(now)
        self.flush(now)

    def report(self, secs):
        c, n = self.counts, len(self.by_addr)
        drops = ", ".join(f"{k} {v}" for k, v in sorted(self.drops.items())) or "none"
        paths = sum(p.introduced_count for p in self.by_addr.values())
        direct = sum(min(p.direct_count, p.introduced_count) for p in self.by_addr.values())
        su = sum(1 for p in self.by_addr.values() if p.wire is self.su)
        self.say(f"{n} player{'' if n == 1 else 's'} ({self.pending} connecting, {su} on SU) in {len(self.parts)} "
                 f"lobb{'y' if len(self.parts) == 1 else 'ies'}, {len(self.listings)} room{'' if len(self.listings) == 1 else 's'}; "
                 f"in {c['in packets'] / secs:.0f}/s {c['in bytes'] / secs / 1024:.1f} KB/s, "
                 f"out {c['out packets'] / secs:.0f}/s {c['out bytes'] / secs / 1024:.1f} KB/s; "
                 f"game relayed {c['game relayed'] / secs:.0f}/s; direct paths {direct} of {paths}; "
                 f"largest {self.max_data} B; dropped: {drops}")
        self.counts.clear()
        self.drops.clear()
        self.max_data = 0

    async def run(self):
        self.load_banlist(time.monotonic())
        last_report = time.monotonic()
        while True:
            await asyncio.sleep(self.o.tick)
            now = time.monotonic()
            self.sweep(now)
            if self.o.stats and now - last_report >= self.o.stats:
                self.report(now - last_report)
                last_report = now


async def serve(o, make=None):
    """A lobby (make(o): Lobby's own by default) on its ports: (SN's transport, the lobby)."""
    loop = asyncio.get_running_loop()
    lobby = (make or Lobby)(o)
    try:
        transport, _ = await loop.create_datagram_endpoint(lambda: Endpoint(lobby, lobby.sn), local_addr=(o.bind, o.port))
        if o.su_port is not None:
            await loop.create_datagram_endpoint(lambda: Endpoint(lobby, lobby.su), local_addr=(o.bind, o.su_port))
    except BaseException:
        lobby.close_ports()
        raise
    return transport, lobby


async def main(o):
    transport, lobby = await serve(o)
    Lobby.say(f"SSBU netplay lobby on UDP {o.bind}:{transport.get_extra_info('sockname')[1]} (protocol {PROTOCOL})")
    if lobby.su.transport is not None:
        Lobby.say(f"and for ssbu-recomp's games on UDP {o.bind}:{lobby.su.transport.get_extra_info('sockname')[1]} "
                  f"(protocol SU {SU_PROTOCOL})")
    task = asyncio.ensure_future(lobby.run())
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, task.cancel)
        except (NotImplementedError, AttributeError):   # (Windows: Ctrl+C raises KeyboardInterrupt)
            pass
    try:
        await task
    except asyncio.CancelledError:
        pass
    finally:
        lobby.close_ports()
        Lobby.say("stopped")


def probe(target, room, name, build, wait=3.0):
    """Join a lobby as a player would, print what it says, and leave."""
    host, _, port = target.rpartition(":")
    if not host or not port.isdigit():
        host, port = target, str(DEFAULT_PORT)
    try:
        addr = (socket.gethostbyname(host), int(port))
    except OSError as e:
        print(f"cannot find {host}: {e}")
        return 1
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(0.5)
    nonce = secrets.token_bytes(8)

    def answer(kinds, until):
        while time.monotonic() < until:
            try:
                data, src = s.recvfrom(65536)
            except (socket.timeout, ConnectionResetError):   # (Windows: "port unreachable" came back)
                return None
            if src == addr and data[:2] == b"SN" and data[3] in kinds:
                return data
        return None

    join = (header(JOIN) + nonce + room.encode()[:NAME_LEN].ljust(NAME_LEN, b"\0") +
            name.encode()[:NAME_LEN].ljust(NAME_LEN, b"\0") + build.encode()[:BUILD_LEN].ljust(BUILD_LEN, b"\0"))
    start = time.monotonic()
    welcome = None
    while welcome is None and time.monotonic() - start < wait:
        s.sendto(join, addr)
        got = answer((WELCOME, REJECT), time.monotonic() + 0.5)
        if got and got[3] == REJECT and got[4:12] == nonce:
            print(f"{target}: refused: {text(got[13:77])}")
            return 1
        if got and got[3] == WELCOME and got[4:12] == nonce and len(got) >= 25:
            welcome = got
    if welcome is None:
        print(f"{target} ({addr[0]}:{addr[1]}): no answer")
        return 1
    ip, cookie, flags = struct.unpack_from(">IQB", welcome, 12)
    sent = time.monotonic()
    s.sendto(header(PING) + nonce + struct.pack(">QQH", int(sent * 1000), cookie, 0xFFFF), addr)
    pong = answer((PONG,), time.monotonic() + wait)
    rtt = (time.monotonic() - sent) * 1000
    s.sendto(header(LIST) + struct.pack(">QI", cookie, 1), addr)
    rooms, until = [], time.monotonic() + wait
    while True:
        got = answer((LISTING, LIST_END), until)
        if got is None or got[3] == LIST_END:
            break
        if len(got) >= 12 + NI_SIZE:
            rooms.append(got[12:12 + NI_SIZE])
    s.sendto(header(LEAVE) + nonce, addr)
    if pong is None or len(pong) < 17:
        print(f"{target}: joined as {ip_text(ip)}, but no answer to a ping")
        return 1
    online, nrooms, flags = struct.unpack_from(">HHB", pong, 12)
    print(f"{target} ({addr[0]}:{addr[1]}): {'lobby' if flags & 1 else 'relay'} protocol {PROTOCOL}"
          f"{', direct connections' if welcome[24] & 2 else ''}, "
          f"ping {rtt:.0f} ms; lobby {room!r}, build {build or '(none)'}: {online} player{'' if online == 1 else 's'} "
          f"(this probe included), {nrooms} room{'' if nrooms == 1 else 's'}")
    for info in rooms:
        print(f"  room of {node_name(info)}: {info[NI_NODE_COUNT]}/{info[NI_NODE_MAX]} players"
              f"{'' if info[NI_POLICY] != 1 and info[NI_NODE_COUNT] < info[NI_NODE_MAX] else ' (closed)'}")
    return 0


def parse_args(argv=None, **defaults):
    d = options(**defaults)
    a = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    a.add_argument("--port", type=int, default=int(os.environ.get("SSBU_NET_PORT", d.port)), help="UDP port (default 39420)")
    a.add_argument("--su-port", type=int, default=d.su_port or 0,
                   help=f"UDP port for ssbu-recomp's games (protocol SU {SU_PROTOCOL}; 0: none; default %(default)s)")
    a.add_argument("--bind", default=d.bind, help="the address to listen on (default: all)")
    a.add_argument("--verbose", action="store_true", help="log players and rooms coming and going")
    a.add_argument("--banlist", help="a file of IP addresses or networks (one a line, # comments) to turn away; "
                                     "re-read when it changes")
    a.add_argument("--max-clients", type=int, default=d.max_clients, help="players at most (default %(default)s)")
    a.add_argument("--per-ip", type=int, default=d.per_ip, help="players at most from one IP address (default %(default)s)")
    a.add_argument("--packet-rate", type=float, default=d.packet_rate, help="a player's packets a second (default %(default)s)")
    a.add_argument("--byte-rate", type=float, default=d.byte_rate, help="a player's bytes a second (default %(default)s)")
    a.add_argument("--max-data", type=int, default=d.max_data, help="largest game datagram (default %(default)s)")
    a.add_argument("--stats", type=float, default=d.stats, help="seconds between statistics lines (0: none; default %(default)s)")
    a.add_argument("--probe", metavar="HOST[:PORT]", help="instead of serving: check the lobby at this address")
    a.add_argument("--room", default="smash", help="(--probe) the lobby code (default: the public lobby)")
    a.add_argument("--build", default=REQUIRED_BUILD.decode(), help="(--probe) the game build id to join with "
                   "(default: 13.0.5's, the one the lobby lets in)")
    args = a.parse_args(argv)
    for k, v in vars(args).items():
        setattr(d, k, v)
    d.su_port = d.su_port or None
    return d


if __name__ == "__main__":
    o = parse_args(su_port=SU_PORT)
    if o.probe:
        sys.exit(probe(o.probe, o.room, "probe", o.build))
    try:
        asyncio.run(main(o))
    except KeyboardInterrupt:
        pass
