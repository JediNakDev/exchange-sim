"""Exercise protocol selection through the real HTTP API and worker processes.

Uses a disposable file-backed dataset. No installed services or data are used.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time
import urllib.request


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, default=Path("build/release/exsim_apid"))
    options = parser.parse_args()
    environment = {key: value for key, value in os.environ.items()
                   if key not in ("EXSIM_DATABASE_URL", "PGPASSWORD", "PGPASSFILE")}
    with tempfile.TemporaryDirectory(prefix="exsim-research-sessions-") as root:
        process = subprocess.Popen([str(options.binary.resolve()), "--port", "0", "--data-dir", root],
                                   env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            line = process.stdout.readline()
            address = re.search(r"http://127.0.0.1:\d+", line)
            assert address, (line, process.poll())
            sequence = 0

            def request(path, body=None):
                nonlocal sequence
                sequence += 1
                headers = {"Content-Type": "application/json", "Idempotency-Key": f"full-session-{sequence}"}
                data = json.dumps(body).encode() if body is not None else None
                with urllib.request.urlopen(urllib.request.Request(address[0] + path, data=data, headers=headers), timeout=20) as response:
                    return json.load(response)

            def await_state(run_id, state):
                deadline = time.monotonic() + 20
                while time.monotonic() < deadline:
                    run = request(f"/v1/runs/{run_id}")
                    if run["state"] == state:
                        return run
                    assert run["state"] not in ("failed", "cancelled", "completed"), run
                    time.sleep(0.02)
                raise AssertionError((state, run))

            baseline = request("/v1/baselines")["items"][0]
            for protocol in ("ouch42", "fix44", "fixp_sbe"):
                config = json.loads(json.dumps(baseline["config"]))
                config["orderEntryProtocol"] = protocol
                config["bounds"]["endSimulationTimeNs"] = "500000000"
                config["seedSet"]["masterSeeds"] = ["42"]
                experiment = request("/v1/experiments", {
                    "name": f"Full {protocol} session", "description": "",
                    "derivedFromBaselineId": baseline["baselineId"], "config": config})
                run = request("/v1/runs", {"experimentId": experiment["experimentId"], "masterSeed": "42", "startPaused": True})
                run_id = run["runId"]
                await_state(run_id, "paused")
                request(f"/v1/runs/{run_id}/step", {"canonicalEventCount": 8})
                request(f"/v1/runs/{run_id}/resume", {})
                await_state(run_id, "completed")
                request(f"/v1/runs/{run_id}/summary")
                saved = request(f"/v1/experiments/{experiment['experimentId']}")
                assert saved["config"]["orderEntryProtocol"] == protocol, saved
                manifests = list(Path(root).rglob("manifest.json"))
                native = [json.loads(path.read_text()) for path in manifests]
                assert any(manifest.get("exchange_session", {}).get("order_entry_protocol") == protocol
                           and manifest["exchange_session"]["market_data_protocol"] == "itch50_moldudp64"
                           for manifest in native), (protocol, manifests)
                print(f"PASS: {protocol}: HTTP selection, native session provenance, pause/step/resume and completed artifacts")
        finally:
            process.terminate()
            try:
                _, errors = process.communicate(timeout=20)
            except subprocess.TimeoutExpired:
                process.kill()
                _, errors = process.communicate()
            if errors:
                print(errors)


if __name__ == "__main__":
    main()
