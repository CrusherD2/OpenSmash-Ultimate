"""Helpers of runtime/recomp.h checked against their definitions, built with the game build's compiler and flags
for this OS (tools/dev/difftest.py's toolchain: clang-cl on Windows, the Linux build's x86-64 flags, the macOS
build's arm64 flags; on an Apple silicon Mac with Rosetta 2, the Linux build's flags under it as well), at -O2 and
-O0, and outside Windows as C++ too (as the build compiles the generated files with landing sites):

  rt_rsqrt_est     (frsqrte's estimate) equal to the architecture's loop, which it replaced, for every input
  rt_crc32*        the CRC-32 and CRC-32C check values (of "123456789"), and the host's instruction or CRC-32's
                   tables (whichever is used) equal to the bitwise definition, rt_crc32_bits, for random operands"""
from pathlib import Path
import importlib.util
import os
import platform
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
_spec = importlib.util.spec_from_file_location('difftest', ROOT / 'tools' / 'dev' / 'difftest.py')
difftest = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(difftest)

CHECK = r'''
#include <stdio.h>
#include "recomp.h"

/* rt_rsqrt_est as it was: the loop of the Arm manual's RecipSqrtEstimate */
static u32 rsqrt_est_loop(u32 a) {
    a = a < 256 ? a * 2 + 1 : ((a & ~1u) + 1) * 2;
    u32 b = 512;
    while ((u64)a * (b + 1) * (b + 1) < (1ull << 28)) b++;
    return (b + 1) / 2;
}

static u64 next(u64 *s) { *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17; return *s; }   /* xorshift64 */
static int bad;
#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); bad++; } } while (0)

int main(void) {
    for (u32 a = 0; a < 4096; a++)   /* (frsqrte gives it 128 .. 511) */
        if (rt_rsqrt_est(a) != rsqrt_est_loop(a)) { printf("rt_rsqrt_est(%u) = %u, the loop's %u\n", a, rt_rsqrt_est(a), rsqrt_est_loop(a)); bad++; }

    /* the check values: "123456789" from all ones, the result inverted */
    const char *s = "123456789";
    u32 b = ~0u, cb = ~0u, bits = ~0u, cbits = ~0u, h = ~0u, ch = ~0u, w = ~0u, cw = ~0u, x = ~0u, cx = ~0u;
    u64 lo = 0;
    for (int i = 0; i < 8; i++) lo |= (u64)(u8)s[i] << (8 * i);
    for (int i = 0; i < 9; i++) {
        b = rt_crc32b(b, (u8)s[i]); cb = rt_crc32cb(cb, (u8)s[i]);
        bits = rt_crc32_bits(bits, (u8)s[i], 8, RT_CRC32_POLY); cbits = rt_crc32_bits(cbits, (u8)s[i], 8, RT_CRC32C_POLY);
    }
    for (int i = 0; i < 4; i++) { h = rt_crc32h(h, (u16)(lo >> (16 * i))); ch = rt_crc32ch(ch, (u16)(lo >> (16 * i))); }
    for (int i = 0; i < 2; i++) { w = rt_crc32w(w, (u32)(lo >> (32 * i))); cw = rt_crc32cw(cw, (u32)(lo >> (32 * i))); }
    x = rt_crc32x(x, lo); cx = rt_crc32cx(cx, lo);
    h = rt_crc32b(h, '9'); ch = rt_crc32cb(ch, '9'); w = rt_crc32b(w, '9'); cw = rt_crc32cb(cw, '9'); x = rt_crc32b(x, '9'); cx = rt_crc32cb(cx, '9');
    CHECK(~b == 0xCBF43926u && ~bits == 0xCBF43926u && ~h == 0xCBF43926u && ~w == 0xCBF43926u && ~x == 0xCBF43926u);
    CHECK(~cb == 0xE3069283u && ~cbits == 0xE3069283u && ~ch == 0xE3069283u && ~cw == 0xE3069283u && ~cx == 0xE3069283u);

    /* each size against the definition, the same operands for both polynomials */
    u64 st = 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < 200000; i++) {
        u32 acc = (u32)next(&st);
        u64 v = next(&st);
        if (i < 64) { acc = i & 1 ? ~0u : 0; v = i & 2 ? ~0ull : i & 4 ? 1ull << (i >> 3) : 0; }
        CHECK(rt_crc32b(acc, (u8)v) == rt_crc32_bits(acc, (u8)v, 8, RT_CRC32_POLY));
        CHECK(rt_crc32h(acc, (u16)v) == rt_crc32_bits(acc, (u16)v, 16, RT_CRC32_POLY));
        CHECK(rt_crc32w(acc, (u32)v) == rt_crc32_bits(acc, (u32)v, 32, RT_CRC32_POLY));
        CHECK(rt_crc32x(acc, v) == rt_crc32_bits(acc, v, 64, RT_CRC32_POLY));
        CHECK(rt_crc32cb(acc, (u8)v) == rt_crc32_bits(acc, (u8)v, 8, RT_CRC32C_POLY));
        CHECK(rt_crc32ch(acc, (u16)v) == rt_crc32_bits(acc, (u16)v, 16, RT_CRC32C_POLY));
        CHECK(rt_crc32cw(acc, (u32)v) == rt_crc32_bits(acc, (u32)v, 32, RT_CRC32C_POLY));
        CHECK(rt_crc32cx(acc, v) == rt_crc32_bits(acc, v, 64, RT_CRC32C_POLY));
        if (bad > 20) break;
    }
#if defined(__ARM_FEATURE_CRC32)
    puts("crc32: arm; crc32c: arm");
#elif defined(__SSE4_2__)
    CHECK(rt_crc32_tables() != 0);
    puts("crc32: tables; crc32c: sse4.2");
#else
    CHECK(rt_crc32_tables() != 0);
    puts("crc32: tables; crc32c: bitwise");
#endif
    puts(bad ? "FAILED" : "ok");
    return bad != 0;
}
'''

