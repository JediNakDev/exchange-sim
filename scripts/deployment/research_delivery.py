#!/usr/bin/env python3
"""Package prebuilt research services and copy explicitly offline data. Stdlib only.

Never starts, stops, builds or selects services. See docs/DEPLOYMENT.md.
"""

import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import sys
import tempfile
from urllib.parse import quote, urlsplit
from urllib.request import HTTPRedirectHandler, ProxyHandler, Request, build_opener


MANIFEST = "delivery.json"
FORMAT = "exsim.delivery/1"


def require(condition, message):
    if not condition:
        raise ValueError(message)


def file_digest(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def inventory(root, allow_links=False):
    """Record empty directories, bytes, executable bits and internal link targets."""
    require(root.is_dir() and not root.is_symlink(), "expected a real directory")
    entries = {}

    def scan_failed(error):
        raise error

    for directory, directories, files in os.walk(root, followlinks=False, onerror=scan_failed):
        for name in sorted(directories + files):
            path = Path(directory) / name
            relative = path.relative_to(root).as_posix()
            mode = path.lstat().st_mode
            if stat.S_ISLNK(mode):
                require(allow_links, "data trees must not contain symlinks")
                target = os.readlink(path)
                require(not os.path.isabs(target), "absolute release symlink")
                resolved = path.resolve(strict=True)
                require(resolved.is_relative_to(root.resolve()), "release symlink escapes its tree")
                entries[relative] = {"link": target}
            elif stat.S_ISDIR(mode):
                entries[relative] = {"directory": True}
            else:
                require(stat.S_ISREG(mode), "special files are not supported")
                entries[relative] = {"sha256": file_digest(path), "size": path.stat().st_size,
                                     "executable": bool(mode & 0o111)}
    return dict(sorted(entries.items()))


def require_separate(source, destination):
    source, destination = source.resolve(), destination.resolve()
    require(not source.is_relative_to(destination) and not destination.is_relative_to(source),
            "source and destination must not overlap")


@contextmanager
def new_directory(destination):
    """Publish only a checked directory. Reserve the name against another tool run."""
    destination = destination.absolute()
    require(not destination.exists() and not destination.is_symlink(), "destination already exists")
    require(destination.parent.is_dir(), "destination parent must already exist")
    lock = destination.with_name(destination.name + ".delivery-lock")
    lock.mkdir(mode=0o700)
    staging = None
    try:
        require(not destination.exists() and not destination.is_symlink(), "destination already exists")
        staging = Path(tempfile.mkdtemp(prefix=".delivery-", dir=destination.parent))
        yield staging
        require(not destination.exists() and not destination.is_symlink(), "destination appeared during copy")
        staging.rename(destination)
    finally:
        if staging is not None and staging.exists():
            shutil.rmtree(staging)
        lock.rmdir()


def write_manifest(root, kind, metadata):
    metadata = {"format": FORMAT, "kind": kind, **metadata,
                "entries": inventory(root, allow_links=kind == "release")}
    (root / MANIFEST).write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n")


def verify(root, kind=None):
    require(not root.is_symlink(), "bundle root must not be a symlink")
    manifest_path = root / MANIFEST
    require(manifest_path.is_file() and not manifest_path.is_symlink(), "missing regular delivery manifest")
    metadata = json.loads(manifest_path.read_text())
    require(isinstance(metadata, dict), "delivery manifest must be an object")
    require(metadata.get("format") == FORMAT and metadata.get("kind") in {"release", "backup"},
            "unsupported delivery manifest")
    require(kind is None or metadata["kind"] == kind, "wrong bundle kind")
    actual = inventory(root, allow_links=metadata["kind"] == "release")
    actual.pop(MANIFEST)
    require(actual == metadata.get("entries"), "bundle inventory mismatch")
    if metadata["kind"] == "release":
        require((root / "api/exsim_api").is_file() and os.access(root / "api/exsim_api", os.X_OK),
                "API binary is missing or not executable")
        require((root / "frontend/server.js").is_file(), "missing standalone server.js")
        require((root / "frontend/.next/BUILD_ID").read_text().strip() == metadata["web_build_id"],
                "frontend build ID mismatch")
    else:
        require(metadata.get("quiescence") == "operator-declared-writers-stopped",
                "backup has no offline attestation")
        require((root / "data/experiments").is_dir() and (root / "data/runs").is_dir(),
                "backup is missing API data directories")
    return metadata


def package(args):
    sources = [args.api, args.standalone, args.static]
    if args.public:
        sources.append(args.public)
    for source in sources:
        require_separate(source, args.output)
    require(args.api.is_file() and not args.api.is_symlink() and os.access(args.api, os.X_OK),
            "API must be an executable regular file")
    before = {}
    for source in sources[1:]:
        before[str(source)] = inventory(source, allow_links=True)
        require(not any(part in {".git", ".npmrc", ".yarnrc.yml"} or part.startswith(".env") for name in before[str(source)]
                        for part in Path(name).parts), "release inputs contain environment or credential files")
    api_digest = file_digest(args.api)
    require((args.standalone / "server.js").is_file(), "missing standalone server.js")
    build_id = (args.standalone / ".next/BUILD_ID").read_text().strip()
    require(bool(build_id) and bool(before[str(args.static)]), "missing build ID or static assets")
    with new_directory(args.output) as staging:
        (staging / "api").mkdir()
        shutil.copy2(args.api, staging / "api/exsim_api")
        shutil.copytree(args.standalone, staging / "frontend", symlinks=True)
        require(inventory(staging / "frontend", allow_links=True) == before[str(args.standalone)],
                "standalone copy differs from input")
        # Replace stale copied assets instead of nesting static/static on retries.
        for source, destination in [(args.static, staging / "frontend/.next/static"),
                                    (args.public, staging / "frontend/public")]:
            if destination.is_symlink():
                destination.unlink()
            elif destination.exists():
                shutil.rmtree(destination)
            if source is not None:
                shutil.copytree(source, destination, symlinks=True)
                require(inventory(destination, allow_links=True) == before[str(source)],
                        "asset copy differs from input")
        require(file_digest(args.api) == api_digest == file_digest(staging / "api/exsim_api"),
                "API changed during packaging")
        for source in sources[1:]:
            require(inventory(source, allow_links=True) == before[str(source)],
                    "frontend input changed during packaging")
        write_manifest(staging, "release", {
            "api_revision": args.api_revision, "web_revision": args.web_revision,
            "web_build_id": build_id, "target": args.target, "node_version": args.node_version,
            "schema_version": "exsim.api/1", "service_build_id": "exsim_api/1",
        })
        verify(staging, "release")


def backup(args):
    require(args.writers_stopped, "backup requires --writers-stopped; disable ALL writers and restart jobs first")
    require_separate(args.data, args.output)
    require((args.data / "experiments").is_dir() and (args.data / "runs").is_dir(),
            "expected API data root with experiments/ and runs/")
    before = inventory(args.data)
    with new_directory(args.output) as staging:
        shutil.copytree(args.data, staging / "data")
        require(inventory(args.data) == before == inventory(staging / "data"),
                "data changed during backup; no backup published")
        write_manifest(staging, "backup", {"quiescence": "operator-declared-writers-stopped"})
        verify(staging, "backup")


def restore(args):
    require_separate(args.backup, args.output)
    original = verify(args.backup, "backup")
    with new_directory(args.output) as staging:
        shutil.copytree(args.backup / "data", staging, dirs_exist_ok=True)
        data_entries = {name.removeprefix("data/"): entry for name, entry in original["entries"].items()
                        if name.startswith("data/")}
        require(inventory(staging) == data_entries, "restored data inventory mismatch")
        require(verify(args.backup, "backup") == original, "backup changed during restore")


class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, request, response, code, message, headers, new_url):
        raise ValueError("readiness endpoint redirected")


