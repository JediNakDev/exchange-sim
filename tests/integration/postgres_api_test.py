"""Real PostgreSQL + native HTTP tests. Requires CREATEDB on the local PG user.

Run: python3 tests/integration/postgres_api_test.py [--binary PATH]
Uses standard PGHOST/PGPORT/PGUSER/PGPASSWORD settings. Creates and drops only
uniquely named exsim_it_<hex> databases; never touches exsim_local or user data.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import select
import shutil
import signal
import subprocess
import tempfile
import time
import unittest
import urllib.error
import urllib.request
import uuid


BINARY = Path("build/release/exsim_api").resolve()


class PostgresApiTests(unittest.TestCase):
    def setUp(self):
        self.database = "exsim_it_" + uuid.uuid4().hex
        self.temporary = tempfile.TemporaryDirectory(prefix="exsim-pg-")
        self.root = Path(self.temporary.name).resolve() / "artifacts"
        self.processes = []
        self.logs = []
        subprocess.run(["createdb", self.database], check=True, capture_output=True, timeout=10)
        self.addCleanup(self.cleanup)

    def cleanup(self):
        for process in self.processes:
            self.stop(process)
        for log in self.logs:
            log.close()
        subprocess.run(["dropdb", "--force", self.database], check=True, capture_output=True, timeout=10)
        self.temporary.cleanup()

    def sql(self, statement):
        return subprocess.run(["psql", "-X", "-w", "-At", "-v", "ON_ERROR_STOP=1", "-d", self.database],
                              input=statement, text=True, check=True, capture_output=True, timeout=10).stdout.strip()

    def launch(self, *, postgres=True, extra=(), root=None, ready=True):
        environment = os.environ.copy()
        environment.pop("EXSIM_DATABASE_URL", None)
        if postgres:
            environment["EXSIM_DATABASE_URL"] = "dbname=" + self.database
        log = tempfile.TemporaryFile(mode="w+")
        self.logs.append(log)
        process = subprocess.Popen([str(BINARY), "--port", "0", "--data-dir", str(root or self.root),
                                    "--max-live-runs", "1", *extra], env=environment,
                                   stdout=subprocess.PIPE, stderr=log, text=True)
        self.processes.append(process)
        if not ready:
            self.assertNotEqual(process.wait(timeout=10), 0)
            log.seek(0)
            return log.read()
        self.assertTrue(select.select([process.stdout], [], [], 10)[0], "API startup timed out")
        line = process.stdout.readline().strip()
        if not line.startswith("exsim_api listening on "):
            log.seek(0)
            self.fail("API failed to start: " + log.read())
        self.url = line.removeprefix("exsim_api listening on ")
        return process

    @staticmethod
    def stop(process, abrupt=False):
        if process.poll() is None:
            process.send_signal(signal.SIGKILL if abrupt else signal.SIGTERM)
            process.wait(timeout=10)
        if process.stdout:
            process.stdout.close()

    def request(self, path, body=None, key=None, status=200):
        headers = {"Content-Type": "application/json"}
        if key:
            headers["Idempotency-Key"] = key
        data = json.dumps(body).encode() if body is not None else None
        request = urllib.request.Request(self.url + path, data=data, headers=headers)
        try:
            response = urllib.request.urlopen(request, timeout=30)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            text = response.read().decode()
            self.assertEqual(response.status, status, text)
            return json.loads(text), text

    def experiment(self, key="experiment-key", baseline_id=None):
        baselines, _ = self.request("/v1/baselines")
        baseline = next((item for item in baselines["items"] if item["baselineId"] == baseline_id), baselines["items"][0])
        body = {"name": "Postgres ' roundtrip", "description": "UTF-8 € and SQL text '; DROP TABLE runs;",
                "derivedFromBaselineId": baseline["baselineId"], "config": baseline["config"]}
        result, text = self.request("/v1/experiments", body, key, 201)
        return result, text, body

    def create_run(self, experiment, key="create-run-key", paused=True):
        body = {"experimentId": experiment["experimentId"],
                "masterSeed": experiment["config"]["seedSet"]["masterSeeds"][0], "startPaused": paused}
        return self.request("/v1/runs", body, key, 201)[0]

    def worker_pids(self, process):
        listing = subprocess.run(["ps", "-axo", "pid=,ppid=,command="], text=True,
                                 capture_output=True, check=True, timeout=5).stdout
        result = []
        for line in listing.splitlines():
            fields = line.strip().split(None, 2)
            if len(fields) == 3 and fields[1] == str(process.pid) and "--run-worker-fd 3" in fields[2]:
                result.append(int(fields[0]))
        return result

    def await_state(self, run_id, state):
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            run, _ = self.request("/v1/runs/" + run_id)
            if run["state"] == state:
                return run
            time.sleep(0.02)
        self.fail(f"run did not reach {state}: {run}")

    def assert_audit_record(self, run_id, state):
        export, _ = self.request(f"/v1/runs/{run_id}/audit-export")
        item = next(item for item in export["items"] if item["name"] == "run.json")
        self.assertTrue(item["present"])
        offset = 0
        data = b""
        while True:
            chunk, _ = self.request(f"/v1/runs/{run_id}/audit-export?artifactIndex={item['artifactIndex']}&offset={offset}")
            data += bytes.fromhex(chunk["contentHex"])
            if chunk["nextOffset"] is None:
                break
            offset = int(chunk["nextOffset"])
        self.assertEqual(int(item["sizeBytes"]), len(data))
        self.assertEqual(item["sha256"], hashlib.sha256(data).hexdigest())
        self.assertEqual(json.loads(data)["state"], state)

    def test_metadata_and_exact_replay_survive_restart_without_json_files(self):
        process = self.launch()
        experiment, text, body = self.experiment()
        self.assertEqual(self.sql("SELECT count(*) FROM exsim.experiments"), "1")
        self.assertEqual(list((self.root / "experiments").glob("*.json")), [])
        self.assertEqual(list((self.root / "idempotency").glob("*.json")), [])
        self.stop(process)
        self.launch()
        self.assertEqual(self.request("/v1/experiments", body, "experiment-key", 201)[1], text)
        self.request("/v1/experiments", {**body, "name": "different"}, "experiment-key", 409)
        self.assertEqual(self.request("/v1/experiments/" + experiment["experimentId"])[0]["name"], body["name"])

    def test_run_controls_artifacts_and_completed_restart(self):
        process = self.launch()
        experiment, _, _ = self.experiment()
        run_id = self.create_run(experiment)["runId"]
        self.await_state(run_id, "paused")
        self.request(f"/v1/runs/{run_id}/step", {"canonicalEventCount": 3}, "step-run-key")
        self.request(f"/v1/runs/{run_id}/resume", {}, "resume-run-key")
        self.await_state(run_id, "completed")
        summary = self.request(f"/v1/runs/{run_id}/summary")[1]
        self.assertTrue((self.root / "runs" / run_id / "events.bin").is_file())
        self.assertFalse((self.root / "runs" / run_id / "run.json").exists())
        self.assert_audit_record(run_id, "completed")
        self.stop(process)
        self.launch()
        self.assertEqual(self.request(f"/v1/runs/{run_id}/summary")[1], summary)
        self.assertEqual(self.sql("SELECT state FROM exsim.runs"), "completed")
        self.assert_audit_record(run_id, "completed")

    def test_queued_run_survives_abrupt_restart_and_live_run_fails(self):
        process = self.launch()
        experiment, _, _ = self.experiment()
        first = self.create_run(experiment)["runId"]
        self.await_state(first, "paused")
        second = self.create_run(experiment, "second-run-key")["runId"]
        self.await_state(second, "queued")
        self.stop(process, abrupt=True)
        self.launch()
        self.await_state(first, "failed")
        self.await_state(second, "paused")
        directory = self.root / "runs" / second
        self.assertEqual(json.loads((directory / "manifest.json").read_text())["provenance"],
                         json.loads((directory / "runner/provenance.json").read_text()))

    def test_queued_run_does_not_execute_under_a_different_release(self):
        process = self.launch()
        experiment, _, _ = self.experiment()
        self.await_state(self.create_run(experiment)["runId"], "paused")
        queued = self.create_run(experiment, "queued-release-key")["runId"]
        self.stop(process, abrupt=True)
        self.launch(extra=("--release-id", "different-release"))
        self.await_state(queued, "failed")
        self.assertFalse((self.root / "runs" / queued / "runner").exists())

    def test_child_validates_provenance_after_api_bootstrap(self):
        self.launch()
        experiment, _, _ = self.experiment()
        first = self.create_run(experiment)["runId"]
        self.await_state(first, "paused")
        queued = self.create_run(experiment, "child-provenance-key")["runId"]
        manifest_path = self.root / "runs" / queued / "manifest.json"
        original = manifest_path.read_bytes()
        manifest = json.loads(original)
        manifest["provenance"]["executableSha256"] = "0" * 64
        manifest_path.write_text(json.dumps(manifest))
        try:
            self.request(f"/v1/runs/{first}/cancel", {}, "release-for-child-check")
            self.await_state(queued, "failed")
            self.assertFalse((self.root / "runs" / queued / "runner").exists())
        finally:
            manifest_path.write_bytes(original)

    def test_imported_queued_audit_uses_database_not_retained_file(self):
        process = self.launch(postgres=False)
        experiment, _, _ = self.experiment()
        self.await_state(self.create_run(experiment)["runId"], "paused")
        queued = self.create_run(experiment, "imported-queued-key")["runId"]
        self.stop(process, abrupt=True)
        self.launch(extra=("--import-file-metadata",))
        self.await_state(queued, "paused")
        self.request(f"/v1/runs/{queued}/cancel", {}, "cancel-imported-key")
        self.assertEqual(json.loads((self.root / "runs" / queued / "run.json").read_text())["state"], "queued")
        self.assert_audit_record(queued, "cancelled")

    def test_second_api_owner_is_rejected(self):
        self.launch()
        self.assertIn("another research API owns", self.launch(ready=False))

    def test_database_cannot_be_rebound_to_another_artifact_root(self):
        self.stop(self.launch())
        self.assertIn("another artifact directory", self.launch(root=self.root.parent / "other", ready=False))

    def test_lost_database_session_does_not_release_artifact_ownership(self):
        self.launch()
        self.sql("SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE datname=current_database() AND application_name='exsim_api' AND pid<>pg_backend_pid()")
        self.assertIn("artifact directory", self.launch(ready=False))

    def test_unknown_schema_version_is_rejected(self):
        self.stop(self.launch())
        self.sql("UPDATE exsim.schema_version SET version=999")
        self.assertIn("unsupported PostgreSQL metadata schema", self.launch(ready=False))

    def test_offline_dump_and_artifacts_restore_to_new_database(self):
        process = self.launch()
        experiment, text, body = self.experiment()
        run_id = self.create_run(experiment)["runId"]
        self.await_state(run_id, "paused")
        self.request(f"/v1/runs/{run_id}/cancel", {}, "restore-cancel-key")
        self.stop(process)
        dump = self.root.parent / "metadata.dump"
        subprocess.run(["pg_dump", "-Fc", "-d", self.database, "-f", str(dump)],
                       check=True, capture_output=True, timeout=15)
        restored_root = self.root.parent / "restored"
        shutil.copytree(self.root, restored_root)
        restored_database = "exsim_it_" + uuid.uuid4().hex
        subprocess.run(["createdb", restored_database], check=True, capture_output=True, timeout=10)
        try:
            subprocess.run(["pg_restore", "--exit-on-error", "-d", restored_database, str(dump)],
                           check=True, capture_output=True, timeout=15)
            # Only this verified, disposable restored database is rebound.
            subprocess.run(["psql", "-X", "-w", "-v", "ON_ERROR_STOP=1", "-d", restored_database],
                           input="UPDATE exsim.schema_version SET artifact_root='" + str(restored_root).replace("'", "''") + "';",
                           text=True, check=True, capture_output=True, timeout=10)
            original_database = self.database
            try:
                self.database = restored_database
                restored = self.launch(root=restored_root)
                self.assertEqual(self.request("/v1/experiments", body, "experiment-key", 201)[1], text)
                self.assert_audit_record(run_id, "cancelled")
                self.stop(restored)
            finally:
                self.database = original_database
        finally:
            # Ensure only this test's uniquely named restored database is removed.
            subprocess.run(["dropdb", "--force", restored_database], check=True, capture_output=True, timeout=10)

    def test_postgres_directory_refuses_legacy_fallback(self):
        self.stop(self.launch())
        self.assertIn("file fallback is unsafe", self.launch(postgres=False, ready=False))

    def test_file_import_is_explicit_and_preserves_every_original_file(self):
        process = self.launch(postgres=False)
        experiment, text, body = self.experiment()
        run_id = self.create_run(experiment)["runId"]
        self.await_state(run_id, "paused")
        self.request(f"/v1/runs/{run_id}/cancel", {}, "cancel-run-key")
        self.stop(process)
        before = {p.relative_to(self.root): p.read_bytes() for p in self.root.rglob("*") if p.is_file()}
        self.assertIn("explicit --import-file-metadata", self.launch(ready=False))
        imported = self.launch(extra=("--import-file-metadata",))
        self.assertEqual(self.request("/v1/experiments", body, "experiment-key", 201)[1], text)
        self.assertEqual(self.sql("SELECT state FROM exsim.runs"), "cancelled")
        self.assertEqual(self.request(f"/v1/runs/{run_id}")[0]["state"], "cancelled")
        for relative, data in before.items():
            self.assertEqual((self.root / relative).read_bytes(), data)
        self.stop(imported)
        self.assertIn("empty database", self.launch(extra=("--import-file-metadata",), ready=False))

    def test_invalid_import_rolls_back_all_metadata(self):
        process = self.launch(postgres=False)
        self.experiment()
        self.stop(process)
        (self.root / "idempotency" / "corrupt.json").write_text("{}")
        self.launch(extra=("--import-file-metadata",), ready=False)
        self.assertEqual(self.sql("SELECT count(*) FROM exsim.experiments"), "0")
        self.assertFalse((self.root / ".postgres-metadata").exists())

    def test_creation_failure_rolls_back_resource_and_preserves_reservation(self):
        self.launch()
        self.sql("""CREATE FUNCTION exsim.reject_response() RETURNS trigger LANGUAGE plpgsql AS $$
          BEGIN IF (NEW.record::jsonb->>'status')::integer > 0 THEN RAISE EXCEPTION 'injected response failure'; END IF; RETURN NEW; END $$;
          CREATE TRIGGER reject_response BEFORE UPDATE ON exsim.idempotency FOR EACH ROW EXECUTE FUNCTION exsim.reject_response();""")
        baselines, _ = self.request("/v1/baselines")
        baseline = baselines["items"][0]
        body = {"name": "rollback", "description": "", "derivedFromBaselineId": baseline["baselineId"], "config": baseline["config"]}
        self.request("/v1/experiments", body, "rollback-experiment", 503)
        self.assertEqual(self.sql("SELECT count(*) FROM exsim.experiments"), "0")
        self.assertEqual(self.sql("SELECT status FROM exsim.idempotency"), "0")
        self.request("/v1/experiments", status=503)
        for process in self.processes:
            self.stop(process)
        self.launch()
        self.request("/v1/experiments", body, "rollback-experiment", 503)
        self.assertEqual(self.request("/v1/experiments")[0]["items"], [])

    def test_database_connection_loss_does_not_fall_back_to_files(self):
        process = self.launch()
        experiment, _, _ = self.experiment()
        self.sql("SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE datname=current_database() AND application_name='exsim_api' AND pid<>pg_backend_pid()")
        self.request("/v1/runs", {"experimentId": experiment["experimentId"],
                     "masterSeed": experiment["config"]["seedSet"]["masterSeeds"][0]}, "lost-connection-key", 503)
        self.assertEqual(self.sql("SELECT count(*) FROM exsim.runs"), "0")
        self.assertEqual(list((self.root / "idempotency").glob("*.json")), [])
        self.stop(process)
        self.launch()
        self.assertEqual(len(self.request("/v1/experiments")[0]["items"]), 1)

    def test_worker_crash_fails_only_its_run_and_releases_capacity(self):
        process = self.launch()
        experiment, _, _ = self.experiment()
        first = self.create_run(experiment)["runId"]
        self.await_state(first, "paused")
        workers = self.worker_pids(process)
        self.assertEqual(len(workers), 1)
        queued = self.create_run(experiment, "after-crash-key")["runId"]
        self.await_state(queued, "queued")
        os.kill(workers[0], signal.SIGKILL)
        self.await_state(first, "failed")
        self.await_state(queued, "paused")
        self.request("/v1/service")
        self.assertEqual(len(self.worker_pids(process)), 1)

    def test_file_mode_worker_failure_is_persisted_in_authoritative_run_json(self):
        process = self.launch(postgres=False)
        experiment, _, _ = self.experiment()
        run_id = self.create_run(experiment)["runId"]
        self.await_state(run_id, "paused")
        workers = self.worker_pids(process)
        self.assertEqual(len(workers), 1)
        os.kill(workers[0], signal.SIGKILL)
        self.await_state(run_id, "failed")
        self.assertEqual(json.loads((self.root / "runs" / run_id / "run.json").read_text())["state"], "failed")

    def test_partial_unacknowledged_caches_do_not_break_api_restart(self):
        process = self.launch()
        experiment, _, _ = self.experiment()
        run_id = self.create_run(experiment)["runId"]
        self.await_state(run_id, "paused")
        worker = self.worker_pids(process)[0]
        os.kill(worker, signal.SIGSTOP)
        directory = self.root / "runs" / run_id
        (directory / "events.bin").write_bytes(b"interrupted cache fixture")
        os.kill(worker, signal.SIGKILL)
        self.await_state(run_id, "failed")
        self.assertFalse((directory / "events.bin").exists())
        self.assertEqual(len(list(directory.glob("interrupted_*/events.bin"))), 1)
        self.stop(process)
        self.launch()
        self.await_state(run_id, "failed")

    def test_worker_timeout_cannot_acknowledge_a_later_control(self):
        process = self.launch()
        experiment, _, _ = self.experiment()
        run_id = self.create_run(experiment)["runId"]
        self.await_state(run_id, "paused")
        workers = self.worker_pids(process)
        self.assertEqual(len(workers), 1)
        os.kill(workers[0], signal.SIGSTOP)
        try:
            self.request(f"/v1/runs/{run_id}/summary", status=503)
        finally:
            try:
                os.kill(workers[0], signal.SIGCONT)
            except ProcessLookupError:
                pass
        self.await_state(run_id, "failed")
        self.request(f"/v1/runs/{run_id}/cancel", {}, "cancel-after-timeout", 409)
        self.assertEqual(self.sql("SELECT count(*) FROM exsim.idempotency WHERE key='cancel-after-timeout'"), "0")

    def test_worker_advances_with_api_suspended_and_no_ipc_consumption(self):
        process = self.launch()
        baselines, _ = self.request("/v1/baselines")
        baseline = baselines["items"][0]
        config = baseline["config"]
        # This proves control-plane independence, not a throughput target.
        # Real trader/exchange IPC makes the old ten-minute workload sensitive
        # to shared-runner CPU contention. Keep the existing 15-second deadline
        # with one simulated minute and verify all work completes before SIGCONT.
        config["bounds"]["endSimulationTimeNs"] = "60000000000"
        experiment = self.request("/v1/experiments", {"name": "independent worker", "description": "",
            "derivedFromBaselineId": baseline["baselineId"], "config": config}, "independent-experiment", 201)[0]
        run_id = self.create_run(experiment)["runId"]
        self.await_state(run_id, "paused")
        self.assertEqual(len(self.worker_pids(process)), 1)
        self.assertEqual(self.sql("SELECT count(*) FROM pg_stat_activity WHERE datname=current_database() AND application_name='exsim_api'"), "1")
        self.request(f"/v1/runs/{run_id}/resume", {}, "independent-resume")
        os.kill(process.pid, signal.SIGSTOP)
        try:
            deadline = time.monotonic() + 15
            record_path = self.root / "runs" / run_id / ".worker-record.json"
            self.assertEqual(json.loads(record_path.read_text())["state"], "running",
                             "fixture completed before API suspension")
            while time.monotonic() < deadline:
                record = json.loads(record_path.read_text())
                if record["state"] == "completed":
                    self.assertGreater(int(record["processedEventCount"]), 0)
                    self.assertEqual(record["finalSimulationTimeNs"], config["bounds"]["endSimulationTimeNs"])
                    # Only the suspended API may publish PostgreSQL metadata.
                    self.assertEqual(self.sql("SELECT state FROM exsim.runs"), "running")
                    break
                self.assertEqual(record["state"], "running", record)
                time.sleep(0.02)
            else:
                self.fail(f"worker did not finish while API was suspended: {record}")
        finally:
            os.kill(process.pid, signal.SIGCONT)
        self.await_state(run_id, "completed")

    def test_paused_workers_are_bounded_and_queued_cancel_never_starts_one(self):
        process = self.launch()
        experiment, _, _ = self.experiment()
        self.await_state(self.create_run(experiment)["runId"], "paused")
        queued = self.create_run(experiment, "queued-cancel-key")["runId"]
        self.await_state(queued, "queued")
        self.assertEqual(len(self.worker_pids(process)), 1)
        self.request(f"/v1/runs/{queued}/cancel", {}, "cancel-without-worker")
        self.await_state(queued, "cancelled")
        self.assertFalse((self.root / "runs" / queued / "runner").exists())
        self.assertEqual(len(self.worker_pids(process)), 1)
        self.assert_audit_record(queued, "cancelled")

    def test_postgres_and_file_metadata_preserve_native_and_inspection_artifacts(self):
        process = self.launch()
        experiment, _, _ = self.experiment()
        run_id = self.create_run(experiment, paused=False)["runId"]
        self.await_state(run_id, "completed")
        isolated = self.root / "runs" / run_id
        # Replay-based reads and comparisons also execute in cold workers.
        self.request(f"/v1/runs/{run_id}/book?eventPosition=0&view=exchange_truth&depth=5")
        self.request(f"/v1/runs/{run_id}/execution-trace", status=409)
        views = {name: self.request(f"/v1/runs/{run_id}/{name}")[0]
                 for name in ("summary", "events?limit=100", "fills?limit=100", "series")}
        self.stop(process)
        direct_root = self.root.parent / "file-metadata"
        self.launch(postgres=False, root=direct_root)
        direct_experiment, _, _ = self.experiment()
        direct_id = self.create_run(direct_experiment, paused=False)["runId"]
        self.await_state(direct_id, "completed")
        direct = direct_root / "runs" / direct_id
        def without_api_ids(value):
            if isinstance(value, dict):
                return {key: without_api_ids(item) for key, item in value.items() if key not in ("runId", "experimentId")}
            if isinstance(value, list):
                return [without_api_ids(item) for item in value]
            return value
        # Inspection caches contain native struct padding, so compare their
        # public field values rather than treating raw cache bytes as canonical.
        for name, expected in views.items():
            actual = self.request(f"/v1/runs/{direct_id}/{name}")[0]
            self.assertEqual(without_api_ids(expected), without_api_ids(actual), name)
        for file in (isolated / "runner").iterdir():
            if file.name not in ("status.json",) and not file.name.endswith(".tmp"):
                self.assertEqual(hashlib.sha256(file.read_bytes()).hexdigest(),
                                 hashlib.sha256((direct / "runner" / file.name).read_bytes()).hexdigest(), file.name)

    def test_graceful_api_shutdown_reaps_workers(self):
        process = self.launch()
        experiment, _, _ = self.experiment()
        run_id = self.create_run(experiment)["runId"]
        self.await_state(run_id, "paused")
        workers = self.worker_pids(process)
        self.assertEqual(len(workers), 1)
        self.stop(process)
        for pid in workers:
            with self.assertRaises(ProcessLookupError):
                os.kill(pid, 0)
        self.assertEqual(self.sql("SELECT state FROM exsim.runs"), "failed")

    def test_multiple_runs_have_separate_processes_and_independent_controls(self):
        process = self.launch(extra=("--max-live-runs", "2"))
        experiment, _, _ = self.experiment()
        first = self.create_run(experiment)["runId"]
        second = self.create_run(experiment, "independent-second")["runId"]
        self.await_state(first, "paused")
        before = self.await_state(second, "paused")["progress"]
        self.assertEqual(len(self.worker_pids(process)), 2)
        self.request(f"/v1/runs/{first}/step", {"canonicalEventCount": 20}, "step-only-first")
        self.assertEqual(self.request(f"/v1/runs/{second}")[0]["progress"], before)

    def test_orphaned_writer_retains_ownership_until_stopped(self):
        process = self.launch()
        experiment, _, _ = self.experiment()
        run_id = self.create_run(experiment)["runId"]
        self.await_state(run_id, "paused")
        workers = self.worker_pids(process)
        self.assertEqual(len(workers), 1)
        os.kill(workers[0], signal.SIGSTOP)
        directory = self.root / "runs" / run_id
        (directory / "events.bin").write_bytes(b"API crash during cache publication")
        try:
            self.stop(process, abrupt=True)
            self.assertIn("artifact directory", self.launch(ready=False))
        finally:
            try:
                os.kill(workers[0], signal.SIGKILL)
            except ProcessLookupError:
                pass
        self.launch()
        self.await_state(run_id, "failed")
        self.assertFalse((directory / "events.bin").exists())
        self.assertEqual(len(list(directory.glob("interrupted_*/events.bin"))), 1)
        for running in self.processes:
            self.stop(running)
        self.launch()
        self.await_state(run_id, "failed")

    def test_completed_execution_trace_and_comparison_use_cold_workers(self):
        process = self.launch(extra=("--max-live-runs", "2"))
        baseline, _, _ = self.experiment("twap-experiment-key", "twap-execution")
        candidate, _, _ = self.experiment("sweep-experiment-key", "immediate-sweep-execution")
        first = self.create_run(baseline, "twap-run-key", paused=False)["runId"]
        second = self.create_run(candidate, "sweep-run-key", paused=False)["runId"]
        self.await_state(first, "completed")
        self.await_state(second, "completed")
        self.request(f"/v1/runs/{first}/execution-trace?limit=10")
        comparison, _ = self.request(f"/v1/comparisons?baselineExperimentId={baseline['experimentId']}&candidateExperimentId={candidate['experimentId']}&maxPoints=20")
        self.assertTrue(comparison["comparable"])
        # Finished workers and per-request query workers are reaped.
        self.assertEqual(self.worker_pids(process), [])


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, default=BINARY)
    options, remaining = parser.parse_known_args()
    BINARY = options.binary.resolve()
    unittest.main(argv=[__file__, *remaining], verbosity=2)
