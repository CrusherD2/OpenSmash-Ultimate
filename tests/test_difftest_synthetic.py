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
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
DIFFTEST = ROOT / 'tools' / 'dev' / 'difftest.py'
# CI's run: some 9,800 encodings x 8 states (capstone 5.0.9, pinned in tests/requirements-dev.txt: the words kept
# are those it decodes), built at -O2 and -O0 (8-9 s on an Apple M5 Pro with 4 processes)
DEFAULTS = ['--per-template', '2', '--states', '8', '--draws', '2000', '--seed', '1']
HAVE_PACKAGES = all(importlib.util.find_spec(m) is not None for m in ('capstone', 'unicorn'))

_spec = importlib.util.spec_from_file_location('difftest', DIFFTEST)
difftest = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(difftest)

SNAN = {16: (0x7C00, 0x3FF, 0x200), 32: (0x7F800000, 0x007FFFFF, 0x00400000),
        64: (0x7FF0000000000000, 0x000FFFFFFFFFFFFF, 0x0008000000000000)}   # exponent, fraction, quiet bit
OPERAND = re.compile(r'\b(?:([hsd])(\d+)|v(\d+)\.(\d+)([hsd]))\b')


def signalling_nan_operand(disasm, example):
    """Whether a source operand of this floating-point instruction held a signalling NaN in a lane it
    reads, in the failing state: from the disassembly, d17 is lane 0 of v17 and v4.2s the low two
    32-bit lanes of v4 (the first operand is the result)."""
    for m in list(OPERAND.finditer(disasm.partition(' ')[2]))[1:]:
        width = difftest.LANE_BITS[m[1] or m[5]]
        exp, frac, quiet = SNAN[width]
        v = int(example['v'][int(m[2] or m[3])], 16)
        for i in range(int(m[4]) if m[3] else 1):
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
    """Whether every failing state of a failure (the report has them all) shows a known bug's condition."""
    check, examples = KNOWN.get(failure['mnemonic']), failure['examples']
    return bool(check and examples and len(examples) == failure['states']
                and all(check(failure['disasm'], ex) for ex in examples))


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
        if r.returncode not in (0, 1) or not report.exists():   # (a crash, a failed compile or link too, exits 1)
            self.fail(f'difftest.py exited with {r.returncode} and no report:\n{r.stdout[-2000:]}\n{r.stderr[-4000:]}')
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
        """KNOWN's check takes a signalling NaN in a lane the instruction reads, and nothing else, for
        that known bug. (Disassembly of words assembled from their fields, in the comments.)"""
        def regs(**lanes):
            return {'v': [lanes.get(f'v{r}', '0' * 32) for r in range(32)]}

        check = signalling_nan_operand
        fmaxnm_d = 'fmaxnm d31, d17, d24'   # 1e786a3f: type 1, Rm 24, opcode 6, Rn 17, Rd 31
        snan_d0 = regs(v17='00000000000000007ff0000000000001')
        qnan_d0 = regs(v17='00000000000000007ff8000000000001')
        self.assertTrue(check(fmaxnm_d, snan_d0))
        self.assertFalse(check(fmaxnm_d, qnan_d0))
        self.assertFalse(check(fmaxnm_d, regs()))
        self.assertFalse(check(fmaxnm_d, regs(v17='7ff00000000000010000000000000000')))   # (the upper half is not read)
        self.assertFalse(check(fmaxnm_d, regs(v31='00000000000000007ff0000000000001')))   # (nor the result's register)
        fmaxnm_s = 'fmaxnm s2, s23, s12'    # 1e2c6ae2: type 0
        self.assertTrue(check(fmaxnm_s, regs(v12='000000000000000000000000ffa00000')))
        self.assertFalse(check(fmaxnm_s, regs(v23='7fbfffff000000000000000000000000')))   # (lane 3)
        fminnmv = 'fminnmv s19, v16.4s'     # 6eb0ca13: Q 1, U 1, size 2, opcode 0xC: all four lanes are read
        self.assertTrue(check(fminnmv, regs(v16='c0a47ae17fbfffff3f0000007f800000')))
        self.assertFalse(check(fminnmv, regs(v17='7fbfffff' * 4)))
        fmaxnmp_2s = 'fmaxnmp v0.2s, v1.2s, v2.2s'   # 2e22c420: Q 0, the low 64 bits
        self.assertTrue(check(fmaxnmp_2s, regs(v2='00000000000000007f80000100000000')))
        self.assertFalse(check(fmaxnmp_2s, regs(v1='7f800001000000000000000000000000')))
        fmaxnmp_d = 'fmaxnmp d20, v4.2d'    # 7e70c894: scalar pairwise, both lanes of v4
        self.assertTrue(check(fmaxnmp_d, regs(v4='7ff7ffffffffffff7fefffffffffffff')))

        failure = {'mnemonic': 'fmaxnm', 'disasm': fmaxnm_d, 'words': ['1e786a3f'], 'states': 2,
                   'examples': [snan_d0, qnan_d0]}
        self.assertFalse(known(failure))   # (every failing state must be that bug)
        failure['examples'] = [snan_d0]
        self.assertFalse(known(failure))   # (and every one must be in the report)
        failure['states'] = 1
        self.assertTrue(known(failure))
        failure['mnemonic'] = 'fadd'
        self.assertFalse(known(failure))


