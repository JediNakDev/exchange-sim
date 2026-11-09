"""Small isolated checks for credential creation; never reads real secrets."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


SOURCE = Path(__file__).resolve().parents[2] / "docker/create-secrets.sh"


class ComposeSecretsTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="exsim-compose-secrets-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        (self.root / "docker").mkdir()
        shutil.copy2(SOURCE, self.root / "docker/create-secrets.sh")

    def run_helper(self, environment=None):
        return subprocess.run(["sh", str(self.root / "docker/create-secrets.sh")],
                              env=environment, text=True, capture_output=True, timeout=5)

    def test_creates_credentials_once_and_preserves_bytes(self):
        self.assertEqual(self.run_helper().returncode, 0)
        before = {p.name: p.read_bytes() for p in (self.root / ".secrets").iterdir()}
        self.assertEqual(set(before), {"postgres_password", "app_password"})
        self.assertTrue(all(len(value.strip()) == 64 for value in before.values()))
        self.assertEqual(self.run_helper().returncode, 0)
        self.assertEqual(before, {p.name: p.read_bytes() for p in (self.root / ".secrets").iterdir()})
        self.assertEqual((self.root / ".secrets").stat().st_mode & 0o777, 0o700)

    def test_failed_generation_leaves_no_final_or_temporary_file(self):
        fake_bin = self.root / "bin"
        fake_bin.mkdir()
        openssl = fake_bin / "openssl"
        openssl.write_text("#!/bin/sh\nprintf partial\nexit 1\n")
        openssl.chmod(0o755)
        result = self.run_helper(dict(os.environ, PATH=str(fake_bin) + os.pathsep + os.environ["PATH"]))
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(list((self.root / ".secrets").iterdir()), [])

    def test_rejects_empty_existing_file_without_replacing_it(self):
        (self.root / ".secrets").mkdir()
        empty = self.root / ".secrets/postgres_password"
        empty.touch()
        self.assertNotEqual(self.run_helper().returncode, 0)
        self.assertEqual(empty.read_bytes(), b"")

    def test_rejects_symlink_without_changing_its_target(self):
        (self.root / ".secrets").mkdir()
        target = self.root / "other"
        target.write_text("fixture secret")
        target.chmod(0o600)
        (self.root / ".secrets/postgres_password").symlink_to(target)
        self.assertNotEqual(self.run_helper().returncode, 0)
        self.assertEqual(target.stat().st_mode & 0o777, 0o600)


if __name__ == "__main__":
    unittest.main(verbosity=2)
