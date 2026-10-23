"""Whether a compiled object is up to date with what it was built from: the Windows build
(tools/recomp.py) and the Linux one (tools/linux/build_linux.py). (The macOS build keeps a record
of its own: tools/mac/build_mac.py.)

An object is current when the key it was compiled with is this build's. The key is a SHA-256 over
  - the content of its sources (the generated C; on Linux also the C++ file that includes it),
  - the content of the runtime headers it is built with (header_files: for the generated C,
    recomp.h or module.h and what they include from runtime/),
  - the compiler: its path and what it prints for --version (compiler_id, once per build),
  - the whole compile command: optimisation level, instruction set, defines, include folders.
So another flag, header or compiler compiles the objects built with it again, and a file saved
again unchanged does not: the decision goes by content, never by date.

Each build folder keeps its record in objcache.json: each object's key, with the parts it is made
of (which say why an object is compiled again), and each source's SHA-256 with its size and time
(to the nanosecond): a source with the same size and time is not read again. An object's key is
recorded once it has compiled (Record.compiled) and forgotten before it is compiled again
(Record.forget), the record written whole each time (a new file, then os.replace): a build that
stops half-way, or a file that fails to compile, leaves no key for an object it did not make.
No record, or one that cannot be read: its objects are compiled again."""
import functools
import hashlib
import json
import os
import re
import subprocess
import threading
import time

RECORD = "objcache.json"
VERSION = 1
# A source written this recently is read again next time: a disk that keeps times to the second or
# two (FAT, exFAT, some shares) would give a file changed in the same second the same time.
RECENT_NS = 3 * 10**9
INCLUDE = re.compile(rb'^[ \t]*#[ \t]*include[ \t]*"([^"]+)"', re.M)

# why an object is compiled; the last four are worth a line in the build's log (summary)
NEW, CHANGED = "no object yet", "its source changed"
UNRECORDED = "no record of what they were built from"
COMPILER, COMMAND, HEADERS = "the compiler changed", "the compile command changed", "the runtime headers changed"
NOTABLE = (UNRECORDED, COMPILER, COMMAND, HEADERS)
PARTS = ("compiler", "command", "headers", "source")


def sha(*parts):
    h = hashlib.sha256()
    for p in parts:
        h.update(p.encode("utf-8", "surrogateescape") + b"\0")
    return h.hexdigest()


def file_sha(path, scan=()):
    """(SHA-256 of a file's content, which of the byte strings `scan` it holds)."""
    h, found, tail = hashlib.sha256(), set(), b""
    keep = max((len(s) for s in scan), default=1) - 1
    with open(path, "rb") as f:
        while True:
            chunk = f.read(1 << 20)
            if not chunk:
                break
            h.update(chunk)
            if scan:
                window = tail + chunk
                found |= {s for s in scan if s in window}
                tail = window[-keep:] if keep else b""
    return h.hexdigest(), found


def header_files(folder, *names):
    """The headers `names` (in folder) and each header they include with quotes, and so on: found
    beside the file including it, else in folder. A name found in neither is left out (a system
    header: the compiler's, which compiler_id stands for)."""
    found, todo = set(), [(folder, n) for n in names]
    while todo:
        here, name = todo.pop()
        path = next((p for p in (os.path.join(here, name), os.path.join(folder, name)) if os.path.isfile(p)), None)
        if path is None or os.path.normpath(path) in found:
            continue
        path = os.path.normpath(path)
        found.add(path)
        with open(path, "rb") as f:
            todo += [(os.path.dirname(path), m.decode("utf-8", "replace")) for m in INCLUDE.findall(f.read())]
    return sorted(found)


def digest_files(paths):
    """One SHA-256 over these files' names and content (headers: a few small files, read each build)."""
    return sha(*(f"{os.path.basename(p)}\0{file_sha(p)[0]}" for p in sorted(paths)))


@functools.lru_cache(maxsize=None)
def compiler_id(compiler):
    """The compiler's path and what it prints for --version: another compiler, or another version at
    the same path, compiles everything again. Run once per build. `compiler`: its path, or a tuple
    of the arguments that run it."""
    args = [compiler] if isinstance(compiler, str) else list(compiler)
    try:
        r = subprocess.run([*args, "--version"], capture_output=True, text=True, errors="replace", timeout=120)
        said = (r.stdout + r.stderr).strip()
    except (OSError, subprocess.SubprocessError) as e:
        said = f"(could not run it: {e})"
    return " ".join(args) + "\n" + said


def summary(reasons):
    """The build log's line for why objects are compiled again (reasons: a Counter of Record.check's
    answers), or None when it is only new and changed sources."""
    notable = [(why, reasons[why]) for why in NOTABLE if reasons.get(why)]
    others = [(why, n) for why, n in reasons.items() if n and why not in NOTABLE + (NEW, CHANGED)]
    notable += others   # (a record that could not be read)
    if not notable:
        return None
    total = sum(n for _w, n in notable)
    said = notable[0][0] if len(notable) == 1 else ", ".join(f"{why} ({n})" for why, n in notable)
    return f"{total} object{'s' if total != 1 else ''} to compile again: {said}"


