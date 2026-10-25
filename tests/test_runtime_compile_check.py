"""tools/dev/runtime_compile_check.py without compiling anything: the made-up game's tree it lays out
(tools/mac/boottest's modules), the build's commands on each system, what its builds leave out, and
how a run ends (a failed step, a build without its program, the line for the job's page). The real
build runs in CI (.github/workflows/runtime-compile.yml) on Windows, Linux and macOS."""
from pathlib import Path
import contextlib
import io
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools' / 'dev'))
import runtime_compile_check as check  # noqa: E402

TOOLS = str(ROOT / 'tools')
BOOTTEST = ROOT / 'tools' / 'mac' / 'boottest'


class Folder(unittest.TestCase):
    def setUp(self):
        self.top = tempfile.mkdtemp(prefix='runtime check ')
        self.addCleanup(shutil.rmtree, self.top, ignore_errors=True)
        self.tree = os.path.join(self.top, 'made-up game')


class Tree(Folder):
    def test_the_tree_is_boottests_game(self):
        check.make_tree(self.tree)
        for m in ('main', 'subsdk0', 'sdk'):
            for name in ('g0000.c', 'g_module.c'):
                self.assertEqual(Path(self.tree, 'build', m, name).read_text(), (BOOTTEST / m / name).read_text())
            rows = [line.split('\t') for line in Path(self.tree, 'build', m, 'manifest.tsv').read_text().splitlines()]
            self.assertEqual([r[0] for r in rows], ['g0000.c'])
            self.assertTrue(os.path.isfile(os.path.join(self.tree, 'dump', 'modules', m + '.reloc.bin')))

    def test_laid_out_again_the_files_keep_their_dates(self):
        check.make_tree(self.tree)
        src = os.path.join(self.tree, 'build', 'main', 'g0000.c')
        os.utime(src, (1_000_000_000, 1_000_000_000))
        check.make_tree(self.tree)   # (an older date than its object's: not compiled again)
        self.assertEqual(os.path.getmtime(src), 1_000_000_000)


class Steps(unittest.TestCase):
    tree = os.path.join('top', 'made-up game')

    def dirs(self):
        return [os.path.join(self.tree, 'build', m) for m in ('main', 'subsdk0', 'sdk')]

    def test_windows_compiles_each_module_then_links_with_the_runtime(self):
        recomp = os.path.join(TOOLS, 'recomp.py')
        exe = os.path.join(self.tree, 'build', 'ssbu.exe')
        link = ['py', recomp, 'link', '--dirs', ','.join(self.dirs()), '--out', exe]
        self.assertEqual(check.steps(self.tree, 'windows', '6', python='py'),
                         [(f'compile {m}', ['py', recomp, 'compile', d, '--jobs', '6'])
                          for m, d in zip(('main', 'subsdk0', 'sdk'), self.dirs())] + [('compile the runtime and link', link)])
        self.assertEqual(check.program(self.tree, 'windows'), exe)

    def test_linux_builds_vkd3d_shader_only_when_it_is_not_ready(self):
        build = ('compile and link', ['py', os.path.join(TOOLS, 'linux', 'build_linux.py'), '--port', self.tree,
                                      '--out', os.path.join(self.tree, 'build-linux'), '--jobs', '3'])
        self.assertEqual(check.steps(self.tree, 'linux', 3, vkd3d_ready=True, python='py'), [build])
        self.assertEqual(check.steps(self.tree, 'linux', 3, vkd3d_ready=False, python='py'),
                         [('vkd3d-shader', ['py', os.path.join(TOOLS, 'linux', 'build_vkd3d.py'), 'linux']), build])
        self.assertEqual(check.program(self.tree, 'linux'), os.path.join(self.tree, 'build-linux', 'ssbu'))

    def test_macos_builds_the_app_in_the_tree(self):
        out = os.path.join(self.tree, 'build-mac')
        self.assertEqual(check.steps(self.tree, 'macos', 3, python='py'),
                         [('compile and link', ['py', os.path.join(TOOLS, 'mac', 'build_mac.py'), '--port', self.tree,
                                                '--out', out, '--jobs', '3'])])
        self.assertEqual(check.program(self.tree, 'macos'), os.path.join(out, 'WindowSmash.app', 'Contents', 'MacOS', 'ssbu'))

    def test_without_jobs_each_build_takes_its_own(self):
        for host in ('windows', 'linux', 'macos'):
            for _what, command in check.steps(self.tree, host, None, vkd3d_ready=False):
                self.assertNotIn('--jobs', command)


