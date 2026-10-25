#!/usr/bin/env python3
"""The whole runtime (runtime/ and this system's platform layer) compiled and linked into the game's
program, as a player's build does it here, with no game files: the game is tools/mac/boottest's
made-up one (main, subsdk0 and sdk, hand-written in the form tools/recomp.py writes a module's C),
laid out as a game tree. What .github/workflows/runtime-compile.yml runs on Windows, Linux and macOS.
  Windows  tools/recomp.py compile for each module, then tools/recomp.py link: <tree>/build/ssbu.exe
  Linux    tools/linux/build_vkd3d.py linux when vkd3d-shader is not built for this instruction set
           (as build_game.py), then tools/linux/build_linux.py: <tree>/build-linux/ssbu
  macOS    tools/mac/build_mac.py: <tree>/build-mac/WindowSmash.app
The program is not started (tools/mac/boottest/boottest.py starts it on a Mac). SSBU_ISA chooses
the instruction set on Windows and Linux, as for a player's build (tools/isa.py).

Usage: runtime_compile_check.py [--tree <dir>] [--jobs N]
  --tree  the made-up game's folder (default: windowsmash-runtime-check in the system's temporary
          folder; not this checkout, whose build/ is the real game's). Kept: a second run compiles
          only what changed
  --jobs  files compiled at a time (default: each build's own)

The tools are a player's: on Windows clang-cl (tools/cc.py finds it) and SDL3 in deps\\SDL3 if
there; on Linux install-windowsmash.sh's packages and deps/linux-toolchain; on macOS the Command
Line Tools and SDL3.framework in deps/SDL3 (SSBU_SDL3_DIR names another folder).
Exit status: 0 the program linked, 1 a step failed (its output says why), 2 a wrong argument."""
import importlib.util
import os
import subprocess
import sys
import tempfile
import time

TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO = os.path.dirname(TOOLS)
RUNTIME = os.path.join(REPO, "runtime")
BOOTTEST = os.path.join(TOOLS, "mac", "boottest", "boottest.py")
MODULES = ("main", "subsdk0", "sdk")
TREE = os.path.join(tempfile.gettempdir(), "windowsmash-runtime-check")


def arg(argv, name, default=None):
    return argv[argv.index(name) + 1] if name in argv and argv.index(name) + 1 < len(argv) else default


def this_host():
    return "windows" if os.name == "nt" else "macos" if sys.platform == "darwin" else "linux"


def make_tree(tree):
    """The made-up game's tree: boottest.py's (build/<module>/ with the C and manifest.tsv, dump/modules/
    with the images). An unchanged file keeps its date."""
    spec = importlib.util.spec_from_file_location("boottest", BOOTTEST)
    boottest = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(boottest)
    boottest.make_tree(tree)


def program(tree, host):
    """Where the build puts the program: where a player's build puts it in its tree."""
    return {"windows": os.path.join(tree, "build", "ssbu.exe"),
            "linux": os.path.join(tree, "build-linux", "ssbu"),
            "macos": os.path.join(tree, "build-mac", "WindowSmash.app", "Contents", "MacOS", "ssbu")}[host]


def runtime_objects(tree, host):
    """(the folder of the runtime's objects, their extension)."""
    return {"windows": (os.path.join(tree, "build", "rt"), ".obj"),
            "linux": (os.path.join(tree, "build-linux", "rt"), ".o"),
            "macos": (os.path.join(tree, "build-mac", "obj", "rt"), ".o")}[host]


def steps(tree, host, jobs=None, vkd3d_ready=True, python=sys.executable):
    """[(what, command)] of the build, in order; jobs None: each build's own default."""
    more = ["--jobs", str(jobs)] if jobs else []
    dirs = [os.path.join(tree, "build", m) for m in MODULES]
    if host == "windows":
        recomp = os.path.join(TOOLS, "recomp.py")
        link = [python, recomp, "link", "--dirs", ",".join(dirs), "--out", program(tree, host)]
        return [(f"compile {m}", [python, recomp, "compile", d, *more]) for m, d in zip(MODULES, dirs)] + \
            [("compile the runtime and link", link)]
    if host == "linux":
        vkd3d = [("vkd3d-shader", [python, os.path.join(TOOLS, "linux", "build_vkd3d.py"), "linux"])]
        build = [python, os.path.join(TOOLS, "linux", "build_linux.py"), "--port", tree,
                 "--out", os.path.join(tree, "build-linux"), *more]
        return ([] if vkd3d_ready else vkd3d) + [("compile and link", build)]
    return [("compile and link", [python, os.path.join(TOOLS, "mac", "build_mac.py"), "--port", tree,
                                  "--out", os.path.join(tree, "build-mac"), *more])]


