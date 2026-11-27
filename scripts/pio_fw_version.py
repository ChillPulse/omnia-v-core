"""PlatformIO pre-script: inject git-derived diagnostics into CPPDEFINES.

Exports OMNIA_FW_GIT_DESCRIBE (git describe --tags --always [--dirty], or "unknown")
and OMNIA_FW_GIT_BUILD (git rev-list --count HEAD). The DSP-kernel semver itself
lives in exactly one place — src/at32/include/omnia_fw_version.h — because a
CHANGELOG-derived version would misreport on documentation-only releases (they do
not bump the DSP fw but would change the top changelog heading).

The SYS_INFO wire format is unchanged; neither value is sent over the wire today.
Never fails the build: on any git error the macros become "unknown"/0 and a
warning is printed.
"""
import subprocess
from pathlib import Path

Import('env')  # noqa: F821 - injected by SCons

project_dir = Path(env.subst('$PROJECT_DIR'))  # noqa: F821

def git(*args):
    try:
        return subprocess.check_output(
            ['git', *args], cwd=str(project_dir), stderr=subprocess.DEVNULL
        ).decode('utf-8', 'replace').strip()
    except Exception:
        return ''

describe = git('describe', '--tags', '--always', '--dirty') or 'unknown'
try:
    build = int(git('rev-list', '--count', 'HEAD') or '0')
except ValueError:
    build = 0

describe_escaped = describe.replace('\\', '/').replace('"', "'")  # keep -D quoting safe
env.Append(CPPDEFINES=[  # noqa: F821
    'OMNIA_FW_GIT_DESCRIBE=\\"%s\\"' % describe_escaped,
    'OMNIA_FW_GIT_BUILD=%d' % build,
])
print('OMNIA fw: git describe %s, build #%d' % (describe, build))
