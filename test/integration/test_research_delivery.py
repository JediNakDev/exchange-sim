#!/usr/bin/env python3
"""Focused disposable delivery tests. No real binaries, data or services needed."""

from argparse import Namespace
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/deployment"))
import research_delivery as delivery


class DeliveryTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="exsim-delivery-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.api = self.root / "built-api"
        self.api.write_bytes(b"fixture API, never executed\n")
        self.api.chmod(0o700)
        for name in ("exsim_exchanged", "exsim_traderd"):
            worker = self.root / name
            worker.write_bytes(b"fixture worker, never executed\n")
            worker.chmod(0o700)
        self.standalone = self.root / "standalone"
        (self.standalone / ".next/static").mkdir(parents=True)
        (self.standalone / "server.js").write_text("// fixture standalone\n")
        (self.standalone / ".next/BUILD_ID").write_text("fixture-build\n")
        (self.standalone / ".next/static/stale.js").write_text("stale")
        (self.standalone / "public").mkdir()
        (self.standalone / "public/stale.txt").write_text("stale")
        self.static = self.root / "static"
        self.static.mkdir()
        (self.static / "current.js").write_bytes(b"// fixture asset\n")
        self.public = self.root / "public"
        self.public.mkdir()
        (self.public / "icon.svg").write_text("<svg/>")
        self.release = self.root / "release"
        self.data = self.root / "data"
        (self.data / "experiments").mkdir(parents=True)
        (self.data / "runs/run-fixture/native/empty").mkdir(parents=True)
        (self.data / "experiments/fixture.json").write_text('{"fixture":true}\n')
        (self.data / "runs/run-fixture/native/commands.bin").write_bytes(bytes(range(256)))
        (self.data / "runs/run-fixture/run.json").write_text('{"state":"completed"}\n')
        (self.data / "idempotency.jsonl").write_text('{"key":"fixture-only","body":"cached"}\n')
        self.backup = self.root / "backup"

    def package(self, **changes):
        args = dict(api=self.api, standalone=self.standalone, static=self.static, public=self.public,
                    output=self.release, api_revision="a" * 40, web_revision="b" * 40,
                    target="fixture-os-arch", node_version="fixture-node")
        delivery.package(Namespace(**(args | changes)))

    def make_backup(self, **changes):
        delivery.backup(Namespace(**(dict(data=self.data, output=self.backup, writers_stopped=True) | changes)))

    def restore(self, **changes):
        delivery.restore(Namespace(**(dict(backup=self.backup, output=self.root / "restored") | changes)))

    def assert_clean_failure(self, output):
        self.assertFalse(output.exists())
        self.assertFalse(output.with_name(output.name + ".delivery-lock").exists())
        self.assertFalse(list(self.root.glob(".delivery-*")))

    def test_package_complete_pair_and_replaces_stale_assets(self):
        self.package()
        metadata = delivery.verify(self.release, "release")
        self.assertEqual(metadata["api_revision"], "a" * 40)
        self.assertEqual(metadata["web_revision"], "b" * 40)
        self.assertEqual(metadata["web_build_id"], "fixture-build")
        self.assertEqual((self.release / "api/exsim_api").read_bytes(), self.api.read_bytes())
        self.assertFalse((self.release / "frontend/.next/static/stale.js").exists())
        self.assertFalse((self.release / "frontend/public/stale.txt").exists())
        self.assertEqual((self.release / "frontend/public/icon.svg").read_text(), "<svg/>")

    def test_package_requires_both_worker_binaries(self):
        (self.root / "exsim_traderd").unlink()
        with self.assertRaisesRegex(ValueError, "worker must be"):
            self.package()
        self.assertFalse(self.release.exists())

    def test_file_only_backup_rejects_postgres_data(self):
        (self.data / ".postgres-metadata").write_text("exsim.postgres/1\n")
        with self.assertRaisesRegex(ValueError, "coordinated database dump"):
            self.make_backup()
        self.assert_clean_failure(self.backup)

    def test_package_optional_public_removes_stale_public(self):
        self.package(public=None)
        delivery.verify(self.release)
        self.assertFalse((self.release / "frontend/public").exists())

    def test_package_independent_of_source_paths(self):
        self.package()
        self.api.unlink()
        shutil.rmtree(self.standalone)
        shutil.rmtree(self.static)
        shutil.rmtree(self.public)
        delivery.verify(self.release)

    def test_package_missing_assets_and_build_id_fail(self):
        for name in ["server.js", ".next/BUILD_ID"]:
            with self.subTest(name=name):
                path = self.standalone / name
                contents = path.read_bytes()
                path.unlink()
                with self.assertRaises((OSError, ValueError)):
                    self.package()
                path.write_bytes(contents)
                self.assert_clean_failure(self.release)

    def test_package_empty_static_fails(self):
        (self.static / "current.js").unlink()
        with self.assertRaises(ValueError):
            self.package()
        self.assert_clean_failure(self.release)

    def test_package_nonexecutable_api_fails(self):
        self.api.chmod(0o600)
        with self.assertRaises(ValueError):
            self.package()

    def test_package_rejects_environment_and_credentials(self):
        for name in [".env", ".env.production", ".npmrc", ".git"]:
            with self.subTest(name=name):
                private = self.standalone / name
                private.write_text("synthetic credential placeholder")
                with self.assertRaises(ValueError):
                    self.package()
                private.unlink()
                self.assert_clean_failure(self.release)

    def test_package_internal_relative_link_survives(self):
        (self.standalone / "linked-server.js").symlink_to("server.js")
        self.package()
        delivery.verify(self.release)
        self.assertTrue((self.release / "frontend/linked-server.js").is_symlink())

    def test_package_external_absolute_and_broken_links_fail(self):
        for target in ["../built-api", str(self.api), "missing"]:
            with self.subTest(target=target):
                link = self.standalone / "linked"
                link.symlink_to(target)
                with self.assertRaises((OSError, ValueError)):
                    self.package()
                link.unlink()
                self.assert_clean_failure(self.release)

    def test_package_refuses_overlap_and_preserves_input(self):
        with self.assertRaises(ValueError):
            self.package(output=self.standalone / "release")
        self.assertTrue((self.standalone / "server.js").is_file())

    def test_package_existing_release_is_unchanged(self):
        self.package()
        before = delivery.inventory(self.release, allow_links=True)
        with self.assertRaises(ValueError):
            self.package()
        self.assertEqual(before, delivery.inventory(self.release, allow_links=True))

    def test_package_copy_error_rolls_back_staging(self):
        with patch.object(delivery.shutil, "copytree", side_effect=OSError("fixture disk full")):
            with self.assertRaises(OSError):
                self.package()
        self.assert_clean_failure(self.release)
        self.package()

    def test_package_input_mutation_aborts(self):
        original = delivery.shutil.copytree

        def changing_copy(source, destination, *positional, **kwargs):
            result = original(source, destination, *positional, **kwargs)
            if source == self.static:
                (self.static / "current.js").write_text("changed during copy")
            return result

        with patch.object(delivery.shutil, "copytree", side_effect=changing_copy):
            with self.assertRaises(ValueError):
                self.package()
        self.assert_clean_failure(self.release)

    def test_package_corrupt_asset_copy_aborts(self):
        original = delivery.shutil.copytree

        def corrupting_copy(source, destination, *positional, **kwargs):
            result = original(source, destination, *positional, **kwargs)
            if source == self.static:
                (destination / "current.js").write_text("bad copy")
            return result

        with patch.object(delivery.shutil, "copytree", side_effect=corrupting_copy):
            with self.assertRaises(ValueError):
                self.package()
        self.assert_clean_failure(self.release)

    def test_verify_rejects_corruption_missing_extra_and_mode_change(self):
        self.package()
        original = self.release / "api/exsim_api"
        contents = original.read_bytes()
        for mutation in ["corrupt", "missing", "extra", "mode"]:
            with self.subTest(mutation=mutation):
                if mutation == "corrupt":
                    original.write_text("corrupt")
                elif mutation == "missing":
                    original.unlink()
                elif mutation == "extra":
                    (self.release / "extra").write_text("extra")
                else:
                    original.chmod(0o600)
                with self.assertRaises(ValueError):
                    delivery.verify(self.release)
                original.write_bytes(contents)
                original.chmod(0o700)
                (self.release / "extra").unlink(missing_ok=True)

    def test_inventory_never_ignores_directory_read_errors(self):
        with patch.object(delivery.os, "scandir", side_effect=PermissionError("fixture unreadable directory")):
            with self.assertRaises(PermissionError):
                delivery.inventory(self.data)

    def test_verify_rejects_malformed_manifest(self):
        self.package()
        for text in ["{", "[]", '{}', '{"format":"exsim.delivery/1","kind":"unknown"}']:
            with self.subTest(text=text):
                (self.release / delivery.MANIFEST).write_text(text)
                with self.assertRaises(ValueError):
                    delivery.verify(self.release)

    def test_backup_requires_offline_attestation(self):
        with self.assertRaises(ValueError):
            self.make_backup(writers_stopped=False)
        self.assert_clean_failure(self.backup)

    def test_backup_restore_preserves_entire_tree_and_idempotency(self):
        self.make_backup()
        metadata = delivery.verify(self.backup, "backup")
        self.assertEqual(metadata["quiescence"], "operator-declared-writers-stopped")
        self.restore()
        self.assertEqual(delivery.inventory(self.data), delivery.inventory(self.root / "restored"))
        self.assertTrue((self.root / "restored/runs/run-fixture/native/empty").is_dir())
        self.assertEqual((self.root / "restored/idempotency.jsonl").read_bytes(),
                         (self.data / "idempotency.jsonl").read_bytes())

    def test_backup_empty_fresh_data_without_idempotency(self):
        shutil.rmtree(self.data)
        (self.data / "experiments").mkdir(parents=True)
        (self.data / "runs").mkdir()
        self.make_backup()
        self.restore()
        self.assertEqual(delivery.inventory(self.data), delivery.inventory(self.root / "restored"))

    def test_backup_missing_api_data_directories_fails(self):
        shutil.rmtree(self.data / "experiments")
        with self.assertRaises(ValueError):
            self.make_backup()
        self.assert_clean_failure(self.backup)

    def test_backup_rejects_symlinks_and_special_files(self):
        path = self.data / "bad"
        path.symlink_to("runs")
        with self.assertRaises(ValueError):
            self.make_backup()
        path.unlink()
        delivery.os.mkfifo(path)
        with self.assertRaises(ValueError):
            self.make_backup()
        self.assert_clean_failure(self.backup)

    def test_backup_rejects_overlap(self):
        with self.assertRaises(ValueError):
            self.make_backup(output=self.data / "backup")

    def test_backup_changed_source_does_not_publish(self):
        original = delivery.shutil.copytree

        def changing_copy(source, destination, *positional, **kwargs):
            result = original(source, destination, *positional, **kwargs)
            if source == self.data:
                (self.data / "idempotency.jsonl").write_text("changed")
            return result

        with patch.object(delivery.shutil, "copytree", side_effect=changing_copy):
            with self.assertRaises(ValueError):
                self.make_backup()
        self.assert_clean_failure(self.backup)

    def test_backup_corrupt_copy_does_not_publish(self):
        original = delivery.shutil.copytree

        def corrupting_copy(source, destination, *positional, **kwargs):
            result = original(source, destination, *positional, **kwargs)
            if source == self.data:
                (destination / "idempotency.jsonl").write_text("bad copy")
            return result

        with patch.object(delivery.shutil, "copytree", side_effect=corrupting_copy):
            with self.assertRaises(ValueError):
                self.make_backup()
        self.assert_clean_failure(self.backup)

    def test_backup_repeat_never_overwrites(self):
        self.make_backup()
        before = delivery.inventory(self.backup)
        with self.assertRaises(ValueError):
            self.make_backup()
        self.assertEqual(before, delivery.inventory(self.backup))

    def test_restore_refuses_existing_target(self):
        self.make_backup()
        self.restore()
        before = delivery.inventory(self.root / "restored")
        with self.assertRaises(ValueError):
            self.restore()
        self.assertEqual(before, delivery.inventory(self.root / "restored"))

    def test_restore_corrupt_backup_preserves_live_data(self):
        self.make_backup()
        before = delivery.inventory(self.data)
        (self.backup / "data/idempotency.jsonl").write_text("corrupt")
        with self.assertRaises(ValueError):
            self.restore()
        self.assert_clean_failure(self.root / "restored")
        self.assertEqual(before, delivery.inventory(self.data))

    def test_restore_copy_error_cleans_up(self):
        self.make_backup()
        with patch.object(delivery.shutil, "copytree", side_effect=OSError("fixture disk full")):
            with self.assertRaises(OSError):
                self.restore()
        self.assert_clean_failure(self.root / "restored")

    def test_restore_corrupt_copy_cleans_up(self):
        self.make_backup()
        original = delivery.shutil.copytree

        def corrupting_copy(source, destination, *positional, **kwargs):
            result = original(source, destination, *positional, **kwargs)
            if source == self.backup / "data":
                (destination / "idempotency.jsonl").write_text("bad copy")
            return result

        with patch.object(delivery.shutil, "copytree", side_effect=corrupting_copy):
            with self.assertRaises(ValueError):
                self.restore()
        self.assert_clean_failure(self.root / "restored")

    def test_restore_backup_changed_during_copy_aborts(self):
        self.make_backup()
        original = delivery.shutil.copytree

        def changing_copy(source, destination, *positional, **kwargs):
            result = original(source, destination, *positional, **kwargs)
            if source == self.backup / "data":
                (source / "idempotency.jsonl").write_text("changed")
            return result

        with patch.object(delivery.shutil, "copytree", side_effect=changing_copy):
            with self.assertRaises(ValueError):
                self.restore()
        self.assert_clean_failure(self.root / "restored")

    def test_restore_rejects_release_bundle(self):
        self.package()
        with self.assertRaises(ValueError):
            self.restore(backup=self.release)

    def test_destination_lock_does_not_touch_other_run(self):
        lock = self.backup.with_name(self.backup.name + ".delivery-lock")
        lock.mkdir()
        with self.assertRaises(FileExistsError):
            self.make_backup()
        self.assertTrue(lock.is_dir())
        self.assertFalse(self.backup.exists())

    def test_rollback_retains_both_releases(self):
        self.package()
        before = delivery.inventory(self.release, allow_links=True)
        (self.static / "current.js").write_text("candidate")
        candidate = self.root / "candidate"
        self.package(output=candidate)
        self.assertNotEqual(delivery.inventory(candidate, allow_links=True), before)
        # Returning supervisor paths to the previous release needs no file mutation.
        delivery.verify(self.release)
        self.assertEqual(delivery.inventory(self.release, allow_links=True), before)

    @contextmanager
    def fixture_http(self, mode="healthy"):
        service = {"apiVersion": "v1", "schemaVersion": "exsim.api/1", "serviceBuildId": "exsim_api/1",
                   "engineBuildId": "fixture-engine", "executableMarketWorlds": ["interactive_synthetic"],
                   "executableStrategyKinds": ["fixed_spread_market_maker", "inventory_aware_market_maker"]}
        asset = (self.static / "current.js").read_bytes()
        requests = []

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                requests.append(self.path)
                status, content_type, body = 200, "application/json", b""
                if self.path.endswith("/service"):
                    value = dict(service)
                    if mode == "proxy-mismatch" and self.path.startswith("/api/"):
                        value["engineBuildId"] = "other"
                    if mode == "schema":
                        value["schemaVersion"] = "wrong"
                    if mode == "capabilities":
                        value["executableStrategyKinds"] = []
                    body = b"not json" if mode == "json" else json.dumps(value).encode()
                elif self.path == "/experiments":
                    content_type, body = "text/html", b"<!doctype html><html>fixture</html>"
                    if mode == "html":
                        body = b"upstream error"
                elif self.path == "/_next/static/current.js":
                    body = b"stale" if mode == "asset" else asset
                else:
                    status = 404
                if mode == "http-error":
                    status = 503
                if mode == "redirect":
                    status = 302
                self.send_response(status)
                self.send_header("Content-Type", content_type)
                self.send_header("Exsim-Schema-Version", "wrong" if mode == "header" else "exsim.api/1")
                if status == 302:
                    self.send_header("Location", "/v1/service")
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *args):
                pass

        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        worker = threading.Thread(target=server.serve_forever)
        worker.start()
        try:
            yield f"http://127.0.0.1:{server.server_port}", requests
        finally:
            server.shutdown()
            worker.join()
            server.server_close()

    def test_readiness_healthy_read_only(self):
        self.package()
        with self.fixture_http() as (origin, requests):
            delivery.ready(Namespace(release=self.release, api_url=origin, frontend_url=origin))
        self.assertEqual(requests, ["/v1/service", "/api/exsim/v1/service", "/experiments", "/_next/static/current.js"])

    def test_readiness_rejects_bad_responses(self):
        self.package()
        for mode in ["proxy-mismatch", "schema", "capabilities", "json", "html", "asset", "http-error", "redirect", "header"]:
            with self.subTest(mode=mode), self.fixture_http(mode) as (origin, _):
                with self.assertRaises((ValueError, OSError)):
                    delivery.ready(Namespace(release=self.release, api_url=origin, frontend_url=origin))

    def test_readiness_refuses_unsafe_origins_before_network(self):
        self.package()
        for origin in ["http://example.com", "http://user:pass@localhost", "http://localhost/path",
                       "http://localhost?secret=fixture", "file:///tmp/fixture"]:
            with self.subTest(origin=origin), patch.object(delivery, "build_opener") as opener:
                with self.assertRaises(ValueError):
                    delivery.ready(Namespace(release=self.release, api_url=origin, frontend_url=origin))
                opener.assert_not_called()

    def test_cli_requires_full_source_revision(self):
        with self.assertRaises(delivery.argparse.ArgumentTypeError):
            delivery.revision("HEAD")
        self.assertEqual(delivery.revision("c" * 40), "c" * 40)

    def test_cli_failure_exit_has_no_synthetic_record_content(self):
        script = Path(delivery.__file__)
        result = subprocess.run([sys.executable, str(script), "backup", "--data", str(self.data),
                                 "--output", str(self.backup)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn("writers-stopped", result.stderr)
        self.assertNotIn("fixture-only", result.stderr)
        self.assertFalse(self.backup.exists())

    def test_cli_round_trip_from_another_working_directory(self):
        script = str(Path(delivery.__file__).resolve())
        commands = [
            ["package", "--api", str(self.api), "--standalone", str(self.standalone),
             "--static", str(self.static), "--api-revision", "a" * 40, "--web-revision", "b" * 40,
             "--target", "fixture-os-arch", "--node-version", "fixture-node", "--output", str(self.release)],
            ["verify", str(self.release)],
            ["backup", "--data", str(self.data), "--output", str(self.backup), "--writers-stopped"],
            ["verify", str(self.backup)],
            ["restore", "--backup", str(self.backup), "--output", str(self.root / "restored")],
        ]
        for command in commands:
            with self.subTest(command=command[0]):
                result = subprocess.run([sys.executable, "-B", script, *command], cwd=self.root,
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.strip(), command[0] + ": OK")
        self.assertEqual(delivery.inventory(self.data), delivery.inventory(self.root / "restored"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