def ready(args):
    metadata = verify(args.release, "release")
    for url in [args.api_url, args.frontend_url]:
        parts = urlsplit(url)
        require(parts.scheme in {"http", "https"} and parts.hostname and not parts.username and
                not parts.password and not parts.query and not parts.fragment and parts.path in {"", "/"},
                "readiness needs an origin URL without credentials, path, query or fragment")
        require(parts.scheme == "https" or parts.hostname in {"127.0.0.1", "localhost", "::1"},
                "plain HTTP readiness is loopback-only")
    opener = build_opener(ProxyHandler({}), NoRedirect())

    def get(origin, path):
        request = Request(origin.rstrip("/") + path, headers={"Exsim-Schema-Version": metadata["schema_version"]})
        with opener.open(request, timeout=5) as response:
            require(response.status == 200, "readiness HTTP status is not 200")
            body = response.read(16 * 1024 * 1024 + 1)
            require(len(body) <= 16 * 1024 * 1024, "readiness response exceeds size limit")
            return response.headers, body

    def service(origin, path):
        headers, body = get(origin, path)
        value = json.loads(body)
        require(isinstance(value, dict), "service response must be an object")
        require(headers.get("Exsim-Schema-Version") == metadata["schema_version"] and
                value.get("schemaVersion") == metadata["schema_version"] and value.get("apiVersion") == "v1" and
                value.get("serviceBuildId") == metadata["service_build_id"], "service identity/schema mismatch")
        require({"fixed_spread_market_maker", "inventory_aware_market_maker"}.issubset(
                    value.get("executableStrategyKinds", [])) and
                "interactive_synthetic" in value.get("executableMarketWorlds", []),
                "research capabilities missing")
        return value

    require(service(args.api_url, "/v1/service") == service(args.frontend_url, "/api/exsim/v1/service"),
            "direct API and frontend proxy report different services")
    headers, body = get(args.frontend_url, "/experiments")
    require("text/html" in headers.get("Content-Type", "") and b"<html" in body.lower(),
            "frontend did not serve an HTML page")
    # Check a packaged asset's exact bytes, not just an HTTP 200/HTML fallback.
    assets = sorted(path for path in metadata["entries"]
                    if path.startswith("frontend/.next/static/") and path.endswith(".js"))
    require(bool(assets), "release has no static JavaScript")
    asset = assets[0]
    _, body = get(args.frontend_url, "/_next/static/" + quote(asset.removeprefix("frontend/.next/static/")))
    require(hashlib.sha256(body).hexdigest() == metadata["entries"][asset]["sha256"],
            "frontend static asset differs from release")


