"""The lifter (tools/lift.py) against the unicorn AArch64 emulator, without the game: tools/dev/difftest.py
--synthetic makes its instruction words from the architecture's bit fields (tools/dev/difftest_words.py), lifts
them, compiles the C with the game build's compiler and flags for this OS (clang-cl on Windows, the Linux build's
x86-64 flags, the macOS build's arm64 flags) at -O2 and -O0, and compares every register, the flags, the next PC
and the memory written, from random states with special floating-point operands. Skipped only where capstone and
unicorn (tests/requirements-dev.txt) or that compiler are missing: CI has all of them.

SSBU_DIFFTEST_ARGS adds difftest.py options for a deeper local run (they win over the defaults below), e.g.
SSBU_DIFFTEST_ARGS="--per-template 4 --states 32 --draws 8000 --seed 2"; with --exact-nan, NaN results that
differ only in sign or payload count as mismatches too."""
from pathlib import Path
import importlib.util
import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
DIFFTEST = ROOT / 'tools' / 'dev' / 'difftest.py'
# CI's run: some 8,600 encodings x 8 states (capstone 5.0.7), built at -O2 and -O0 (8 s on an Apple M5 Pro with 4 processes)
DEFAULTS = ['--per-template', '2', '--states', '8', '--draws', '2000', '--seed', '1']
HAVE_PACKAGES = all(importlib.util.find_spec(m) is not None for m in ('capstone', 'unicorn'))

SNAN = {32: (0x7F800000, 0x007FFFFF, 0x00400000), 64: (0x7FF0000000000000, 0x000FFFFFFFFFFFFF, 0x0008000000000000)}


