"""PlatformIO pre-script: pin and validate the firmware library before builder use."""
from pathlib import Path
import sys

Import('env')  # noqa: F821 - injected by SCons
sys.path.insert(0, str(Path(env.subst('$PROJECT_DIR')) / 'scripts'))
from at32_sdk_lock import LockError, SDK_REV, prepare

compiler = env.PioPlatform().get_package_dir('toolchain-gccarmnoneeabi')
if not compiler:
    raise RuntimeError('AT32 compiler package is unavailable; cannot locate SDK cache')
try:
    sdk = prepare(Path(compiler).parent)
except LockError as exc:
    raise RuntimeError('AT32 SDK lock failed: ' + str(exc)) from exc
print('OMNIA AT32 SDK:', sdk, 'commit', SDK_REV)
