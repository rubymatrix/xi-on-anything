"""Paths shared by the Android tools (android_deps.py, build_android.py, build_android_apk.py).

ANDROID_HOME is the SDK (default ~/Library/Android/sdk), ANDROID_NDK_HOME the NDK (default the SDK's
ndk/27.0.12077973), XI_ANDROID_DEPS the dependency cache (default build/android-deps). The launcher's
phone page sets ANDROID_HOME, ANDROID_NDK_HOME and JAVA_HOME to the tools it fetched.
"""

import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SDK = Path(os.environ.get('ANDROID_HOME', str(Path.home() / 'Library/Android/sdk')))
NDK = Path(os.environ.get('ANDROID_NDK_HOME', str(SDK / 'ndk/27.0.12077973')))
HOST = {'darwin': 'darwin-x86_64', 'win32': 'windows-x86_64'}.get(sys.platform, 'linux-x86_64')
EXE = '.exe' if sys.platform == 'win32' else ''
TC = NDK / 'toolchains/llvm/prebuilt' / HOST / 'bin'
# the NDK's own GNU make (LuaJIT's makefiles), so the host needs none
MAKE = NDK / 'prebuilt' / HOST / 'bin' / ('make' + EXE)
CACHE = Path(os.environ.get('XI_ANDROID_DEPS', str(ROOT / 'build/android-deps'))).resolve()
SDL = CACHE / 'SDL3-3.4.16'  # the source tree android_deps.py unpacks; its Java goes into the APK
