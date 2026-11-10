"""Management API, child processes, TCP sessions and UDP publication integration.

No browser, database, synthetic traders, or external Python dependencies.
"""
import concurrent.futures
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import select
import signal
import socket
import struct
import subprocess
import tempfile
import time
import unittest
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / os.environ.get("EXSIM_BUILD", "build/release")
CONFIG = (ROOT / "test/fixtures/exchange.conf").read_text()


def unused_udp_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def config(feed_port=None):
    return (CONFIG.replace("ouch_port = 15000", "ouch_port = 0")
            .replace("retransmit_port = 16001", "retransmit_port = 0")
            .replace("address = 239.192.10.1", "address = 127.0.0.1")
            .replace("port = 16000", f"port = {feed_port or unused_udp_port()}"))


class Trader:
    def __init__(self, port, username, password):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=3)
        self.send(b"L" + username.encode().ljust(6) + password.encode().ljust(10)
                  + b" " * 10 + b"1".rjust(20))
        assert self.receive()[:1] == b"A", "login was not accepted"

    def send(self, message):
        self.sock.sendall(struct.pack("!H", len(message)) + message)

    def exact(self, size):
        result = b""
        while len(result) < size:
            part = self.sock.recv(size - len(result))
            if not part:
                raise AssertionError("truncated SoupBinTCP frame")
            result += part
        return result

    def receive(self):
        return self.exact(struct.unpack("!H", self.exact(2))[0])

    def event(self, kind):
        for _ in range(20):
            frame = self.receive()
            if frame[:2] == b"S" + kind:
                return frame[1:]
            if frame[:2] == b"SJ":
                raise AssertionError(f"order rejected: {frame!r}")
        raise AssertionError(f"missing event {kind!r}")

    def order(self, token, side, quantity=100, price=1500000):
        message = (b"O" + token.encode().ljust(14) + side + struct.pack("!I", quantity)
                   + b"AAPL    " + struct.pack("!II", price, 99999)
                   + b"TEST" + b"Y" + b"A" + b"N" + struct.pack("!I", 0) + b"N" + b" ")
        assert len(message) == 49
        self.send(b"U" + message)
        return self.event(b"A")

    def close(self):
        self.sock.close()


class ExchangeApiTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="exsim-exchange-it-")
        self.addCleanup(self.temporary.cleanup)
        self.data = Path(self.temporary.name) / "exchanges"
        self.children = []
        self.start_api()
        self.addCleanup(self.stop_api)

    def start_api(self):
        self.stderr = tempfile.TemporaryFile(mode="w+")
        self.process = subprocess.Popen([str(BUILD / "exsim_api"), "--port", "0", "--data-dir", str(self.data)],
                                        stdout=subprocess.PIPE, stderr=self.stderr, text=True)
        if not select.select([self.process.stdout], [], [], 10)[0]:
            self.process.kill()
            self.process.wait()
            self.fail("API startup timed out")
        line = self.process.stdout.readline().strip()
        if not line:
            self.stderr.seek(0)
            self.fail(self.stderr.read())
        self.url = line.split("listening on ", 1)[1]

    def stop_api(self):
        if self.process.poll() is None:
            self.process.terminate()
            self.process.wait(timeout=10)
        self.process.stdout.close()
        self.stderr.close()

    def api(self, method, path, body=None, expected=200):
        request = urllib.request.Request(self.url + path, method=method,
                                         headers={"Content-Type": "application/json"},
                                         data=json.dumps(body).encode() if body is not None else None)
        try:
            response = urllib.request.urlopen(request, timeout=15)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            raw = response.read()
            self.assertEqual(response.status, expected, raw)
        return json.loads(raw) if raw else None

    def create(self, text=None):
        return self.api("POST", "/v1/exchanges", {"config": text or config()}, 201)

    def start(self, exchange):
        started = self.api("POST", f"/v1/exchanges/{exchange['id']}/start")
        self.children.append(started["pid"])
        return started

    def test_crud_validation_and_redaction(self):
        self.assertEqual(self.api("GET", "/v1/exchanges"), [])
        self.api("POST", "/v1/exchanges", {"config": "[instrument X]"}, 400)
        exchange = self.create()
        self.assertNotIn("password = alice", exchange["config"])
        self.assertIn("[REDACTED]", exchange["config"])
        path = f"/v1/exchanges/{exchange['id']}"
        self.assertEqual(self.api("GET", path)["state"], "stopped")
        self.api("PUT", path, {"config": exchange["config"]}, 400)
        updated = self.api("PUT", path, {"config": config().replace("name = EXSIM", "name = OTHER")})
        self.assertEqual(updated["name"], "OTHER")
        self.api("DELETE", path, expected=204)
        self.api("GET", path, expected=404)

    def test_rejects_bots_and_unavailable_protocols(self):
        for extra in ("\n[bot maker]\ntype=market_maker\n", "\n[exchange]\nfixp_sbe_port=15001\n"):
            self.api("POST", "/v1/exchanges", {"config": config() + extra}, 400)

    def test_two_instances_have_independent_ports_and_empty_books(self):
        first, second = self.start(self.create()), self.start(self.create())
        self.assertNotEqual(first["pid"], second["pid"])
        self.assertNotEqual(first["runtime"]["connections"]["orderEntryPort"], second["runtime"]["connections"]["orderEntryPort"])
        self.assertNotEqual(first["runtime"]["connections"]["recoveryPort"], second["runtime"]["connections"]["recoveryPort"])
        trader = Trader(first["runtime"]["connections"]["orderEntryPort"], "ALICE", "alice")
        self.addCleanup(trader.close)
        trader.order("ONE", b"B")
        a = self.api("GET", f"/v1/exchanges/{first['id']}/status")
        b = self.api("GET", f"/v1/exchanges/{second['id']}/status")
        self.assertEqual(a["runtime"]["liveOrders"], "1")
        self.assertEqual(b["runtime"]["liveOrders"], "0")
        self.assertEqual(self.api("POST", f"/v1/exchanges/{first['id']}/start")["pid"], first["pid"])
        self.api("PUT", f"/v1/exchanges/{first['id']}", {"config": config()}, 409)
        self.api("DELETE", f"/v1/exchanges/{first['id']}", expected=409)

    def test_external_sessions_match_and_publish_recoverable_market_data(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as feed:
            feed.bind(("127.0.0.1", 0))
            feed.settimeout(3)
            exchange = self.start(self.create(config(feed.getsockname()[1])))
            details = self.api("GET", f"/v1/exchanges/{exchange['id']}/connections")
            alice, bob = Trader(details["orderEntryPort"], "ALICE", "alice"), Trader(details["orderEntryPort"], "BOB", "bob")
            self.addCleanup(alice.close)
            self.addCleanup(bob.close)
            alice.order("BUY", b"B")
            bob.order("SELL", b"S")
            self.assertEqual(alice.event(b"E")[:1], b"E")
            self.assertEqual(bob.event(b"E")[:1], b"E")
            status = self.api("GET", f"/v1/exchanges/{exchange['id']}/status")
            self.assertEqual(status["runtime"]["liveOrders"], "0")
            packet, _ = feed.recvfrom(65535)
            session, sequence, count = struct.unpack("!10sQH", packet[:20])
            self.assertGreater(count, 0)
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as recovery:
                recovery.settimeout(3)
                recovery.sendto(struct.pack("!10sQH", session, sequence, count), ("127.0.0.1", details["recoveryPort"]))
                replay, _ = recovery.recvfrom(65535)
            self.assertEqual(replay, packet)

    def test_restart_is_empty_with_a_fresh_session(self):
        exchange = self.start(self.create())
        path = f"/v1/exchanges/{exchange['id']}"
        trader = Trader(exchange["runtime"]["connections"]["orderEntryPort"], "ALICE", "alice")
        trader.order("BEFORE", b"B")
        self.api("POST", path + "/stop")
        trader.close()
        restarted = self.start(exchange)
        self.assertEqual(restarted["runtime"]["liveOrders"], "0")
        self.assertNotEqual(restarted["runtime"]["connections"]["session"], exchange["runtime"]["connections"]["session"])

    def test_port_conflict_rolls_back_and_can_retry(self):
        with socket.socket() as busy:
            busy.bind(("127.0.0.1", 0))
            busy.listen()
            exchange = self.create(config().replace("ouch_port = 0", f"ouch_port = {busy.getsockname()[1]}"))
            path = f"/v1/exchanges/{exchange['id']}"
            self.api("POST", path + "/start", expected=409)
            failed = self.api("GET", path)
            self.assertEqual(failed["state"], "failed")
            self.assertIsNone(failed["pid"])
        self.assertEqual(self.start(exchange)["state"], "running")

    def test_feed_conflict_is_reported(self):
        text = config()
        first, second = self.create(text), self.create(text)
        self.start(first)
        self.api("POST", f"/v1/exchanges/{second['id']}/start", expected=409)

    def test_concurrent_start_creates_one_child(self):
        exchange = self.create()
        path = f"/v1/exchanges/{exchange['id']}/start"
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            started = list(pool.map(lambda _: self.api("POST", path), range(4)))
        self.assertEqual(len({record["pid"] for record in started}), 1)

    def test_configs_survive_manager_restart_without_restarting_exchanges(self):
        exchange = self.start(self.create())
        pid = exchange["pid"]
        self.stop_api()
        with self.assertRaises(ProcessLookupError):
            os.kill(pid, 0)
        self.start_api()
        saved = self.api("GET", f"/v1/exchanges/{exchange['id']}")
        self.assertEqual(saved["state"], "stopped")
        self.assertEqual(saved["name"], exchange["name"])

    def test_second_manager_cannot_use_same_directory(self):
        other = subprocess.run([str(BUILD / "exsim_api"), "--port", "0", "--data-dir", str(self.data)], capture_output=True, timeout=5)
        self.assertNotEqual(other.returncode, 0)
        self.assertIn(b"locked", other.stderr)

    def test_child_crash_is_reported_and_restart_works(self):
        exchange = self.start(self.create())
        os.kill(exchange["pid"], signal.SIGKILL)
        path = f"/v1/exchanges/{exchange['id']}"
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            record = self.api("GET", path)
            if record["state"] == "failed":
                break
            time.sleep(0.01)
        self.assertEqual(record["state"], "failed")
        self.assertEqual(self.start(exchange)["state"], "running")

    def test_recovery_bind_failure_releases_order_entry_listener(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as busy:
            busy.bind(("127.0.0.1", 0))
            with socket.socket() as available:
                available.bind(("127.0.0.1", 0))
                order_port = available.getsockname()[1]
            text = config().replace("ouch_port = 0", f"ouch_port = {order_port}")
            text = text.replace("retransmit_port = 0", f"retransmit_port = {busy.getsockname()[1]}")
            exchange = self.create(text)
            self.api("POST", f"/v1/exchanges/{exchange['id']}/start", expected=409)
            with socket.socket() as probe:
                probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                probe.bind(("127.0.0.1", order_port))
                probe.listen()

    def test_invalid_update_preserves_saved_config(self):
        exchange = self.create()
        path = f"/v1/exchanges/{exchange['id']}"
        self.api("PUT", path, {"config": "broken"}, 400)
        self.assertEqual(self.api("GET", path)["config"], exchange["config"])
        self.assertEqual(self.start(exchange)["state"], "running")

    def test_stale_temporary_config_does_not_block_replacement(self):
        exchange = self.create()
        (self.data / (exchange["id"] + ".tmp")).write_text("interrupted write")
        updated = self.api("PUT", f"/v1/exchanges/{exchange['id']}", {"config": config()})
        self.assertEqual(updated["state"], "stopped")
        self.assertFalse((self.data / (exchange["id"] + ".tmp")).exists())

    def test_manager_crash_closes_child_control_and_releases_trading_ports(self):
        exchange = self.start(self.create())
        port = exchange["runtime"]["connections"]["orderEntryPort"]
        self.process.kill()
        self.process.wait(timeout=5)
        deadline = time.monotonic() + 5
        released = False
        while time.monotonic() < deadline:
            with socket.socket() as probe:
                probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                try:
                    probe.bind(("127.0.0.1", port))
                    probe.listen()
                    released = True
                    break
                except OSError:
                    time.sleep(0.02)
        self.assertTrue(released, "child kept trading ports open after manager death")

    def test_api_managed_fix_sessions_trade_through_discovered_port(self):
        self.assertIn("fix44", self.api("GET", "/v1/service")["orderEntryProtocols"])
        exchange = self.start(self.create(config().replace("protocol = ouch42", "protocol = fix44")))
        path = f"/v1/exchanges/{exchange['id']}"
        details = self.api("GET", path + "/connections")
        self.assertGreater(details["fixOrderEntryPort"], 0)
        self.assertNotEqual(details["fixOrderEntryPort"], details["orderEntryPort"])

        def receive(sock):
            # Read one FIX frame by BodyLength, preserving any following frame.
            header = b""
            while header.count(b"\x01") < 2:
                byte = sock.recv(1)
                self.assertTrue(byte, "FIX connection closed before header")
                header += byte
            body_length = int(header.split(b"\x01")[1].split(b"=")[1])
            remaining = body_length + 7  # Body plus 10=ddd<SOH>.
            body = b""
            while len(body) < remaining:
                chunk = sock.recv(remaining - len(body))
                self.assertTrue(chunk, "FIX connection closed before checksum")
                body += chunk
            return header + body

        def send(sock, user, kind, sequence, fields):
            timestamp = datetime.now(timezone.utc).strftime("%Y%m%d-%H:%M:%S.%f")[:-3]
            body = f"35={kind}|49={user}|56=EXSIM|34={sequence}|52={timestamp}|{fields}".replace("|", "\x01").encode()
            frame = f"8=FIX.4.4\x019={len(body)}\x01".encode() + body
            sock.sendall(frame + f"10={sum(frame) % 256:03d}\x01".encode())
            return timestamp

        alice = socket.create_connection(("127.0.0.1", details["fixOrderEntryPort"]), timeout=3)
        bob = socket.create_connection(("127.0.0.1", details["fixOrderEntryPort"]), timeout=3)
        self.addCleanup(alice.close)
        self.addCleanup(bob.close)
        send(alice, "ALICE", "A", 1, "98=0|108=30|554=alice|")
        self.assertIn(b"35=A\x01", receive(alice))
        send(bob, "BOB", "A", 1, "98=0|108=30|554=bob|")
        self.assertIn(b"35=A\x01", receive(bob))
        timestamp = datetime.now(timezone.utc).strftime("%Y%m%d-%H:%M:%S.%f")[:-3]
        send(alice, "ALICE", "D", 2, f"11=BUY|55=AAPL|54=1|38=100|40=2|44=150|59=0|60={timestamp}|")
        self.assertIn(b"39=0\x01", receive(alice))
        self.assertEqual(self.api("GET", path + "/status")["runtime"]["liveOrders"], "1")
        send(bob, "BOB", "D", 2, f"11=SELL|55=AAPL|54=2|38=100|40=2|44=150|59=0|60={timestamp}|")
        for sock in (bob, alice):
            for _ in range(3):
                if b"39=2\x01" in receive(sock):
                    break
            else:
                self.fail("FIX execution report did not reach the participant")
        self.assertEqual(self.api("GET", path + "/status")["runtime"]["liveOrders"], "0")


if __name__ == "__main__":
    unittest.main(verbosity=2)
