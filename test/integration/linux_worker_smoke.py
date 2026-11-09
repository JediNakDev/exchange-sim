"""Exercise a built Linux API inside a disposable, already-running Docker container.

No Python/curl packages are needed in the container. The host uses Python's JSON
parser; fixed Bash /dev/tcp commands transfer HTTP over container loopback only.
The caller owns the container and removes it after validation.
"""
import argparse
import json
import subprocess
import time
import uuid


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--container", required=True)
    parser.add_argument("--binary", default="/tmp/exsim-worker/exsim_api")
    options = parser.parse_args()
    root = "/tmp/exsim-worker-smoke-" + uuid.uuid4().hex

    def execute(arguments, **kwargs):
        return subprocess.run(["docker", "exec", *arguments], check=True, capture_output=True, timeout=30, **kwargs)

    def request(path, body=None, key="linux-smoke-key", expected=200):
        payload = b"" if body is None else json.dumps(body).encode()
        method = "GET" if body is None else "POST"
        raw = (f"{method} {path} HTTP/1.1\r\nHost: worker-smoke\r\nConnection: close\r\n"
               f"Content-Type: application/json\r\nIdempotency-Key: {key}\r\nContent-Length: {len(payload)}\r\n\r\n").encode() + payload
        response = execute(["-i", options.container, "/bin/bash", "-c",
                            "exec 7<>/dev/tcp/127.0.0.1/47600; cat >&7; cat <&7"], input=raw).stdout
        header, content = response.split(b"\r\n\r\n", 1)
        status = int(header.split(b" ", 2)[1])
        if status != expected:
            raise AssertionError(f"HTTP {status}, expected {expected}: {content[:1000]!r}")
        return json.loads(content)

    def await_state(run_id, state):
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            result = request("/v1/runs/" + run_id)
            if result["state"] == state:
                return result
            time.sleep(0.05)
        raise AssertionError(f"worker did not reach {state}")

    execute(["-d", options.container, "/bin/bash", "-c",
             'mkdir -p "$2"; echo "$$" > "$2/pid"; exec "$1" --port 47600 --data-dir "$2/data" > "$2/api.log" 2>&1',
             "worker-smoke", options.binary, root])
    try:
        deadline = time.monotonic() + 15
        while True:
            try:
                request("/v1/service")
                break
            except (subprocess.SubprocessError, ValueError):
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.05)
        baseline = request("/v1/baselines")["items"][0]
        experiments = []
        for index in range(2):
            experiment = request("/v1/experiments", {"name": "Linux worker", "description": "",
                "derivedFromBaselineId": baseline["baselineId"], "config": baseline["config"]}, f"linux-experiment-{index}", 201)
            experiments.append(experiment["experimentId"])
            run = request("/v1/runs", {"experimentId": experiment["experimentId"], "masterSeed": "1", "startPaused": True},
                          f"linux-run-key-{index}", 201)
            run_id = run["runId"]
            await_state(run_id, "paused")
            request(f"/v1/runs/{run_id}/step", {"canonicalEventCount": 10}, f"linux-step-{index}")
            request(f"/v1/runs/{run_id}/resume", {}, f"linux-resume-{index}")
            await_state(run_id, "completed")
            request(f"/v1/runs/{run_id}/book?eventPosition=0&view=exchange_truth&depth=5")
            request(f"/v1/runs/{run_id}/audit-export")
        comparison = request(f"/v1/comparisons?baselineExperimentId={experiments[0]}&candidateExperimentId={experiments[1]}")
        assert comparison["comparable"]
        print("Linux worker smoke passed: two isolated runs, pause/step/resume, cold book replay, audit and paired comparison")
    finally:
        execute([options.container, "/bin/bash", "-c", 'read -r pid < "$1/pid"; kill -TERM "$pid"', "worker-smoke", root])


if __name__ == "__main__":
    main()