def signalling_nan_operand(word, example):
    """Whether a source register of this floating-point instruction held a signalling NaN of its
    operand width (bit 22: size<0> / type<0>) in the failing state."""
    three = (word & 0x9F200400 == 0x0E200400 or word & 0xDF200400 == 0x5E200400   # (scalar) three same
             or word & 0xFF200C00 == 0x1E200800)                                  # fp, two sources
    width = 64 if word >> 22 & 1 else 32
    exp, frac, quiet = SNAN[width]
    for r in ({word >> 5 & 31, word >> 16 & 31} if three else {word >> 5 & 31}):
        v = int(example['v'][r], 16)
        for i in range(128 // width):
            lane = v >> (i * width) & ((1 << width) - 1)
            if lane & exp == exp and lane & frac and not lane & quiet:
                return True
    return False


# Known lifter mismatches, by mnemonic, each with the condition that makes a failure that known one
# and no other. Each is a bug to fix in a pull request of its own; remove its entry then.
#
# FMAXNM / FMINNM, and their pairwise (fmaxnmp) and across-lanes (fmaxnmv) forms, with a signalling
# NaN operand: Arm's FPMaxNum / FPMinNum quiet and return it (a signalling NaN goes before a number),
# while runtime/recomp.h's fmaxnmS / fminnmS (and the D forms) return the other operand. In the
# reductions the quiet NaN then drops out a step later, so there both results are numbers, but
# different ones. Exact NaN semantics are their own later change, with a benchmark of what they
# cost; quiet NaN operands, and every operand that is not a NaN, match here.
KNOWN = {m: signalling_nan_operand for m in ('fmaxnm', 'fminnm', 'fmaxnmp', 'fminnmp', 'fmaxnmv', 'fminnmv')}


def known(failure):
    check = KNOWN.get(failure['mnemonic'])
    word = int(failure['words'][0], 16)
    return bool(check and failure['examples'] and all(check(word, ex) for ex in failure['examples']))


def describe(failure):
    lines = [f"{' '.join(failure['words'])}  {failure['disasm']}  ({failure['states']} failing states)"]
    lines += [f'    C: {c}' for c in failure['c']]
    ex = failure['examples'][0]
    lines.append(f"    {ex['why']}" + (f"; {ex['more']}" if ex['more'] else ''))
    lines.append(f"    x: {' '.join(ex['x'])}  sp {ex['sp']}  nzcv {ex['nzcv']}")
    lines.append(f"    v: {' '.join(ex['v'])}")
    return '\n'.join(lines)


@unittest.skipUnless(HAVE_PACKAGES, 'needs capstone and unicorn (pip install -r tests/requirements-dev.txt)')
class SyntheticDifftestTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = Path(tempfile.mkdtemp(prefix='difftest-synthetic-'))

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.dir, ignore_errors=True)

    def test_lifted_code_matches_unicorn(self):
        report = self.dir / 'report.json'
        extra = shlex.split(os.environ.get('SSBU_DIFFTEST_ARGS', ''))
        cmd = [sys.executable, str(DIFFTEST), '--synthetic', *extra, *DEFAULTS, '--work', str(self.dir / 'work'),
               '--report', str(report), '--jobs', str(min(16, os.cpu_count() or 2))]
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=1800)
        if r.returncode == 3:
            self.skipTest(r.stdout.strip().splitlines()[-1])
        self.assertIn(r.returncode, (0, 1), r.stdout[-4000:] + r.stderr[-4000:])
        # (in CI's log the whole report, with what each instruction class has that is not lifted)
        lines = r.stdout.splitlines()
        print('\n' + '\n'.join(lines if os.environ.get('CI') else
                               [l for l in lines if l.startswith(('[O', 'native side', 'ran in')) or ' test encodings' in l
                                if not l.startswith('[O') or ' ran ' in l or 'encodings with' in l]))
        res = json.loads(report.read_text(encoding='utf-8'))

        # it tested what it should have: thousands of encodings, every state compared at both levels
        self.assertGreater(res['tests'], 3000)
        self.assertEqual(res['compared'] + res['reference_faults'], res['tests'] * res['states'])
        self.assertLess(res['reference_faults'] * 100, res['compared'])
        self.assertEqual([lv['level'] for lv in res['levels']], ['O2', 'O0'])
        unknown = []
        for lv in res['levels']:
            self.assertEqual(lv['native_faults'], 0, f"{lv['level']}: native faults where the reference ran")
            unknown += [f"[{lv['level']}] {describe(f)}" for f in lv['failing'] if not known(f)]
        if unknown:
            self.fail(f'lifted code and unicorn disagree on {len(unknown)} encodings (the first 20):\n' + '\n'.join(unknown[:20]))

    def test_known_failure_check(self):
        """KNOWN's check takes a signalling NaN operand, and nothing else, for that known bug."""
        fmaxnm_d = 0x1E786A3F   # fmaxnm d31, d17, d24 (assembled: type 1, Rm 24, opcode 6, Rn 17, Rd 31)
        v = ['0' * 32] * 32
        with_snan = v[:17] + ['00000000000000007ff0000000000001'] + v[18:]
        with_qnan = v[:17] + ['00000000000000007ff8000000000001'] + v[18:]
        self.assertTrue(signalling_nan_operand(fmaxnm_d, {'v': with_snan}))
        self.assertFalse(signalling_nan_operand(fmaxnm_d, {'v': with_qnan}))
        self.assertFalse(signalling_nan_operand(fmaxnm_d, {'v': v}))
        fminnmv = 0x6EB0CA13    # fminnmv s19, v16.4s (Q 1, U 1, size 2, opcode 0xC, Rn 16, Rd 19): one source;
        #                         a signalling NaN in any of its lanes counts
        lanes = ['c0a47ae17fbfffff3f0000007f800000' if r == 16 else '0' * 32 for r in range(32)]
        self.assertTrue(signalling_nan_operand(fminnmv, {'v': lanes}))
        elsewhere = ['7fbfffff' * 4 if r == 17 else '0' * 32 for r in range(32)]   # (v17 is not an operand)
        self.assertFalse(signalling_nan_operand(fminnmv, {'v': elsewhere}))
        failure = {'mnemonic': 'fmaxnm', 'words': [f'{fmaxnm_d:08x}'], 'examples': [{'v': with_snan}, {'v': with_qnan}]}
        self.assertFalse(known(failure))   # (every failing state must be that bug)
        failure['examples'] = [{'v': with_snan}]
        self.assertTrue(known(failure))
        failure['mnemonic'] = 'fadd'
        self.assertFalse(known(failure))


if __name__ == '__main__':
    unittest.main()
