# Pre-build script: ensure the 'intelhex' Python package is available.
#
# Newer versions of esptool (used by PlatformIO to flash ESP32 boards) depend
# on 'intelhex', but PlatformIO does not always pull it in automatically.
# This script runs before every build and installs the package if it is absent.

Import("env")  # noqa: F821 – injected by PlatformIO's SCons environment
import subprocess
import sys


def install_intelhex(*args, **kwargs):
    try:
        import intelhex  # noqa: F401
    except ImportError:
        print("intelhex not found – installing now …")
        subprocess.check_call(
            [sys.executable, "-m", "pip", "install", "--quiet", "intelhex"]
        )
        print("intelhex installed successfully.")


install_intelhex()
