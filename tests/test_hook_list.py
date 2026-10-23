"""tools/hooks.txt: guest functions where a hook needs the caller's whole register state. The
recompiler reads the list (recomp.hook_list, resolve_hooks) and the lifter honours it: a direct
call to an exact-callee, and every call an exact-caller makes, store every register the caller
changed and read back every register it uses (tools/regcache.py). The functions that reach an
exact-callee by a tail call, and the import stubs bound to one, are exact-callees too.
Everything else is lifted exactly as before. The functions here are made of hand-assembled
words, the lists of made-up addresses."""
import contextlib
from pathlib import Path
import platform
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock
import warnings

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import lift  # noqa: E402
import recomp  # noqa: E402

BASE = lift.BASE
MAIN = recomp.MODULES['main']
RET = 0xD65F03C0
MOV_X19_5, MOV_X20_6 = 0xD28000B3, 0xD28000D4   # movz x19, #5; movz x20, #6
MOV_X2_1 = 0xD2800022                           # movz x2, #1


# branches from pc to `to`, assembled from their fields
def b(pc, to):
    return 0x14000000 | ((to - pc) >> 2 & 0x3FFFFFF)


def bl(pc, to):
    return 0x94000000 | ((to - pc) >> 2 & 0x3FFFFFF)


def b_cond(pc, to, cond):
    return 0x54000000 | ((to - pc) >> 2 & 0x7FFFF) << 5 | cond


def cbz(pc, to, rt):
    return 0xB4000000 | ((to - pc) >> 2 & 0x7FFFF) << 5 | rt


def tbz(pc, to, rt, bit):
    return 0x36000000 | bit << 19 | ((to - pc) >> 2 & 0x3FFF) << 5 | rt


# A (0): x19 = 5, x20 = 6, a frame, bl B, then returns x19 and x20 in x0 and x1
A = [MOV_X19_5, MOV_X20_6, 0xD10083FF, 0x910043FD,   # sub sp, sp, #0x20; add x29, sp, #0x10
     0x9400000C, 0xAA1303E0, 0xAA1403E1,             # bl B (0x40); mov x0, x19; mov x1, x20
     0x910083FF, RET]                                # add sp, sp, #0x20; ret
C = [MOV_X19_5, 0xD63F0100, 0xAA1303E0, RET]        # (0x80) blr x8; mov x0, x19
D = [MOV_X19_5, 0x17FFFFDF]                         # (0xC0) b B: a tail call
E = [0x97FFFFD0, RET, 0xD2800000, RET]              # (0x100) bl B in a call range with a landing pad (0x108)
F = [MOV_X19_5, 0x97FFFFCF, 0xAA1303E0, RET]        # (0x140) bl C
G = A[:4] + [bl(0x190, 0x1C0)] + A[5:]              # (0x180) A, calling T instead of B
T = [MOV_X2_1, b(0x1C4, 0x40)]                      # (0x1C0) some work, then a tail call to B
FUNCS = {0: A, 0x40: [RET], 0x80: C, 0xC0: D, 0x100: E, 0x140: F, 0x180: G, 0x1C0: T}
PADS = {0x100: [(0x100, 0x104, 0x108)]}


def image():
    img = bytearray(0x200)
    for off, words in FUNCS.items():
        struct.pack_into(f'<{len(words)}I', img, off, *words)
    return bytes(img)


def closed(callees):
    """callees and what reaches them by tail calls, as recomp.resolve_hooks adds them."""
    return recomp.close_over_tail_calls(callees, lift.tail_calls(image(), [(o, 4 * len(w)) for o, w in FUNCS.items()],
                                                                 set(FUNCS)))


