"""Real Compose deployment check using a uniquely owned disposable dataset.

Build the default images and run docker/create-secrets.sh first. This check
removes only its random project's containers, network and volumes on exit.
"""
import html
import json
import os
from pathlib import Path
import re
import subprocess
import time
import urllib.error
import urllib.request
import uuid


ROOT = Path(__file__).resolve().parents[2]


def main():
    project = "exsim-compose-it-" + uuid.uuid4().hex[:12]
    environment = dict(os.environ, EXSIM_WEB_PORT="0", EXSIM_BIND_ADDRESS="127.0.0.1",
                       EXSIM_API_IMAGE=os.environ.get("EXSIM_API_IMAGE", "exsim-api:local"),
                       EXSIM_WEB_IMAGE=os.environ.get("EXSIM_WEB_IMAGE", "exsim-web:local"))
    command = ["docker", "compose", "--project-directory", str(ROOT), "-f", str(ROOT / "compose.yaml"), "-p", project]

    def compose(*arguments, timeout=180):
        return subprocess.run([*command, *arguments], env=environment, cwd=ROOT,
                              capture_output=True, text=True, check=True, timeout=timeout).stdout.strip()

    base_url = ""

    def request(path, body=None, key=None):
        headers = {"Content-Type": "application/json"}
        if key:
            headers["Idempotency-Key"] = key
        request = urllib.request.Request(base_url + path, headers=headers,
                                         data=json.dumps(body).encode() if body is not None else None)
        with urllib.request.urlopen(request, timeout=30) as response:
            raw = response.read()
            return response.status, raw

    def api(path, body=None, key=None, status=200):
        actual, raw = request("/api/exsim" + path, body, key)
        assert actual == status, (actual, status)
        return json.loads(raw), raw

    def await_state(run_id, state):
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            run, _ = api("/v1/runs/" + run_id)
            if run["state"] == state:
                return run
            time.sleep(0.05)
        raise AssertionError(f"run never reached {state}")

    def start():
        compose("up", "-d", "--no-build", "--wait", "--wait-timeout", "150")
        return "http://" + compose("port", "web", "3000")

    try:
        base_url = start()
        status, page = request("/experiments")
        assert status == 200 and b"<html" in page
        script = re.search(rb'src="(/_next/static/[^\"]+\.js[^\"]*)"', page)
        assert script, "production HTML has no static script"
        assert request(html.unescape(script.group(1).decode()))[0] == 200
        assert compose("exec", "-T", "api", "id", "-u") == "10001"
        assert compose("exec", "-T", "web", "id", "-u") != "0"
        assert compose("exec", "-T", "db", "psql", "-U", "postgres", "-Atc",
                       "SELECT rolsuper FROM pg_roles WHERE rolname='exsim'") == "f"
        for service in ("api", "db"):
            identifier = compose("ps", "-q", service)
            ports = subprocess.run(["docker", "inspect", "--format", "{{json .NetworkSettings.Ports}}", identifier],
                                   text=True, capture_output=True, check=True, timeout=10).stdout
            assert all(value is None for value in (json.loads(ports) or {}).values()), service
        baselines, _ = api("/v1/baselines")
        baseline = baselines["items"][0]
        body = {"name": "Compose persistence check", "description": "disposable integration data",
                "derivedFromBaselineId": baseline["baselineId"], "config": baseline["config"]}
        experiment, created = api("/v1/experiments", body, "compose-experiment", 201)
        run, _ = api("/v1/runs", {"experimentId": experiment["experimentId"],
                     "masterSeed": baseline["config"]["seedSet"]["masterSeeds"][0], "startPaused": True}, "compose-run-key", 201)
        run_id = run["runId"]
        await_state(run_id, "paused")
        container = compose("ps", "-q", "api")
        processes = subprocess.run(["docker", "top", container, "-eo", "pid,ppid,args"],
                                   capture_output=True, text=True, check=True, timeout=10).stdout
        assert "exsim_apid" in processes, "no API daemon"
        assert "exsim_exchanged --run-worker-fd 3" in processes, "no exchange daemon"
        assert "exsim_traderd --strategy-fd 3" in processes, "no trader daemon"
        api(f"/v1/runs/{run_id}/step", {"canonicalEventCount": 10}, "compose-step-key")
        api(f"/v1/runs/{run_id}/resume", {}, "compose-resume-key")
        await_state(run_id, "completed")
        summary = api(f"/v1/runs/{run_id}/summary")[1]
        export, _ = api(f"/v1/runs/{run_id}/audit-export")
        assert any(item["name"] == "run.json" and item["present"] for item in export["items"])
        compose("exec", "-T", "api", "test", "-f", f"/var/lib/exsim/runs/{run_id}/events.bin")
        assert compose("exec", "-T", "db", "psql", "-U", "postgres", "-d", "exsim", "-Atc",
                       "SELECT state FROM exsim.runs") == "completed"
        # Recreate every container, including PostgreSQL, retaining both volumes.
        compose("down")
        base_url = start()
        assert api("/v1/experiments", body, "compose-experiment", 201)[1] == created
        assert api(f"/v1/runs/{run_id}/summary")[1] == summary
        assert api(f"/v1/runs/{run_id}")[0]["state"] == "completed"
        print("Compose deployment passed: production HTML/assets, frontend proxy, non-root services, private API/DB, "
              "real worker/controls/artifacts, PostgreSQL metadata and exact replay/result persistence after full recreation.")
    except Exception:
        # Credentials are mounted as files and never included in commands/config.
        print(compose("logs", "--no-color", "--tail", "60"))
        raise
    finally:
        compose("down", "--volumes", "--remove-orphans")


if __name__ == "__main__":
    main()
