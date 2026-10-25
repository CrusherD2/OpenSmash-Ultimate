"""The native kernel's wait queues (runtime/kernel.c) through tools/dev/waittest.c, with made-up guest
threads and no game: WaitSynchronization (events, threads ending), userspace mutexes, condition
variables and the address arbiter - a signal before the wait, timeouts, cancels, many waiters and
one signal (first come first served, only the waiters of what was signalled woken, each once), a
wait on several objects, an event set and cleared before its waiters run, waiters that exit, the
result codes, the freeze report's states - and a stress run of each for lost wake-ups; with
SSBU_THREAD_RECYCLING=1 and without. Built with the platform's compiler: clang-cl on Windows (as
tests/test_clock.py builds clocktest.c), clang with the platform layer elsewhere
(runtime/linux/win32.c, with runtime/macos/'s memory and platform parts on macOS; the Windows types
from runtime/macos/include on both). Skipped where there is no such compiler, and on hosts the
platform layer is not written for (Linux on x86-64 and macOS on Apple silicon only, as the game)."""
from pathlib import Path
import os
import platform
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import cc  # noqa: E402

if os.name == 'nt':
    HAVE_CLANG = os.path.exists(cc.CLANG) or shutil.which(cc.CLANG) is not None
else:
    HAVE_CLANG = shutil.which('clang') is not None and platform.machine() in (('arm64',) if sys.platform == 'darwin' else ('x86_64', 'AMD64'))


def build(folder):
    src = str(ROOT / 'tools' / 'dev' / 'waittest.c')
    if os.name == 'nt':
        exe = os.path.join(folder, 'waittest.exe')
        # (as cc.link links the game: kernel_init reserves the guest's areas at fixed addresses, and with
        # high-entropy ASLR Windows can put a heap or a thread stack there first)
        cmd = [cc.CLANG, '/nologo', '/O1', *cc.LINK_FLAGS, *cc.BASE_CFLAGS, src, '/Fe' + exe, '/Fo' + os.path.join(folder, 'waittest.obj'),
               '/link', *cc.LINK_OPTIONS, '/HIGHENTROPYVA:NO']
    else:
        exe = os.path.join(folder, 'waittest')
        layer = ['runtime/linux/win32.c']   # (the platform layer)
        if sys.platform == 'darwin':
            layer += ['runtime/macos/memory.c', 'runtime/macos/platform.c']
        else:
            layer += ['-lpthread']
        cmd = ['clang', '-O1', '-w', '-I', 'runtime/macos/include', '-I', 'runtime/linux/include', '-I', 'runtime', src, *layer, '-o', exe]
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if r.returncode:
        raise AssertionError('waittest.c did not build:\n' + r.stdout + r.stderr)
    return exe


@unittest.skipUnless(HAVE_CLANG, 'needs clang on x86-64 Linux or an Apple silicon Mac, or clang-cl on Windows (deps\\llvm, or Visual Studio\'s C++ Clang tools)')
class KernelWaitTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.mkdtemp(prefix='waittest ')
        cls.exe = build(cls.dir)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.dir, ignore_errors=True)

    def waittest(self, switch=None):
        env = {k: v for k, v in os.environ.items() if k != 'SSBU_THREAD_RECYCLING'}
        if switch is not None:
            env['SSBU_THREAD_RECYCLING'] = switch
        r = subprocess.run([self.exe], capture_output=True, text=True, env=env, timeout=600)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertRegex(r.stdout, r'\d+ checks, 0 failed')
        return r.stdout

    def test_with_recycling(self):
        self.assertIn('thread recycling on', self.waittest('1'))

    def test_without_recycling(self):
        self.assertIn('thread recycling off', self.waittest())


if __name__ == '__main__':
    unittest.main()