class SoftNanRuleTests(unittest.TestCase):
    """difftest.py calls a difference soft (a NaN payload or sign) only in the lanes of a floating-point
    result, at that result's lane width."""

    def test_lane_width(self):
        nan_only = difftest.nan_only
        self.assertTrue(nan_only(0x7FF8000000000001, 0xFFF8000000000000, 64))
        self.assertFalse(nan_only(0xFFF0000000000000, 0x7FF8000000000000, 64))   # -inf, a quiet NaN
        self.assertFalse(nan_only(0x7FFFFFFFFFFFFFFF, 0x7FEFFFFFFFFFFFFF, 64))   # a NaN, the largest double
        self.assertTrue(nan_only(0xFFF0000000000000, 0x7FF8000000000000, 32))    # (as f32 lanes, both NaNs)
        self.assertTrue(nan_only(0x7FC000013F800000, 0xFFC000003F800000, 32))
        self.assertFalse(nan_only(0x7FC000013F800000, 0x7FC000013F800001, 32))
        self.assertFalse(nan_only(0x7FF0000000000001, 0x7FF0000100000002, 32))   # (NaNs as doubles; lane 0 differs)
        self.assertTrue(nan_only(0x7E013C00, 0xFE003C00, 16))
        self.assertFalse(nan_only(0x7C003C00, 0x7E003C00, 16))
        self.assertTrue(nan_only(0xFFF0000000000000, 0x7FF8000000000000))        # (functest: no width)

    def test_result_register_and_width(self):
        fp = difftest.fp_result
        self.assertEqual(fp(['fmaxnm d31, d17, d24']), (31, 64))
        self.assertEqual(fp(['frsqrte v31.2d, v20.2d']), (31, 64))
        self.assertEqual(fp(['fminnmv s19, v16.4s']), (19, 32))
        self.assertEqual(fp(['fmla v0.4s, v1.4s, v2.s[1]']), (0, 32))
        self.assertEqual(fp(['fcvtn v0.4h, v1.4s']), (0, 16))
        self.assertEqual(fp(['fadd h0, h1, h2']), (0, 16))
        for text in ('sub v0.4s, v1.4s, v2.4s', 'fcvtzs v0.4s, v1.4s', 'fcvtzs w0, s1', 'fcmeq v0.2d, v1.2d, v2.2d',
                     'fcsel s0, s1, s2, eq', 'fmov d0, x1', 'ldr d0, [x1]', ''):
            self.assertIsNone(fp([text]), text)   # integers, compare masks, copies of bits
        self.assertIsNone(fp(['fadd s0, s1, s2', 'fadd d1, d2, d3']))


if __name__ == '__main__':
    unittest.main()
