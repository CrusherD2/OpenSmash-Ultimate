"""tools/hooks.txt: guest functions where a hook needs the caller's whole register state. The
recompiler reads the list (recomp.hook_list, resolve_hooks) and the lifter honours it: a direct
call to an exact-callee, and every call an exact-caller makes, store every register the caller
changed and read back every register it uses (tools/regcache.py). Everything else is lifted
exactly as before. The functions here are made of hand-assembled words, the lists of made-up
addresses."""
from pathlib import Path
import platform
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import warnings

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import lift  # noqa: E402
import recomp  # noqa: E402

BASE = lift.BASE
RET = 0xD65F03C0
MOV_X19_5, MOV_X20_6 = 0xD28000B3, 0xD28000D4   # movz x19, #5; movz x20, #6
# A (0): x19 = 5, x20 = 6, a frame, bl B, then returns x19 and x20 in x0 and x1
A = [MOV_X19_5, MOV_X20_6, 0xD10083FF, 0x910043FD,   # sub sp, sp, #0x20; add x29, sp, #0x10
     0x9400000C, 0xAA1303E0, 0xAA1403E1,             # bl B (0x40); mov x0, x19; mov x1, x20
     0x910083FF, RET]                                # add sp, sp, #0x20; ret
C = [MOV_X19_5, 0xD63F0100, 0xAA1303E0, RET]        # (0x80) blr x8; mov x0, x19
D = [MOV_X19_5, 0x17FFFFDF]                         # (0xC0) b B: a tail call
E = [0x97FFFFD0, RET, 0xD2800000, RET]              # (0x100) bl B in a call range with a landing pad (0x108)
F = [MOV_X19_5, 0x97FFFFCF, 0xAA1303E0, RET]        # (0x140) bl C
FUNCS = {0: A, 0x40: [RET], 0x80: C, 0xC0: D, 0x100: E, 0x140: F}
PADS = {0x100: [(0x100, 0x104, 0x108)]}


def module(regcache=True, forward=None, **lists):
    """{offset: C} of FUNCS, lifted with these exact_callees / exact_callers."""
    img = bytearray(0x200)
    for off, words in FUNCS.items():
        struct.pack_into(f'<{len(words)}I', img, off, *words)
    lifter = lift.Lifter(bytes(img), set(FUNCS), forward=forward, pads=PADS, **lists)
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


CALL_B = 'f_7100000040(c);'


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

    def test_a_tail_call_is_exact_already(self):
        """Every exit stores every register it changed, so a tail call needs no entry."""
        self.assertIn('{ c->x[19] = x_19;  TAIL(f_7100000040) }', module()[0xC0])
        self.assertEqual(module(exact_callees={0x40})[0xC0], module()[0xC0])

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
        img = bytearray(0x200)
        for off, words in FUNCS.items():
            struct.pack_into(f'<{len(words)}I', img, off, *words)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'image.bin'
            path.write_bytes(bytes(img))
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


class HookList(unittest.TestCase):
    def parse(self, text):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'hooks.txt'
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
                     'exact-callee 7100001000 deadbeef\n', 'exact-callee 39a4040c name\n'):
            with self.subTest(line=line):
                self.refused(line, 'instruction words')

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
    """The one thing signature() reads from a module: its words."""

    def __init__(self, words):
        self.image = struct.pack(f'<{len(words)}I', *words)

    def u32(self, a):
        return struct.unpack_from('<I', self.image, a)[0]


class ResolveHooks(unittest.TestCase):
    MAIN = recomp.MODULES['main']

    def resolve(self, text, sizes=None, base=None):
        mod = FakeModule([0] * 16 + A)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'hooks.txt'
            path.write_text(text, encoding='utf-8')
            with mock.patch.object(recomp, 'HOOKS', str(path)):
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


# ---- built and run on an arm64 Mac ------------------------------------------------------
# B is wrapped natively, as an HLE_WRAP does it: hle_test_hook reads the registers A had when
# it called B and changes x19 and x20, which A then returns.
CLANG = shutil.which('clang')
ON_ARM64_MAC = sys.platform == 'darwin' and platform.machine() == 'arm64' and CLANG is not None
MAC_CFLAGS = ['-arch', 'arm64', '-mmacosx-version-min=14.0', '-mcpu=apple-m1', '-fno-strict-aliasing', '-ffp-contract=off',
              '-fno-stack-protector', '-fvisibility=hidden', '-w']
HARNESS = r'''
#include <stdio.h>
#include "recomp.h"

GuestFn f_7100000000;
static u64 seen[4];
void hle_test_hook(Cpu *c) { seen[0] = c->x[19]; seen[1] = c->x[20]; seen[2] = c->x[29]; seen[3] = c->sp; c->x[19] = 77; c->x[20] = 88; }

int main(void) {
    static Cpu c;
    c.x[19] = 1; c.x[20] = 2; c.x[29] = 3; c.sp = 0x10000;
    f_7100000000(&c);
    printf("%llu %llu %llx %llx %llu %llu\n", (unsigned long long)seen[0], (unsigned long long)seen[1], (unsigned long long)seen[2],
           (unsigned long long)seen[3], (unsigned long long)c.x[0], (unsigned long long)c.x[1]);
    return 0;
}
'''


@unittest.skipUnless(ON_ARM64_MAC, 'needs an arm64 Mac with clang (the Command Line Tools)')
class MacRunTests(unittest.TestCase):
    def run_a(self, tmp, **lists):
        out = module(forward={0x40: ('wrap', 'hle_test_hook')}, **lists)
        gen = Path(tmp) / 'g0000.c'
        gen.write_text('#include "recomp.h"\n\nGuestFn f_7100000000, f_7100000040, f_7100000040_orig;\n\n'
                       + out[0] + '\n\n' + out[0x40] + '\n', encoding='utf-8')
        harness = Path(tmp) / 'harness.c'
        harness.write_text(HARNESS, encoding='utf-8')
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


if __name__ == '__main__':
    unittest.main()
