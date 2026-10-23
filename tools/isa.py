"""The instruction set the game is compiled for, on x86-64 (the Windows and Linux builds; the macOS
build is arm64 and has none of this):

  avx2       the default: AVX2, FMA, BMI1, BMI2 and LZCNT (Intel since Haswell, 2013; AMD since
             Excavator, 2015). What every build was before there was a choice.
  x86-64-v2  for older processors: SSE4.2 and POPCNT (Intel since Nehalem, 2008; AMD since
             Bulldozer, 2011). Expected to be slower (how much is not measured): fma() is a library
             call there, and nothing is vectorised wider than 128 bits.

The lifter's C asks for no extension by name: fma() (ARM's fused multiply-add, one rounding on
every target), __builtin_roundeven and the bit counts are plain C, and half floats and TBL are
done in software (runtime/recomp.h). So the choice is only the compile flags: CLANG_CL (tools/cc.py),
CLANG (tools/linux/toolchain.py) and opus's defines (tools/codec_libs.py). x86-64-v2 is the floor:
below it the rounding functions become library calls too, and Windows' C runtime has no roundeven.

Which one (choose): SSBU_ISA=avx2 or SSBU_ISA=x86-64-v2 when set (SSBU_ISA=auto: as if unset);
else x86-64-v2 if this PC's processor is known to lack something avx2 needs; else the build
folder's last choice if SSBU_ISA made it (told once, kept); else avx2 if the processor is known to
have it; else, when it cannot be asked, the folder's last choice, else x86-64-v2. A build writes
what it compiled for into its folder's isa.txt (record): build/isa.txt on Windows, build-linux/isa.txt
on Linux, which build_game.py compares with today's choice (another one builds again; tools/objcache.py
then compiles every object again, their command having changed).

The program checks the processor itself before anything else of it runs (runtime/cpu_check.c): one
built for AVX2 on a processor without it says what is missing and what to do, instead of crashing."""
import os
import sys

TARGETS = ("avx2", "x86-64-v2")
DEFAULT = "avx2"
ANY = "x86-64"   # (not a target: what runtime/cpu_check.c is compiled for, so that it runs on any x86-64 processor)
# what each needs of the processor, in the names cpu_features gives
V2 = ("sse3", "ssse3", "sse4.1", "sse4.2", "popcnt", "cx16")
NEEDS = {"avx2": V2 + ("avx", "avx2", "fma", "bmi1", "bmi2", "lzcnt"), "x86-64-v2": V2}
# The compile flags of each. clang-cl's /arch:AVX2 is -march=haswell (AVX2, FMA, BMI1, BMI2, LZCNT,
# MOVBE, F16C); its x64 default is x86-64 (SSE2). /clang: hands -march to clang itself: a clang-cl
# that does not know -march as its own would leave it out with a warning that -w hides.
CLANG_CL = {"avx2": ("/arch:AVX2", "-mfma"), "x86-64-v2": ("/clang:-march=x86-64-v2",), ANY: ()}
CLANG = {"avx2": ("-mavx2", "-mfma", "-mbmi", "-mbmi2", "-mlzcnt", "-mpopcnt"), "x86-64-v2": ("-march=x86-64-v2",),
         ANY: ("-march=x86-64",)}
RECORD = "isa.txt"

# /proc/cpuinfo's names (Linux lists avx only when the kernel saves the YMM registers; abm is LZCNT)
CPUINFO = {"pni": "sse3", "ssse3": "ssse3", "sse4_1": "sse4.1", "sse4_2": "sse4.2", "popcnt": "popcnt", "cx16": "cx16",
           "avx": "avx", "avx2": "avx2", "fma": "fma", "bmi1": "bmi1", "bmi2": "bmi2", "abm": "lzcnt"}
# Windows' IsProcessorFeaturePresent: PF_SSE3_INSTRUCTIONS_AVAILABLE, PF_COMPARE_EXCHANGE128 and, from
# Windows 10 version 2004 (build 19041), PF_SSSE3/SSE4_1/SSE4_2/AVX/AVX2_INSTRUCTIONS_AVAILABLE
PF = {13: "sse3", 14: "cx16", 36: "ssse3", 37: "sse4.1", 38: "sse4.2", 39: "avx", 40: "avx2"}
XSTATE_AVX = 1 << 2   # GetEnabledXStateFeatures: the system saves the YMM registers


def cpuinfo_features(text):
    """The extensions in /proc/cpuinfo's first flags line (names as in NEEDS), or None without one."""
    for line in text.splitlines():
        key, _, value = line.partition(":")
        if key.strip() == "flags":
            return frozenset(CPUINFO[f] for f in value.split() if f in CPUINFO)
    return None