class Record:
    """A build folder's objcache.json. Its methods may be called from several threads.

    save() keeps the objects and sources this Record looked at (check, compiled): a build looks at
    every object of its folder, and one it did not look at is no longer built there."""

    def __init__(self, folder, scan=(), log=print):
        self.folder, self.path, self.scan = folder, os.path.join(folder, RECORD), tuple(scan)
        self.lock = threading.Lock()
        self.objects, self.sources = {}, {}
        self.seen_objects, self.seen_sources, self.recent = set(), set(), set()
        self.unrecorded = UNRECORDED
        try:
            with open(self.path, encoding="utf-8") as f:
                data = json.load(f)
            if not isinstance(data, dict) or data.get("version") != VERSION:
                raise ValueError(f"not version {VERSION}'s")
            self.objects = {k: v for k, v in data["objects"].items() if isinstance(v, dict)}
            self.sources = {k: v for k, v in data["sources"].items() if isinstance(v, list) and len(v) == 4}
        except FileNotFoundError:
            pass
        except (OSError, ValueError, KeyError, TypeError, AttributeError) as e:
            self.unrecorded = f"{RECORD} could not be read"
            log(f"{self.path} could not be read ({e}): the objects there are compiled again")

    def name(self, obj):
        return os.path.relpath(obj, self.folder)

    def source(self, path):
        """(SHA-256 of a source's content, which of `scan` it holds); read again only when its size
        or time changed."""
        path = os.path.abspath(path)
        st = os.stat(path)
        with self.lock:
            self.seen_sources.add(path)
            was = self.sources.get(path)
        if was and was[0] == st.st_size and was[1] == st.st_mtime_ns and isinstance(was[3], dict) \
                and all(s.decode() in was[3] for s in self.scan):
            return was[2], {s for s in self.scan if was[3][s.decode()]}
        digest, found = file_sha(path, self.scan)
        with self.lock:
            self.sources[path] = [st.st_size, st.st_mtime_ns, digest, {s.decode(): s in found for s in self.scan}]
            if time.time_ns() - st.st_mtime_ns < RECENT_NS:
                self.recent.add(path)   # (this build trusts it; the record does not keep it)
            else:
                self.recent.discard(path)
        return digest, found

    def entry(self, sources, command, compiler, headers):
        """The key of an object compiled from `sources` (their content) by `command` (a list), with
        `compiler` (compiler_id) and runtime headers whose digest_files is `headers`; with its parts."""
        parts = {"compiler": sha(compiler), "command": sha(*command), "headers": headers,
                 "source": sha(*(self.source(p)[0] for p in sources))}
        return {"key": sha(*(parts[p] for p in PARTS)), **{p: parts[p][:16] for p in PARTS}}

    def check(self, obj, entry):
        """None if the object is there and was compiled with this key, else why it is to be compiled."""
        name = self.name(obj)
        with self.lock:
            self.seen_objects.add(name)
            was = self.objects.get(name)
        if not os.path.exists(obj):
            return NEW
        if not was:
            return self.unrecorded
        if was.get("key") == entry["key"]:
            return None
        return next((why for p, why in zip(PARTS, (COMPILER, COMMAND, HEADERS)) if was.get(p) != entry[p]), CHANGED)

    def forget(self, objs):
        """Before these objects are compiled: their keys gone from the record on disk."""
        with self.lock:
            for obj in objs:
                self.objects.pop(self.name(obj), None)
            self._save()

    def compiled(self, obj, entry, log=print):
        """An object compiled with this key: recorded at once (a build stopped later keeps it)."""
        if not os.path.exists(obj):
            return
        with self.lock:
            self.objects[self.name(obj)] = entry
            self.seen_objects.add(self.name(obj))
            try:
                self._save()
            except OSError as e:   # (not worth stopping the build for: the object is compiled again next time)
                log(f"could not write {self.path} ({e}): its objects may be compiled again next time")

    def save(self):
        with self.lock:
            self._save()

    def _save(self):
        data = {"version": VERSION,
                "objects": {k: v for k, v in self.objects.items() if k in self.seen_objects},
                "sources": {k: v for k, v in self.sources.items() if k in self.seen_sources and k not in self.recent}}
        os.makedirs(self.folder, exist_ok=True)
        tmp = self.path + ".tmp"
        with open(tmp, "w", encoding="utf-8", newline="\n") as f:
            json.dump(data, f, separators=(",", ":"), sort_keys=True)
        for attempt in range(5):   # (on Windows, something - an antivirus - can hold the old file open a moment)
            try:
                os.replace(tmp, self.path)
                return
            except PermissionError:
                if attempt == 4:
                    raise
                time.sleep(0.1 * (attempt + 1))
