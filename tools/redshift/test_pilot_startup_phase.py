"""Offline libpq startup-phase fixture; never contacts AWS or Redshift."""

from __future__ import annotations

from contextlib import contextmanager
import os
from pathlib import Path
import shutil
import socket
import ssl
import struct
import subprocess
import tempfile
import threading
import time
from typing import Optional
import unittest

from tools.redshift import pilot_live
from tools.redshift.pilot_preflight import Blocked


def _exact(sock: socket.socket, size: int) -> bytes:
    value = bytearray()
    while len(value) < size:
        part = sock.recv(size - len(value))
        if not part:
            raise EOFError("peer closed")
        value.extend(part)
    return bytes(value)


def _message(kind: bytes, payload: bytes) -> bytes:
    return kind + struct.pack("!I", len(payload) + 4) + payload


def _ready_messages() -> bytes:
    parameter = b"server_version\x0017.0\x00"
    return b"".join(
        (
            _message(b"R", struct.pack("!I", 0)),
            _message(b"S", parameter),
            _message(b"K", struct.pack("!II", 1234, 5678)),
            _message(b"Z", b"I"),
        )
    )


def _set_result() -> bytes:
    return _message(b"C", b"SET\x00") + _message(b"Z", b"I")


def _select_result() -> bytes:
    field = b"?column?\x00" + struct.pack("!IhIhih", 0, 0, 23, 4, -1, 0)
    return b"".join(
        (
            _message(b"T", struct.pack("!h", 1) + field),
            _message(b"D", struct.pack("!hI", 1, 1) + b"1"),
            _message(b"C", b"SELECT 1\x00"),
            _message(b"Z", b"I"),
        )
    )


def _read_query(secure: ssl.SSLSocket) -> None:
    if _exact(secure, 1) != b"Q":
        raise AssertionError("expected simple Query message")
    query_length = struct.unpack("!I", _exact(secure, 4))[0]
    if not 5 <= query_length <= 10000:
        raise AssertionError("invalid Query length")
    _exact(secure, query_length - 4)


class _StartupServer:
    def __init__(self, certificate: Path, key: Path, *, delay_after_startup: float = 0):
        self._certificate = certificate
        self._key = key
        self._delay = delay_after_startup
        self._listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._listener.bind(("127.0.0.1", 0))
        self._listener.listen(1)
        self._listener.settimeout(15)
        self.port = self._listener.getsockname()[1]
        self.phases: list[tuple[str, float]] = []
        self.query_received = False
        self.error: Optional[BaseException] = None
        self._started = time.monotonic()
        self._thread = threading.Thread(target=self._serve, name="offline-pg-startup", daemon=True)

    def _phase(self, name: str) -> None:
        self.phases.append((name, time.monotonic() - self._started))

    def start(self) -> None:
        self._thread.start()

    def finish(self) -> None:
        self._thread.join(timeout=15)
        self._listener.close()
        if self._thread.is_alive():
            raise AssertionError("synthetic startup server did not stop")
        if self.error is not None:
            raise self.error

    def _serve(self) -> None:
        try:
            plain, _ = self._listener.accept()
            with plain:
                plain.settimeout(15)
                request = _exact(plain, 8)
                if request != struct.pack("!II", 8, 80877103):
                    raise AssertionError("expected PostgreSQL SSLRequest")
                self._phase("ssl_request")
                plain.sendall(b"S")

                context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
                context.load_cert_chain(self._certificate, self._key)
                with context.wrap_socket(plain, server_side=True) as secure:
                    secure.settimeout(15)
                    self._phase("tls_complete")
                    length = struct.unpack("!I", _exact(secure, 4))[0]
                    if not 8 <= length <= 10000:
                        raise AssertionError("invalid StartupMessage length")
                    startup = _exact(secure, length - 4)
                    if startup[:4] != struct.pack("!I", 196608):
                        raise AssertionError("expected PostgreSQL protocol 3 startup")
                    self._phase("startup_received")

                    if self._delay:
                        time.sleep(self._delay)
                        secure.settimeout(0.5)
                        try:
                            self.query_received = secure.recv(1) == b"Q"
                        except (OSError, ssl.SSLError):
                            self.query_received = False
                        self._phase("delay_complete")
                        return

                    secure.sendall(_ready_messages())
                    self._phase("ready_sent")
                    _read_query(secure)
                    self.query_received = True
                    self._phase("set_query_received")
                    secure.sendall(_set_result())
                    _read_query(secure)
                    self._phase("select_query_received")
                    secure.sendall(_select_result())
                    self._phase("result_sent")
                    termination = _exact(secure, 1)
                    if termination != b"X" or struct.unpack("!I", _exact(secure, 4))[0] != 4:
                        raise AssertionError(f"expected client Terminate message, got type {termination.hex()}")
                    self._phase("client_terminated")
        except BaseException as error:  # delivered to the unittest thread
            self.error = error


