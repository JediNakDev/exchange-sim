"""Verify real daemon boundaries and trader failure through the HTTP API.

Uses only a disposable file-backed dataset, never installed services or data.
"""
import argparse
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import tempfile
import time
import urllib.request


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, default=Path("build/release/exsim_apid"))
    options = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="exsim-process-architecture-") as root:
        environment = {k: v for k, v in os.environ.items() if k not in ("EXSIM_DATABASE_URL", "PGPASSWORD", "PGPASSFILE")}
        process = subprocess.Popen([str(options.binary.resolve()), "--port", "0", "--data-dir", root],
                                   env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            line = process.stdout.readline()
            match = re.search(r"http://127.0.0.1:\d+", line)
            assert match, (line, process.poll())
            base = match[0]
            sequence = 0

            def request(path, body=None):
                nonlocal sequence
                sequence += 1
                headers = {"Content-Type": "application/json", "Idempotency-Key": f"process-architecture-{sequence}"}
                data = json.dumps(body).encode() if body is not None else None
                with urllib.request.urlopen(urllib.request.Request(base + path, data=data, headers=headers), timeout=20) as response:
                    return json.load(response)

            def await_state(run_id, target):
                deadline = time.monotonic() + 20
                while time.monotonic() < deadline:
                    run = request("/v1/runs/" + run_id)
                    if run["state"] == target:
                        return run
                    assert run["state"] not in ("failed", "cancelled", "completed"), (target, run)
                    time.sleep(0.02)
                raise AssertionError((target, run))

            def processes():
                return [(int(pid), int(parent), command) for pid, parent, command in
                        (row.split(None, 2) for row in subprocess.check_output(["ps", "-axo", "pid=,ppid=,command="], text=True).splitlines())]

            baseline = request("/v1/baselines")["items"][0]
            experiment = request("/v1/experiments", {"name": "Process boundary test", "description": "",
                "derivedFromBaselineId": baseline["baselineId"], "config": baseline["config"]})

            def create_run(seed):
                run = request("/v1/runs", {"experimentId": experiment["experimentId"], "masterSeed": str(seed), "startPaused": True})
                await_state(run["runId"], "paused")
                entries = processes()
                exchanges = [pid for pid, parent, command in entries if parent == process.pid and "exsim_exchanged --run-worker-fd 3" in command]
                assert len(exchanges) == 1, entries
                traders = [pid for pid, parent, command in entries if parent == exchanges[0] and "exsim_traderd --strategy-fd 3" in command]
                assert len(traders) == 1, entries
                assert len({process.pid, exchanges[0], traders[0]}) == 3
                return run["runId"], exchanges[0], traders[0]

            run_id, exchange, trader = create_run(1)
            # A paused trader must survive longer than the active-message deadline.
            time.sleep(11)
            assert any(pid == trader for pid, _, _ in processes()), "trader died while paused"
            request(f"/v1/runs/{run_id}/step", {"canonicalEventCount": 10})
            request(f"/v1/runs/{run_id}/resume", {})
            await_state(run_id, "completed")
            request(f"/v1/runs/{run_id}/summary")
            request(f"/v1/runs/{run_id}/book?eventPosition=0&view=exchange_truth&depth=5")
            print("PASS: three distinct daemon PIDs; pause past transport deadline; step/resume/completion; cold inspection")
            run_id, exchange, trader = create_run(2)
            os.kill(trader, signal.SIGKILL)
            request(f"/v1/runs/{run_id}/resume", {})
            await_state(run_id, "failed")
            assert request("/v1/service"), "API unavailable after trader crash"
            print("PASS: trader crash fails its run and leaves API available")
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