def windows_features(build, present, xstate):
    """The extensions Windows says the processor has (present(PF number), xstate(): the enabled XSTATE
    mask), or None before build 19041, which cannot be asked about SSSE3 to AVX2. Windows has no flag
    for POPCNT, FMA, BMI1, BMI2 or LZCNT: every processor with SSE4.2 has POPCNT and every one with
    AVX2 the rest (runtime/cpu_check.c asks the processor about each as the game starts)."""
    if build < 19041:
        return None
    has = {name for pf, name in PF.items() if present(pf)}
    if not xstate() & XSTATE_AVX:
        has -= {"avx", "avx2"}
    if "sse4.2" in has:
        has.add("popcnt")
    if "avx2" in has:
        has |= {"fma", "bmi1", "bmi2", "lzcnt"}
    return frozenset(has)


def cpu_features():
    """The extensions this PC's processor has and its system saves the registers of, or None when that
    cannot be told (another system, an older Windows, no /proc/cpuinfo)."""
    try:
        if sys.platform == "win32":
            import ctypes
            k32 = ctypes.windll.kernel32
            k32.GetEnabledXStateFeatures.restype = ctypes.c_uint64
            return windows_features(sys.getwindowsversion().build, lambda pf: bool(k32.IsProcessorFeaturePresent(pf)),
                                    k32.GetEnabledXStateFeatures)
        if sys.platform.startswith("linux"):
            with open("/proc/cpuinfo", encoding="utf-8", errors="replace") as f:
                return cpuinfo_features(f.read())
    except (OSError, AttributeError, ValueError):
        pass
    return None


def best(features):
    """The fastest target these extensions run, or None below x86-64-v2."""
    return next((t for t in TARGETS if set(NEEDS[t]) <= set(features)), None)


def missing(target, features):
    """What of `target`'s needs these extensions lack, in NEEDS' order."""
    return [f for f in NEEDS[target] if f not in features]


def choose(env, features, record):
    """(target, told: SSBU_ISA chose it, why: a few words for the build's log) for the environment
    `env`, this processor's extensions (None: unknown) and the folder's record ((target, told) or None)."""
    want = (env.get("SSBU_ISA") or "").strip().lower()
    if want in TARGETS:
        return want, True, "SSBU_ISA"
    if want not in ("", "auto"):
        raise SystemExit(f"SSBU_ISA={env['SSBU_ISA']}: the instruction set to build for is one of {', '.join(TARGETS)} "
                         "(or auto: this processor's)")
    last, told = record or (None, False)
    if features is not None and best(features) != "avx2":
        lacks = ", ".join(missing("avx2", features))
        if best(features) is None:
            return "x86-64-v2", False, f"this processor lacks {', '.join(missing('x86-64-v2', features))}: WindowSmash cannot run on it"
        return "x86-64-v2", False, f"this processor lacks {lacks}: the build for older processors, expected to be slower"
    if told and not want:
        return last, True, f"as SSBU_ISA chose for the last build ({RECORD}; SSBU_ISA=auto chooses by the processor)"
    if features is not None:
        return "avx2", False, "this processor has AVX2"
    if last:
        return last, False, "as the last build: the processor could not be asked"
    return "x86-64-v2", False, ("the processor could not be asked whether it has AVX2: the build for older processors, "
                                "expected to be slower (SSBU_ISA=avx2 builds for AVX2)")


def recorded(folder):
    """(target, told) from a build folder's record, or None."""
    try:
        with open(os.path.join(folder, RECORD), encoding="utf-8") as f:
            lines = f.read().splitlines()
    except OSError:
        return None
    if not lines or lines[0].strip() not in TARGETS:
        return None
    return lines[0].strip(), "told" in lines[1:2]


def built_for(folder):
    """What a build folder's objects are compiled for: its record's, else avx2 (every build was, before
    the record was written)."""
    return (recorded(folder) or (DEFAULT, False))[0]


def record(folder, target, told):
    """Writes a build folder's record: what it was built for, and whether SSBU_ISA said so (then kept)."""
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(folder, RECORD), "w", encoding="utf-8", newline="\n") as f:
        f.write(f"{target}\n{'told' if told else 'chosen'}\n# the instruction set this folder's build is compiled for "
                f"(tools/isa.py): {' or '.join(TARGETS)}; told: SSBU_ISA chose it, and later builds keep it\n")


def target(folder):
    """choose() for this process: its environment, this PC's processor and `folder`'s record."""
    return choose(os.environ, cpu_features(), recorded(folder))