def revision(value):
    if not re.fullmatch(r"[0-9a-f]{40}", value):
        raise argparse.ArgumentTypeError("use the full 40-character source commit SHA")
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    command = commands.add_parser("package", help="copy already-built API and standalone frontend into a new release")
    for name in ["api", "standalone", "static", "output"]:
        command.add_argument("--" + name, type=Path, required=True)
    command.add_argument("--public", type=Path)
    for name in ["api-revision", "web-revision"]:
        command.add_argument("--" + name, type=revision, required=True)
    command.add_argument("--target", required=True, help="operator-recorded build OS/architecture, e.g. macos-arm64")
    command.add_argument("--node-version", required=True, help="exact Node version used to validate this standalone bundle")
    command.set_defaults(action=package)
    command = commands.add_parser("verify", help="check every bundled entry against its manifest")
    command.add_argument("bundle", type=Path)
    command.set_defaults(action=lambda args: verify(args.bundle))
    command = commands.add_parser("backup", help="copy the whole API data root, only while all writers are stopped")
    command.add_argument("--data", type=Path, required=True)
    command.add_argument("--output", type=Path, required=True)
    command.add_argument("--writers-stopped", action="store_true",
                         help="attest API is stopped, supervisor restart disabled, and all writers stay stopped until exit")
    command.set_defaults(action=backup)
    command = commands.add_parser("restore", help="restore a verified backup into a NEW data directory")
    command.add_argument("--backup", type=Path, required=True)
    command.add_argument("--output", type=Path, required=True)
    command.set_defaults(action=restore)
    command = commands.add_parser("ready", help="read-only API, proxy, HTML and one static-asset checks")
    command.add_argument("--release", type=Path, required=True)
    command.add_argument("--api-url", default="http://127.0.0.1:7600")
    command.add_argument("--frontend-url", default="http://127.0.0.1:3760")
    command.set_defaults(action=ready)
    args = parser.parse_args()
    try:
        args.action(args)
    except (OSError, ValueError, KeyError, TypeError, RuntimeError) as error:
        # No data records, idempotency keys or HTTP response bodies in diagnostics.
        print(f"{args.command}: failed ({type(error).__name__}); {error}", file=sys.stderr)
        return 1
    print(f"{args.command}: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
