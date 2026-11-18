"""Install/check a commit-pinned Artery firmware library without overwriting caches.

The pinned PlatformIO platform otherwise clones the library at its moving HEAD.
Only the platform-generated package.json is allowed as an untracked file.
"""
from __future__ import annotations

import json
from pathlib import Path
import subprocess
import tempfile

SDK_NAME = 'AT32F403A_407_Firmware_Library'
SDK_URL = 'https://github.com/ArteryTek/AT32F403A_407_Firmware_Library.git'
SDK_REV = '29f8bfd95b190fa38070d2e0fbbb9f3871b0b3f5'


class LockError(RuntimeError):
    pass


def git(directory: Path, *args: str) -> str:
    try:
        return subprocess.run(['git', '-C', str(directory), *args], check=True,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              text=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError) as exc:
        raise LockError(f'git failed for {directory}: {exc}') from exc


def validate(path: Path, revision: str = SDK_REV) -> None:
    if not path.is_dir() or path.is_symlink() or not (path / '.git').is_dir():
        raise LockError(f'{path}: expected a git checkout, not a missing directory, symlink or copied package')
    actual = git(path, 'rev-parse', 'HEAD')
    if actual != revision:
        raise LockError(f'{path}: expected commit {revision}, found {actual}; move this cache aside and rebuild')
    # Avoid --ignored: platform writes package metadata here; ignored files can still
    # influence compilation, so reject them as well, except this exact metadata file.
    entries = subprocess.run(['git', '-C', str(path), 'status', '--porcelain=v1',
                              '--untracked-files=all', '--ignored'], check=True,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True).stdout.splitlines()
    unexpected = [line for line in entries if line not in ('?? package.json', '!! package.json')]
    if unexpected:
        raise LockError(f'{path}: SDK has modified/untracked/ignored files: ' + ', '.join(unexpected[:5]))
    meta = path / 'package.json'
    if meta.is_symlink() or not meta.is_file():
        raise LockError(f'{path}: missing/invalid PlatformIO package.json')
    try:
        data = json.loads(meta.read_text(encoding='utf-8'))
        if data.get('name') != SDK_NAME or not isinstance(data.get('version'), str):
            raise ValueError('wrong package metadata')
    except (OSError, ValueError, TypeError) as exc:
        raise LockError(f'{path}: invalid package.json') from exc


def prepare(packages_dir: Path, url: str = SDK_URL, revision: str = SDK_REV) -> Path:
    packages_dir = Path(packages_dir)
    packages_dir.mkdir(parents=True, exist_ok=True)
    dest = packages_dir / SDK_NAME
    if dest.exists() or dest.is_symlink():
        validate(dest, revision)
        return dest
    # Build in a sibling temporary directory; no partially installed SDK is reused.
    with tempfile.TemporaryDirectory(prefix='.omnia-at32-sdk-', dir=packages_dir) as tmp:
        checkout = Path(tmp) / 'checkout'
        checkout.mkdir()
        git(checkout, 'init', '--quiet')
        git(checkout, 'remote', 'add', 'origin', url)
        git(checkout, 'fetch', '--quiet', '--depth=1', 'origin', revision)
        git(checkout, 'checkout', '--quiet', '--detach', 'FETCH_HEAD')
        (checkout / 'package.json').write_text(json.dumps({
            'name': SDK_NAME, 'version': '0.0.0',
            'description': 'OMNIA pinned checkout; revision checked by pre-build script'
        }, indent=2) + '\n', encoding='utf-8')
        validate(checkout, revision)
        try:
            checkout.rename(dest)
        except OSError as exc:
            # Another builder might have created the directory. Never replace it.
            if dest.exists():
                validate(dest, revision)
            else:
                raise LockError(f'cannot install SDK at {dest}: {exc}') from exc
    return dest