class LeftOut(Folder):
    def test_the_sources_without_an_object(self):
        runtime, objects = os.path.join(self.top, 'runtime'), os.path.join(self.top, 'rt')
        os.makedirs(runtime)
        os.makedirs(objects)
        for name in ('host.c', 'smoke.c', 'host.h', '._host.c'):
            Path(runtime, name).write_text('')
        for name in ('host.obj', 'hle_table.obj', 'smoke.o'):
            Path(objects, name).write_text('')
        self.assertEqual(check.left_out(runtime, objects, '.obj'), ['smoke.c'])
        self.assertEqual(check.left_out(runtime, os.path.join(self.top, 'none'), '.obj'), ['host.c', 'smoke.c'])


class Run(Folder):
    def main(self, argv, host, made=True, fail=None, isa=None):
        """main with each build step recorded rather than run: the last one makes the program (made),
        the one named `fail` fails; isa: what the made-up link's isa.txt says."""
        self.ran = []

        def run(command, cwd):
            self.ran.append((command, cwd))
            if fail and fail in command[1]:
                return subprocess.CompletedProcess(command, 1)
            if made and len(self.ran) == len(check.steps(self.tree, host, None, vkd3d_ready=False)):
                exe = check.program(self.tree, host)
                os.makedirs(os.path.dirname(exe), exist_ok=True)
                Path(exe).write_bytes(b'\0' * 4096)
                if isa:
                    Path(os.path.dirname(exe), 'isa.txt').write_text(isa + '\nchosen\n')
            return subprocess.CompletedProcess(command, 0)
        out = io.StringIO()
        with contextlib.redirect_stdout(out), mock.patch.object(check, 'vkd3d_ready', return_value=False):
            code = check.main(argv, run=run, host=host)
        return code, out.getvalue()

    def test_the_steps_run_in_order_from_the_checkout(self):
        code, out = self.main(['--tree', self.tree], 'linux', isa='x86-64-v2')
        self.assertEqual(code, 0, out)
        self.assertEqual([c for c, _cwd in self.ran], [c for _w, c in check.steps(self.tree, 'linux', None, vkd3d_ready=False)])
        self.assertEqual({cwd for _c, cwd in self.ran}, {check.REPO})
        self.assertIn('linux, x86-64-v2: the runtime compiled and linked into ssbu', out)
        self.assertTrue(os.path.isfile(os.path.join(self.tree, 'build', 'main', 'g0000.c')))

    def test_a_failed_step_stops_the_run(self):
        code, out = self.main(['--tree', self.tree, '--jobs', '2'], 'windows', fail='recomp.py')
        self.assertEqual(code, 1)
        self.assertEqual(len(self.ran), 1)
        self.assertIn('FAILED: compile main (exit status 1)', out)

    def test_a_build_that_ends_without_its_program_fails(self):
        code, out = self.main(['--tree', self.tree], 'macos', made=False)
        self.assertEqual(code, 1)
        self.assertIn('FAILED: the build ended without', out)

    def test_the_line_for_the_jobs_page(self):
        summary = os.path.join(self.top, 'summary.md')
        with mock.patch.dict(os.environ, {'GITHUB_STEP_SUMMARY': summary}):
            code, out = self.main(['--tree', self.tree], 'macos')
        self.assertEqual(code, 0, out)
        line = Path(summary).read_text()
        self.assertRegex(line, r'^macos, arm64: the runtime compiled and linked into ssbu \(0\.0 MB\) in \d+ s; '
                               r'runtime/\*\.c not in it: .*\bhost\.c\b')   # (no object folder: every source left out)
        self.assertIn(line.strip(), out)

    def test_not_in_this_checkout(self):
        code, out = self.main(['--tree', check.REPO], 'macos')
        self.assertEqual((code, self.ran), (2, []))
        self.assertIn("this checkout, whose build/ is the real game's", out)

    def test_jobs_is_a_number_of_files(self):
        for jobs in ('many', '0'):
            code, out = self.main(['--tree', self.tree, '--jobs', jobs], 'macos')
            self.assertEqual((code, self.ran), (2, []))
            self.assertIn(f'--jobs {jobs}: not a number of files', out)
        self.assertFalse(os.path.exists(self.tree))


if __name__ == '__main__':
    unittest.main()
