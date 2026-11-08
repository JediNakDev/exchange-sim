#!/usr/bin/env python3
"""Run a verified copied release with NEW participant data; Ctrl+C stops owned services.
This launches an isolated session, not a claim that human usability has passed.
"""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import time
from types import SimpleNamespace
from urllib.request import ProxyHandler, build_opener

from deployment.research_delivery import ready, verify


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--release', type=Path, required=True)
    parser.add_argument('--session', type=Path, required=True, help='new directory, never reused')
    parser.add_argument('--node', default='node')
    parser.add_argument('--api-port', type=int, default=46073)
    parser.add_argument('--frontend-port', type=int, default=46072)
    args = parser.parse_args()
    if not (1 <= args.api_port <= 65535 and 1 <= args.frontend_port <= 65535) or args.api_port == args.frontend_port:
        parser.error('use distinct ports between 1 and 65535')
    args.release = args.release.resolve(strict=True)
    metadata = verify(args.release, 'release')
    node = shutil.which(args.node)
    if not node:
        parser.error('Node executable not found')
    if subprocess.check_output([node, '--version'], text=True).strip() != metadata['node_version']:
        parser.error('Node version must match the verified release metadata')
    args.session = args.session.absolute()
    if args.session.resolve().is_relative_to(args.release):
        parser.error('participant data must be outside the release')
    args.session.mkdir(exist_ok=False)
    data = args.session / 'data'
    api_url = f'http://127.0.0.1:{args.api_port}'
    frontend_url = f'http://127.0.0.1:{args.frontend_port}'
    receipt = {'state': 'starting', 'release': str(args.release), 'metadata': {key: metadata[key] for key in ['api_revision', 'web_revision', 'web_build_id', 'node_version']},
               'participant_url': frontend_url + '/experiments', 'api_url': api_url, 'data': str(data),
               'started_at': datetime.now(timezone.utc).isoformat(), 'human_result': 'NOT RUN'}
    receipt_path = args.session / 'session.json'
    receipt_path.write_text(json.dumps(receipt, indent=2) + '\n')
    children = []
    stopped = False

    def stop(_signal, _frame):
        nonlocal stopped
        stopped = True

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    try:
        with (args.session / 'api.log').open('w') as api_log, (args.session / 'frontend.log').open('w') as frontend_log:
            children.append(subprocess.Popen([str(args.release / 'api/exsim_api'), '--bind', '127.0.0.1', '--port', str(args.api_port), '--data-dir', str(data),
                                              '--source-revision', metadata['api_revision'], '--release-id', 'human-' + metadata['web_build_id']],
                                             cwd=args.session, stdout=api_log, stderr=subprocess.STDOUT))
            environment = {**os.environ, 'HOSTNAME': '127.0.0.1', 'PORT': str(args.frontend_port), 'EXSIM_API_BASE_URL': api_url, 'NODE_PATH': ''}
            children.append(subprocess.Popen([node, str(args.release / 'frontend/server.js')], cwd=args.release / 'frontend', env=environment,
                                             stdout=frontend_log, stderr=subprocess.STDOUT))
            opener = build_opener(ProxyHandler({}))
            deadline = time.monotonic() + 20
            while not stopped:
                if any(child.poll() is not None for child in children):
                    raise RuntimeError('owned service exited; inspect this session\'s api.log and frontend.log')
                try:
                    with opener.open(frontend_url + '/experiments', timeout=1) as response:
                        if response.status == 200:
                            break
                except OSError:
                    pass
                if time.monotonic() >= deadline:
                    raise RuntimeError('session readiness timed out; inspect retained logs')
                time.sleep(0.1)
            if not stopped:
                ready(SimpleNamespace(release=args.release, api_url=api_url, frontend_url=frontend_url))
                receipt.update(state='ready', pids=[child.pid for child in children])
                receipt_path.write_text(json.dumps(receipt, indent=2) + '\n')
                print('Session URL:', receipt['participant_url'], flush=True)
                print('Session logs and data:', args.session, flush=True)
                print('Ctrl+C stops only these services and preserves participant data.', flush=True)
            while not stopped:
                if any(child.poll() is not None for child in children):
                    raise RuntimeError('owned service exited; inspect retained session logs')
                time.sleep(0.2)
    finally:
        for child in children:
            if child.poll() is None:
                child.terminate()
        for child in children:
            try:
                child.wait(timeout=3)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
        receipt.update(state='stopped', stopped_at=datetime.now(timezone.utc).isoformat())
        receipt_path.write_text(json.dumps(receipt, indent=2) + '\n')


if __name__ == '__main__':
    main()