def module(regcache=True, forward=None, **lists):
    """{offset: C} of FUNCS, lifted with these exact_callees / exact_callers."""
    lifter = lift.Lifter(image(), set(FUNCS), forward=forward, pads=PADS, **lists)
    cached, lift.REGCACHE = lift.REGCACHE, regcache
    try:
        out = {}
        for off, words in FUNCS.items():
            text, reasons, _callees = lifter.lift_function(off, 4 * len(words))
            assert not reasons, reasons
            out[off] = text
        return out
    finally:
        lift.REGCACHE = cached


def around(text, call):
    """(registers stored, registers read back) at the statement with this call."""
    line = next(s for s in text.splitlines() if call in s)
    before, after = line.split(call)
    stored = set(re.findall(r'c->x\[(\d+)\] = x_\d+', before)) | ({'sp'} if 'c->sp = x_sp' in before else set())
    loaded = set(re.findall(r'x_(\d+) = c->x\[\d+\]', after)) | ({'sp'} if 'x_sp = c->sp' in after else set())
    return stored, loaded


CALL_B, CALL_T = 'f_7100000040(c);', 'f_71000001c0(c);'


class Lifting(unittest.TestCase):
    def test_a_call_to_an_exact_callee_stores_and_reads_back_everything(self):
        plain = module()[0]
        self.assertEqual(around(plain, CALL_B), ({'29', '30', 'sp'}, {'0', '1', '30'}))   # the ABI's registers only
        exact = module(exact_callees={0x40})[0]
        self.assertEqual(around(exact, CALL_B), ({'19', '20', '29', '30', 'sp'}, {'0', '1', '19', '20', '29', '30', 'sp'}))
        self.assertIn('{ c->x[0] = x_0; c->x[1] = x_1; c->sp = x_sp;  return; }', exact)   # (all clean after the call)

    def test_an_exact_caller_stores_and_reads_back_at_every_call(self):
        out = module(exact_callers={0, 0x80})
        self.assertEqual(around(out[0], CALL_B), ({'19', '20', '29', '30', 'sp'}, {'0', '1', '19', '20', '29', '30', 'sp'}))
        self.assertEqual(around(out[0x80], 'rt_lookup(t_)(c);'), ({'19', '30'}, {'0', '8', '19', '30'}))
        self.assertEqual(around(module()[0x80], 'rt_lookup(t_)(c);'), ({'30'}, {'0', '8', '30'}))

    def test_an_exact_call_inside_a_landing_site(self):
        text = module(exact_callees={0x40})[0x100]
        self.assertIn('__try', text)
        self.assertEqual(around(text, CALL_B), ({'30'}, {'0', '30', 'sp'}))   # (E uses no other register)

    def test_what_reaches_an_exact_callee_by_a_tail_call_is_one_too(self):
        """What T jumps to returns to G: a hook at B sees G's registers, and what it writes goes to
        G. T's exit stores only what T changed, so G's call to T has to be exact."""
        self.assertEqual(closed({0x40}), {0x40, 0xC0, 0x1C0})                  # D and T jump to B
        self.assertEqual(closed({0x80}), {0x80})
        self.assertIn('{ c->x[2] = x_2;  TAIL(f_7100000040) }', module()[0x1C0])
        self.assertEqual(around(module(exact_callees={0x40})[0x180], CALL_T), ({'29', '30', 'sp'}, {'0', '1', '30'}))
        self.assertEqual(around(module(exact_callees=closed({0x40}))[0x180], CALL_T),
                         ({'19', '20', '29', '30', 'sp'}, {'0', '1', '19', '20', '29', '30', 'sp'}))

    def test_tail_calls_as_the_lifter_lifts_them(self):
        """lift.tail_calls finds the TAIL(...) of each function's C, without lifting it."""
        funcs = {0x00: [b(0x00, 0x100)],                                       # b to another function
                 0x10: [b_cond(0x10, 0x100, 0), RET],                           # b.eq
                 0x20: [cbz(0x20, 0x140, 3), tbz(0x24, 0x180, 3, 5), RET],      # cbz, tbz
                 0x40: [b_cond(0x40, 0x100, 14)],                              # b.al
                 0x50: [b_cond(0x50, 0x54, 1), MOV_X2_1],                       # a branch inside, then runs on
                 0x58: [bl(0x58, 0x100), RET],                                  # a call is not a tail call
                 0x60: [b(0x60, 0x104)],                                       # into a body: a trap, then runs on
                 0x64: [0x00000000],                                           # udf
                 0x68: [MOV_X2_1, 0xD61F0220],                                  # br x17: not known
                 0x100: [MOV_X2_1, RET], 0x140: [RET], 0x180: [RET]}
        img = bytearray(0x200)
        for off, words in funcs.items():
            struct.pack_into(f'<{len(words)}I', img, off, *words)
        sizes = [(off, 4 * len(words)) for off, words in funcs.items()]
        lifter = lift.Lifter(bytes(img), set(funcs))
        lifted = {off: {int(t, 16) - BASE for t in re.findall(r'TAIL\(f_([0-9a-f]+)\)', lifter.lift_function(off, size)[0])}
                  for off, size in sizes}
        tails = lift.tail_calls(bytes(img), sizes, set(funcs))
        self.assertEqual(tails, {off: t for off, t in lifted.items() if t})
        self.assertEqual(tails, {0x00: {0x100}, 0x10: {0x100}, 0x20: {0x140, 0x180}, 0x40: {0x100}, 0x50: {0x58},
                                 0x60: {0x64}})

    def test_functions_not_on_the_list_are_unchanged(self):
        plain = module()
        self.assertEqual(module(exact_callees={0x140}), plain)        # F: no function here calls it
        callers = module(exact_callers={0x140})
        self.assertEqual({k: v for k, v in callers.items() if k != 0x140}, {k: v for k, v in plain.items() if k != 0x140})
        self.assertNotEqual(callers[0x140], plain[0x140])
        callees = module(exact_callees={0x80})                         # C: only F calls it
        self.assertEqual({k: v for k, v in callees.items() if k != 0x140}, {k: v for k, v in plain.items() if k != 0x140})
        self.assertEqual(around(callees[0x140], 'f_7100000080(c);')[0], {'19', '30'})

    def test_without_the_register_cache_the_list_changes_nothing(self):
        """Every register is in the context at every call then."""
        plain = module(regcache=False)
        self.assertEqual(module(regcache=False, exact_callees={0x40, 0x80}, exact_callers=set(FUNCS)), plain)
        self.assertNotIn('x_19', plain[0])

    def test_the_build_hands_the_lists_to_every_worker(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'image.bin'
            path.write_bytes(image())
            with warnings.catch_warnings():   # (_init leaves its file to the mmap, as a worker does)
                warnings.simplefilter('ignore', ResourceWarning)
                recomp._init(str(path), set(FUNCS), BASE, {}, set(), PADS, {0x40}, {0x80})
            try:
                lifter = recomp._lifter()
                self.assertEqual((lifter.exact_callees, lifter.exact_callers), ({0x40}, {0x80}))
                cached, lift.REGCACHE = lift.REGCACHE, True
                try:
                    text = lifter.lift_function(0, 4 * len(A))[0]
                finally:
                    lift.REGCACHE = cached
                self.assertIn('19', around(text, CALL_B)[0])
            finally:
                recomp._state['img'].close()
                recomp._state.clear()


@contextlib.contextmanager
def hooks_file(text):
    """tools/hooks.txt, for recomp, holding text."""
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / 'hooks.txt'
        path.write_text(text, encoding='utf-8')
        with mock.patch.object(recomp, 'HOOKS', str(path)):
            yield


class HookList(unittest.TestCase):
    def parse(self, text):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'hooks.txt'
            if isinstance(text, bytes):
                path.write_bytes(text)
            else:
                path.write_text(text, encoding='utf-8')
            return recomp.hook_list(str(path))

    def refused(self, text, why):
        with self.assertRaises(SystemExit) as e:
            self.parse(text)
        self.assertIn(why, str(e.exception))

    def test_reads_entries_comments_and_blank_lines(self):
        self.assertEqual(self.parse('# a comment\n\nexact-callee 7100001000 some_function\n'
                                    'exact-caller\t0x7109000020 16 words, sha256 0123456789abcdef  app::Thing::run  # why\n'
                                    'exact-callee 7108000040 1 word, sha256 fedcba9876543210 other\n'
                                    'exact-caller 7100001000 some_function\n'),
                         {('exact-callee', 0x7100001000): None, ('exact-caller', 0x7109000020): (16, '0123456789abcdef'),
                          ('exact-callee', 0x7108000040): (1, 'fedcba9876543210'), ('exact-caller', 0x7100001000): None})
        self.assertEqual(self.parse(''), {})

    def test_refuses_what_could_be_instruction_words(self):
        for line in ('exact-callee 7100001000 f d10083ff\n', 'exact-callee 7100001000 name  # d10083ff 94000123\n',
                     '# 9....... 54.....1\n', 'exact-callee 7100001000 name # was 0xd10083ff\n',
                     'exact-callee 7100001000 deadbeef\n', 'exact-callee 39a4040c name\n',
                     # next to punctuation
                     '# the first word is d10083ff.\n', '#d10083ff\n', '# d10083ff/aa1303e0\n', '# d10083ff-aa1303e0\n',
                     '# *d10083ff*\n', '# |d10083ff|\n', '# 0xd10083ff.\n', '# d10083ff+d10083ff\n', '# sub_d10083ff\n',
                     '# (d10083ff)\n', 'exact-callee 7100001000 name #d10083ff\n', '# 9.......!\n',
                     'exact-callee 7100001000 name # words: d10083ff|aa1303e0|aa1403e1|d65f03c0\n'):
            with self.subTest(line=line):
                self.refused(line, 'instruction words')

    def test_reads_addresses_hashes_and_prose_in_comments(self):
        """A longer run of hex digits is an address or a hash, not a word."""
        self.assertEqual(self.parse('# see 7100001000. Its hash, 0123456789abcdef, is for 13.0.5 (x19-x28)...\n'
                                    '# ........ a rule\n'), {})

    def test_reads_a_file_saved_with_a_bom_or_crlf(self):
        self.assertEqual(self.parse('\ufeff# a comment\r\nexact-callee 7100001000 name\r\n'.encode('utf-8')),
                         {('exact-callee', 0x7100001000): None})

    def test_refuses_malformed_lines(self):
        for line, why in (('exact-callee 7100001000\n', 'not `exact-callee|exact-caller'),
                          ('exact_callee 7100001000 name\n', 'not `exact-callee|exact-caller'),
                          ('Exact-Callee 7100001000 name\n', 'not `exact-callee|exact-caller'),
                          ('hook 7100001000 name\n', 'not `exact-callee|exact-caller'),
                          ('exact-callee 71000010zz name\n', 'not `exact-callee|exact-caller'),
                          ('exact-callee 7100001000 name extra\n', 'not `exact-callee|exact-caller'),
                          ('exact-callee 7100001000 16 words, sha256 0123 name\n', 'not `exact-callee|exact-caller'),
                          ('exact-callee 7100001000 sixteen words, sha256 0123456789abcdef name\n', 'not `exact-callee'),
                          ('exact-callee 7100001000 16 words sha256 0123456789abcdef name\n', 'not `exact-callee'),
                          ('exact-callee 7100001000 0 words, sha256 0123456789abcdef name\n', 'at least one word'),
                          ('exact-callee 7100001002 name\n', 'not a function address'),
                          ('exact-callee 7200000000 name\n', 'not a function address'),    # a fighter module's
                          ('exact-callee 7000001000 name\n', 'not a function address'),
                          ('exact-callee 7100001000 9lives\n', 'not a function name'),
                          ('exact-callee 7100001000 deadbeefdeadbeef\n', 'not a function name'),
                          ('exact-callee 7100001000 a\nexact-callee 0x7100001000 b\n', 'listed twice')):
            with self.subTest(line=line):
                self.refused(line, why)

    def test_the_repositorys_list_parses(self):
        recomp.hook_list()   # (raises on anything it cannot read)
        self.assertTrue(Path(recomp.HOOKS).is_file())

    def test_the_fingerprint_follows_the_entries(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'hooks.txt'

            def fingerprint(text):
                path.write_text(text, encoding='utf-8')
                with mock.patch.object(recomp, 'HOOKS', str(path)):
                    return recomp.tools_fingerprint()
            empty = fingerprint('# nothing\n')
            one = fingerprint('exact-callee 7100001000 name\n')
            self.assertNotEqual(empty, one)
            self.assertEqual(fingerprint('# a comment\nexact-callee   7100001000 name   # changed\n'), one)
            self.assertNotEqual(fingerprint('exact-caller 7100001000 name\n'), one)
            self.assertNotEqual(fingerprint('exact-callee 7100001000 16 words, sha256 0123456789abcdef name\n'), one)


class FakeModule:
    """The one thing signature() reads from a module: its words (or its image)."""

    def __init__(self, words):
        self.image = words if isinstance(words, bytes) else struct.pack(f'<{len(words)}I', *words)

    def u32(self, a):
        return struct.unpack_from('<I', self.image, a)[0]


# P (0) calls nn_hooked through the module's import stub (0x100), whose GOT slot is 0x200; J (0x40) jumps to it
STUB = [0x90000010, 0xF9400211 | 0x40 << 10, 0x91000210 | 0x200 << 10, 0xD61F0220]   # adrp x16; ldr x17; add x16; br x17
P = [MOV_X19_5, bl(4, 0x100), 0xAA1303E0, RET]                                     # ...; mov x0, x19; ret
STUB_FUNCS = {0: P, 0x40: [b(0x40, 0x100)], 0x100: STUB}


class StubModule(FakeModule):
    """What recomp.import_stub_symbols reads (funcs.import_stubs): STUB_FUNCS, and a jump-slot
    relocation binding the stub's slot to `symbol`."""

    def __init__(self, symbol):
        img = bytearray(0x208)
        for off, words in STUB_FUNCS.items():
            struct.pack_into(f'<{len(words)}I', img, off, *words)
        super().__init__(bytes(img))
        self.text, self.eh_frame_hdr = types.SimpleNamespace(mem=0, end=0x200), (0, 0)
        self.relocs = [(0x200, 1026, 1, 0)]   # R_AARCH64_JUMP_SLOT
        self.symbols = [('', 0, 0, 0, 0), (symbol, 0, 0, 0x12, 0)]


class ResolveHooks(unittest.TestCase):
    MAIN = MAIN

    def resolve(self, text, sizes=None, base=None):
        mod = FakeModule([0] * 16 + A)
        with hooks_file(text):
            return recomp.resolve_hooks(mod, base or self.MAIN, sizes or {0x40: 4 * len(A), 0x80: 4}, 'main')

    def test_this_modules_entries_as_offsets(self):
        sig = recomp.signature_comment(recomp.signature(FakeModule(A), 0, 4 * len(A)))[len('/* signature: '):-len(' */')]
        got = self.resolve(f'exact-callee {self.MAIN + 0x40:x} {sig} a\nexact-caller {self.MAIN + 0x80:x} b\n'
                           f'exact-callee {recomp.MODULES["sdk"] + 0x40:x} elsewhere\n')
        self.assertEqual(got, ({0x40}, {0x80}))
        self.assertEqual(self.resolve(''), (set(), set()))

    def test_no_function_there_stops_the_build(self):
        with self.assertRaises(SystemExit) as e:
            self.resolve(f'exact-callee {self.MAIN + 0x44:x} a\n')
        self.assertIn('no function starts there', str(e.exception))

    def test_a_different_function_stops_the_build(self):
        other = list(A)
        other[0] = MOV_X20_6
        sig = recomp.signature_comment(recomp.signature(FakeModule(other), 0, 4 * len(other)))[len('/* signature: '):-len(' */')]
        with self.assertRaises(SystemExit) as e:
            self.resolve(f'exact-caller {self.MAIN + 0x40:x} {sig} a\n')
        self.assertIn('does not begin as its signature says', str(e.exception))

    def test_what_reaches_an_exact_callee_by_a_tail_call_is_one_too(self):
        sizes = {off: 4 * len(words) for off, words in FUNCS.items()}
        with hooks_file(f'exact-callee {self.MAIN + 0x40:x} b\nexact-caller {self.MAIN + 0x180:x} g\n'):
            self.assertEqual(recomp.resolve_hooks(FakeModule(image()), self.MAIN, sizes, 'main', set(FUNCS)),
                             ({0x40, 0xC0, 0x1C0}, {0x180}))

    def test_import_stubs_bound_to_an_exact_callee_are_exact_callees(self):
        """Calls from another module reach it through an import stub of the caller's module."""
        sdk, sizes = recomp.MODULES['sdk'], {off: 4 * len(words) for off, words in STUB_FUNCS.items()}

        def resolve(symbol, imports):
            return recomp.resolve_hooks(StubModule(symbol), self.MAIN, sizes, 'main', set(sizes), imports)
        with hooks_file(f'exact-callee {sdk + 0x1000:x} nn_hooked\n'):
            self.assertEqual(resolve('nn_hooked', {'nn_hooked': sdk + 0x1000}), ({0x40, 0x100}, set()))   # J jumps to it
            self.assertEqual(resolve('nn_other', {'nn_hooked': sdk + 0x1000}), (set(), set()))
            self.assertEqual(resolve('nn_hooked', None), (set(), set()))
        cached, lift.REGCACHE = lift.REGCACHE, True
        try:
            text = lift.Lifter(StubModule('nn_hooked').image, set(sizes), exact_callees={0x40, 0x100}).lift_function(0, 16)[0]
        finally:
            lift.REGCACHE = cached
        self.assertEqual(around(text, 'f_7100000100(c);'), ({'19', '30'}, {'0', '19', '30'}))

    def test_import_names_are_bound_as_the_loader_binds_them(self):
        """The first of main, subsdk0 and sdk to export a name defines it (runtime/host.c)."""
        sdk = recomp.MODULES['sdk']
        mods = {'main': types.SimpleNamespace(exports=[('a', 0x40, 0, 0x12, 1), ('shared', 0x80, 0, 0x12, 1)]),
                'sdk': types.SimpleNamespace(exports=[('b', 0x1000, 0, 0x12, 1), ('shared', 0x2000, 0, 0x12, 1),
                                                      ('d', 0x3000, 0, 0x12, 1)])}
        with hooks_file(f'exact-callee {self.MAIN + 0x40:x} a\nexact-callee {sdk + 0x1000:x} b\n'
                        f'exact-callee {sdk + 0x2000:x} c\nexact-caller {sdk + 0x3000:x} d\n'):
            self.assertEqual(recomp.hook_imports(mods), {'a': self.MAIN + 0x40, 'b': sdk + 0x1000})   # main's `shared` first
        with hooks_file('# nothing\n'):
            self.assertEqual(recomp.hook_imports(mods), {})


# ---- built and run on an arm64 Mac ------------------------------------------------------
# B is wrapped natively, as an HLE_WRAP does it: hle_test_hook reads the registers A had when
# it called B and changes x19 and x20, which A then returns. Or the same through T, called by
# G, which jumps to B.
CLANG = shutil.which('clang')
ON_ARM64_MAC = sys.platform == 'darwin' and platform.machine() == 'arm64' and CLANG is not None
MAC_CFLAGS = ['-arch', 'arm64', '-mmacosx-version-min=14.0', '-mcpu=apple-m1', '-fno-strict-aliasing', '-ffp-contract=off',
              '-fno-stack-protector', '-fvisibility=hidden', '-w']
HARNESS = r'''
#include <stdio.h>
#include "recomp.h"

GuestFn ENTRY;
static u64 seen[4];
void hle_test_hook(Cpu *c) { seen[0] = c->x[19]; seen[1] = c->x[20]; seen[2] = c->x[29]; seen[3] = c->sp; c->x[19] = 77; c->x[20] = 88; }

int main(void) {
    static Cpu c;
    c.x[19] = 1; c.x[20] = 2; c.x[29] = 3; c.sp = 0x10000;
    ENTRY(&c);
    printf("%llu %llu %llx %llx %llu %llu\n", (unsigned long long)seen[0], (unsigned long long)seen[1], (unsigned long long)seen[2],
           (unsigned long long)seen[3], (unsigned long long)c.x[0], (unsigned long long)c.x[1]);
    return 0;
}
'''


@unittest.skipUnless(ON_ARM64_MAC, 'needs an arm64 Mac with clang (the Command Line Tools)')
class MacRunTests(unittest.TestCase):
    def run_a(self, tmp, entry=0, **lists):
        """What the hook saw and what the function at entry (A, or G) returned."""
        out = module(forward={0x40: ('wrap', 'hle_test_hook')}, **lists)
        parts = [0, 0x40] if entry == 0 else [entry, 0x1C0, 0x40]
        gen = Path(tmp) / 'g0000.c'
        gen.write_text('#include "recomp.h"\n\nGuestFn ' + ', '.join(f'f_{BASE + p:x}' for p in parts)
                       + ', f_7100000040_orig;\n\n' + '\n\n'.join(out[p] for p in parts) + '\n', encoding='utf-8')
        harness = Path(tmp) / 'harness.c'
        harness.write_text(HARNESS.replace('ENTRY', f'f_{BASE + entry:x}'), encoding='utf-8')
        exe = Path(tmp) / 'run'
        r = subprocess.run([CLANG, *MAC_CFLAGS, '-O2', '-I', str(ROOT / 'runtime'), str(gen), str(harness), '-o', str(exe)],
                           capture_output=True, text=True, timeout=300)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 0, r.stderr)
        return r.stdout.split()

    def test_a_hook_sees_and_changes_the_callers_registers(self):
        with tempfile.TemporaryDirectory() as tmp:
            # x19, x20 as A set them; x29 and SP as always; and what the hook wrote is what A returns
            self.assertEqual(self.run_a(tmp, exact_callees={0x40}), ['5', '6', 'fff0', 'ffe0', '77', '88'])
            self.assertEqual(self.run_a(tmp, exact_callers={0}), ['5', '6', 'fff0', 'ffe0', '77', '88'])
            # not listed: the context still holds what x19 and x20 were before A, and A keeps its own
            self.assertEqual(self.run_a(tmp), ['1', '2', 'fff0', 'ffe0', '5', '6'])

    def test_through_a_tail_call(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(self.run_a(tmp, 0x180, exact_callees=closed({0x40})), ['5', '6', 'fff0', 'ffe0', '77', '88'])
            self.assertEqual(self.run_a(tmp, 0x180, exact_callers={0x180}), ['5', '6', 'fff0', 'ffe0', '77', '88'])
            # B alone, without T (what resolve_hooks adds): G's call to T is an ordinary one
            self.assertEqual(self.run_a(tmp, 0x180, exact_callees={0x40}), ['1', '2', 'fff0', 'ffe0', '5', '6'])


if __name__ == '__main__':
    unittest.main()