@contextmanager
def _server(certificate: Path, key: Path, *, delay_after_startup: float = 0):
    value = _StartupServer(certificate, key, delay_after_startup=delay_after_startup)
    value.start()
    try:
        yield value
    finally:
        value.finish()


class LibpqStartupPhaseTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.openssl = shutil.which("openssl")
        if not Path(pilot_live.PSQL).is_file() or cls.openssl is None:
            raise unittest.SkipTest("bundled psql or openssl is unavailable")
        cls.temporary = tempfile.TemporaryDirectory(prefix="odbcpp-pg-startup-")
        cls.directory = Path(cls.temporary.name).resolve()
        cls.directory.chmod(0o700)
        cls._make_certificates()

    @classmethod
    def tearDownClass(cls) -> None:
        if hasattr(cls, "temporary"):
            cls.temporary.cleanup()

    @classmethod
    def _run_openssl(cls, *arguments: str) -> None:
        subprocess.run(
            [cls.openssl, *arguments],
            cwd=cls.directory,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=True,
            timeout=15,
        )

    @classmethod
    def _make_certificates(cls) -> None:
        (cls.directory / "ca.cnf").write_text(
            "[req]\nprompt=no\ndistinguished_name=dn\n[dn]\nCN=ODBCPP Offline CA\n"
            "[v3_ca]\nbasicConstraints=critical,CA:TRUE\nkeyUsage=critical,keyCertSign\n"
        )
        (cls.directory / "server.cnf").write_text(
            "[req]\nprompt=no\ndistinguished_name=dn\nreq_extensions=req_ext\n"
            "[dn]\nCN=localhost\n[req_ext]\nsubjectAltName=DNS:localhost\n"
            "[server_ext]\nbasicConstraints=critical,CA:FALSE\n"
            "keyUsage=critical,digitalSignature,keyEncipherment\n"
            "extendedKeyUsage=serverAuth\nsubjectAltName=DNS:localhost\n"
        )
        cls._run_openssl(
            "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
            "-keyout", "ca.key", "-out", "ca.crt", "-config", "ca.cnf", "-extensions", "v3_ca",
        )
        cls._run_openssl(
            "req", "-newkey", "rsa:2048", "-nodes", "-keyout", "server.key",
            "-out", "server.csr", "-config", "server.cnf",
        )
        cls._run_openssl(
            "x509", "-req", "-days", "1", "-in", "server.csr", "-CA", "ca.crt",
            "-CAkey", "ca.key", "-CAcreateserial", "-out", "server.crt",
            "-extfile", "server.cnf", "-extensions", "server_ext",
        )
        for path in cls.directory.iterdir():
            path.chmod(0o600)

    def _window(self, port: int, directory: Path, seconds: int = 25) -> pilot_live.Window:
        config = {
            "host": "localhost",
            "port": port,
            "database": "offline",
            "admin_user": "offline_admin",
            "test_user": "offline_test",
            "admin_password": "offline-password",
            "test_password": "offline-password",
            "ca_file": str(self.directory / "ca.crt"),
        }
        return pilot_live.Window(directory, config, time.monotonic() + seconds)

    def test_exact_window_sql_reaches_ready_and_query(self) -> None:
        with tempfile.TemporaryDirectory(prefix="happy-", dir=self.directory) as raw:
            output = Path(raw)
            output.chmod(0o700)
            with _server(self.directory / "server.crt", self.directory / "server.key") as server:
                result = self._window(server.port, output).sql("SELECT 1;")
            self.assertEqual(result, "1")
            self.assertTrue(server.query_received)
            self.assertEqual(
                [name for name, _ in server.phases],
                ["ssl_request", "tls_complete", "startup_received", "ready_sent", "set_query_received",
                 "select_query_received", "result_sent", "client_terminated"],
            )

    def test_post_tls_startup_delay_times_out_before_sql(self) -> None:
        with tempfile.TemporaryDirectory(prefix="delay-", dir=self.directory) as raw:
            output = Path(raw)
            output.chmod(0o700)
            started = time.monotonic()
            with _server(
                self.directory / "server.crt", self.directory / "server.key", delay_after_startup=11
            ) as server:
                with self.assertRaisesRegex(Blocked, "sql_step_failed"):
                    self._window(server.port, output).sql("SELECT 1;")
            elapsed = time.monotonic() - started
            self.assertGreaterEqual(elapsed, 10)
            self.assertLess(elapsed, 15)
            self.assertIn("timeout expired", (output / "bootstrap-01-sql.log").read_text())
            self.assertFalse(server.query_received)
            self.assertEqual(
                [name for name, _ in server.phases],
                ["ssl_request", "tls_complete", "startup_received", "delay_complete"],
            )


if __name__ == "__main__":
    unittest.main()
