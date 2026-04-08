"""
enable_lto.py – PlatformIO extra_script (pre-build)
====================================================
When LTO (-flto) is active, GCC requires its own ``gcc-ar`` / ``gcc-ranlib``
wrappers as the archiver instead of the plain ``ar`` / ``ranlib`` binaries.
Plain ``ar`` cannot read or write the LTO IR bitcode sections that GCC embeds
in object files, which causes a ``file format not recognized`` link error.

This script detects the toolchain directory from the current ``CC`` path and
replaces the SCons ``AR`` / ``RANLIB`` variables with the matching GCC wrappers
(e.g. ``xtensa-esp32-elf-gcc-ar``).  It runs only when ``-flto`` is present in
``BUILD_FLAGS``; if the wrapper binary cannot be found it prints a warning and
leaves the default archiver in place so the build still proceeds (it may just
fail at link time with the original error).

Usage
-----
In ``platformio.ini`` (uncomment both lines together):

    build_flags =
        ...
        -flto

    extra_scripts = pre:firmware/tools/enable_lto.py
"""

Import("env")  # noqa: F821 – SCons injects this
import os

# Only do anything when -flto is actually in the build flags.
build_flags = env.get("BUILD_FLAGS", [])
if not any("-flto" in str(f) for f in build_flags):
    # LTO not requested – nothing to do.
    pass
else:
    cc = env.get("CC", "")
    toolchain_dir = os.path.dirname(cc)
    cc_name = os.path.basename(cc)

    # Derive cross-prefix: "xtensa-esp32-elf-gcc" → "xtensa-esp32-elf"
    # rsplit("-", 1) splits on the LAST hyphen only.
    prefix = cc_name.rsplit("-", 1)[0]

    gcc_ar = os.path.join(toolchain_dir, f"{prefix}-gcc-ar")
    gcc_ranlib = os.path.join(toolchain_dir, f"{prefix}-gcc-ranlib")

    if os.path.isfile(gcc_ar):
        print(f"[enable_lto] Using LTO archiver: {gcc_ar}")
        env.Replace(AR=gcc_ar)
        if os.path.isfile(gcc_ranlib):
            env.Replace(RANLIB=gcc_ranlib)
    else:
        print(
            f"[enable_lto] WARNING: {gcc_ar} not found – "
            "LTO build may fail at link time.\n"
            "  Check that the espressif32 toolchain package is fully installed:\n"
            "    pio pkg install --tool toolchain-xtensa-esp32"
        )