def vkd3d_ready():
    """Linux: whether vkd3d-shader is built for the instruction set the build compiles for (build_game.py's
    question). tools/linux/toolchain.py stops here when clang and lld are missing."""
    sys.path.insert(0, os.path.join(TOOLS, "linux"))
    sys.path.insert(1, TOOLS)
    import isa
    import toolchain
    folder = os.path.join(toolchain.VKD3D_BUILD, "linux")
    return os.path.isfile(os.path.join(folder, "libvkd3d-shader.a")) and (isa.recorded(folder) or (None,))[0] == toolchain.ISA


def left_out(runtime, objects, ext):
    """The runtime's own sources (runtime/*.c) with no object in the build's runtime folder: not in this
    system's program (some on purpose: the Direct3D 11 renderer on macOS, the test harnesses)."""
    have = {n[:-len(ext)] for n in os.listdir(objects) if n.endswith(ext)} if os.path.isdir(objects) else set()
    return sorted(n for n in os.listdir(runtime) if n.endswith(".c") and not n.startswith("._") and n[:-2] not in have)


def built_for(tree, host):
    """The instruction set the program is compiled for (the isa.txt its link writes), or arm64 on macOS."""
    if host == "macos":
        return "arm64"
    sys.path.insert(0, TOOLS)
    import isa
    return (isa.recorded(os.path.dirname(program(tree, host))) or ("?",))[0]


def same_path(a, b):
    return os.path.normcase(os.path.abspath(a)) == os.path.normcase(os.path.abspath(b))


def main(argv=None, run=subprocess.run, host=None):
    argv = sys.argv[1:] if argv is None else argv
    if "-h" in argv or "--help" in argv:
        print(__doc__)
        return 0
    host = host or this_host()
    tree = os.path.abspath(arg(argv, "--tree") or TREE)
    jobs = arg(argv, "--jobs")
    if jobs is not None and not (jobs.isdigit() and int(jobs) > 0):
        print(f"--jobs {jobs}: not a number of files")
        return 2
    if same_path(tree, REPO):
        print(f"--tree {tree}: this checkout, whose build/ is the real game's: name another folder")
        return 2
    print(f"the made-up game (tools/mac/boottest's modules) in {tree}", flush=True)
    make_tree(tree)
    if host == "windows":
        sys.path.insert(0, TOOLS)
        import cc
        if not cc.SDL3_DLL:
            print("(no SDL3 in deps\\SDL3: the runtime is compiled without its SDL3 controllers)", flush=True)
    t0 = time.time()
    for what, command in steps(tree, host, jobs, vkd3d_ready() if host == "linux" else True):
        print(f"\n{what}:\n  {subprocess.list2cmdline(command)}", flush=True)
        code = run(command, cwd=REPO).returncode
        if code:
            print(f"\nFAILED: {what} (exit status {code})")
            return 1
    exe = program(tree, host)
    if not os.path.isfile(exe):
        print(f"\nFAILED: the build ended without {exe}")
        return 1
    seconds = time.time() - t0
    out = left_out(RUNTIME, *runtime_objects(tree, host))
    line = (f"{host}, {built_for(tree, host)}: the runtime compiled and linked into {os.path.basename(exe)} "
            f"({os.path.getsize(exe) / 1048576:,.1f} MB) in {seconds:,.0f} s; runtime/*.c not in it: {', '.join(out) or 'none'}")
    print("\n" + line)
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:   # (the job's page on GitHub)
        with open(summary, "a", encoding="utf-8") as f:
            f.write(line + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