# the CRC instructions each build uses
EXPECTED = {'windows': 'crc32: tables; crc32c: sse4.2', 'linux': 'crc32: tables; crc32c: sse4.2',
            'rosetta': 'crc32: tables; crc32c: sse4.2', 'mac': 'crc32: arm; crc32c: arm'}
HOST = {'win32': 'windows', 'darwin': 'mac'}.get(sys.platform, 'linux')


def rosetta():
    """Whether this is an Apple silicon Mac that runs x86-64 programs."""
    if sys.platform != 'darwin' or platform.machine() != 'arm64' or not shutil.which('clang'):
        return False
    try:
        return subprocess.run(['arch', '-x86_64', '/usr/bin/true'], capture_output=True, timeout=60).returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False


class HelperTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = Path(tempfile.mkdtemp(prefix='recomp-helpers-'))

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.dir, ignore_errors=True)

    def build_and_run(self, name):
        llvm = os.environ.get('SSBU_LLVM_BIN')
        try:
            tc = difftest.toolchain(name)
        except SystemExit as e:
            self.skipTest(f'{name}: {e}')
        finally:   # (the rosetta toolchain sets it for tools/linux/toolchain.py; other tests see it as it was)
            if llvm is None:
                os.environ.pop('SSBU_LLVM_BIN', None)
        out = self.dir / name
        out.mkdir()
        builds = [('c', 'O2'), ('c', 'O0')] + ([('cpp', 'O2')] if name != 'windows' else [])
        for lang, level in builds:
            with self.subTest(toolchain=name, lang=lang, level=level):
                src = out / f'check_{level}.{lang}'
                src.write_text(CHECK, encoding='utf-8')
                obj, exe = out / f'check_{lang}_{level}{tc.obj}', out / f'check_{lang}_{level}{tc.exe}'
                cmd = tc.compile_cmd(str(src), str(obj), tc.opts[level])
                if lang == 'cpp':
                    cmd[1:1] = ['-x', 'c++', '-std=gnu++20', '-fexceptions']
                for step in (cmd, tc.link_cmd([str(obj)], str(exe))):
                    r = subprocess.run(step, capture_output=True, text=True, timeout=300)
                    self.assertEqual(r.returncode, 0, f'{" ".join(step)}\n{r.stdout}{r.stderr}')
                r = subprocess.run([*tc.run, str(exe)], capture_output=True, text=True, timeout=300)
                self.assertEqual((r.returncode, r.stdout.splitlines()[-2:]), (0, [EXPECTED[name], 'ok']), r.stdout + r.stderr)

    def test_this_os(self):
        self.build_and_run(HOST)

    @unittest.skipUnless(rosetta(), 'needs an Apple silicon Mac with Rosetta 2 and clang')
    def test_x86_64_under_rosetta(self):
        self.build_and_run('rosetta')


if __name__ == '__main__':
    unittest.main()
