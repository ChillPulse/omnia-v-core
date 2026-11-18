import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('at32_sdk_lock', ROOT / 'scripts/at32_sdk_lock.py')
lock = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lock)


def run(path, *args):
    return subprocess.run(['git', '-C', str(path), *args], check=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True).stdout.strip()


class SdkLockTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.repo = self.root / 'repo'
        self.repo.mkdir()
        run(self.repo, 'init', '-q')
        run(self.repo, 'config', 'user.name', 'Test')
        run(self.repo, 'config', 'user.email', 'test@example.invalid')
        (self.repo / 'tracked.c').write_text('int x;\n')
        run(self.repo, 'add', 'tracked.c')
        run(self.repo, 'commit', '-qm', 'first')
        self.commit = run(self.repo, 'rev-parse', 'HEAD')
        self.packages = self.root / 'packages'

    def install(self):
        return lock.prepare(self.packages, self.repo.as_uri(), self.commit)

    def test_install_and_reuse(self):
        path = self.install()
        self.assertEqual(run(path, 'rev-parse', 'HEAD'), self.commit)
        self.assertEqual(lock.prepare(self.packages, self.repo.as_uri(), self.commit), path)
        self.assertEqual(json.loads((path / 'package.json').read_text())['name'], lock.SDK_NAME)

    def test_modified_and_untracked_rejected_without_overwrite(self):
        path = self.install()
        (path / 'tracked.c').write_text('changed')
        with self.assertRaisesRegex(lock.LockError, 'modified'):
            self.install()
        self.assertEqual((path / 'tracked.c').read_text(), 'changed')
        run(path, 'checkout', '--', 'tracked.c')
        (path / 'extra.c').write_text('unexpected')
        with self.assertRaisesRegex(lock.LockError, 'untracked'):
            self.install()

    def test_ignored_file_rejected(self):
        path = self.install()
        (path / '.git' / 'info' / 'exclude').write_text('ignored.c\n')
        (path / 'ignored.c').write_text('not compiled safely')
        with self.assertRaisesRegex(lock.LockError, 'ignored'):
            self.install()

    def test_wrong_revision_and_non_git_cache_rejected(self):
        path = self.install()
        with self.assertRaisesRegex(lock.LockError, 'expected commit'):
            lock.validate(path, '0' * 40)
        (path / 'package.json').unlink()
        with self.assertRaisesRegex(lock.LockError, 'package.json'):
            self.install()
        other = self.root / 'other' / lock.SDK_NAME
        other.mkdir(parents=True)
        with self.assertRaisesRegex(lock.LockError, 'git checkout'):
            lock.prepare(other.parent, self.repo.as_uri(), self.commit)

    def test_fetch_failure_does_not_leave_destination(self):
        with self.assertRaises(lock.LockError):
            lock.prepare(self.packages, self.repo.as_uri(), '0' * 40)
        self.assertFalse((self.packages / lock.SDK_NAME).exists())


if __name__ == '__main__':
    unittest.main()
