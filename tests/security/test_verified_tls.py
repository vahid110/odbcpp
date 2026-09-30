"""Exercise the production TLS client against an independent Python/OpenSSL TLS peer."""
import argparse
import concurrent.futures
import pathlib
import socket
import ssl
import subprocess
import tempfile


def certificate(openssl, root, name):
    cert, key = root / (name + ".pem"), root / (name + ".key")
    subprocess.run([
        openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
        "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1",
        "-keyout", str(key), "-out", str(cert),
    ], check=True, capture_output=True, timeout=30)
    return cert, key


def run_case(probe, context, ca, host, expected):
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        listener.settimeout(10)

        def serve():
            raw, _ = listener.accept()
            raw.settimeout(5)
            with raw:
                try:
                    with context.wrap_socket(raw, server_side=True) as peer:
                        if expected == "accept":
                            payload = b""
                            while len(payload) < 4:
                                part = peer.recv(4 - len(payload))
                                if not part:
                                    raise AssertionError("client closed before payload")
                                payload += part
                            if payload != b"p\x00\xffg":
                                raise AssertionError(f"unexpected payload: {payload!r}")
                            peer.sendall(payload)
                        else:
                            if peer.recv(1) != b"":
                                raise AssertionError("rejected peer received application bytes")
                except ssl.SSLError as error:
                    # Trust rejection sends an alert; hostname rejection occurs
                    # after TLS negotiation and may close without close_notify.
                    if expected == "accept":
                        raise
                    if expected == "protocol":
                        if error.reason != "UNSUPPORTED_PROTOCOL":
                            raise
                    elif error.reason not in ("TLSV1_ALERT_UNKNOWN_CA", "SSLV3_ALERT_BAD_CERTIFICATE",
                                            "UNEXPECTED_EOF_WHILE_READING"):
                        raise
                except (ConnectionResetError, BrokenPipeError):
                    # A rejected client can close while the TLS 1.3 peer is
                    # sending session tickets. The probe must still report the
                    # specific verification failure before this case passes.
                    if expected == "accept":
                        raise

        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
            server = pool.submit(serve)
            if probe is None:
                # A successful independent legacy client rules out a disabled
                # server/cipher fixture masquerading as client-policy rejection.
                control = ssl.create_default_context(cafile=str(ca))
                control.minimum_version = control.maximum_version = ssl.TLSVersion.TLSv1_1
                control.set_ciphers("DEFAULT:@SECLEVEL=0")
                with socket.create_connection(listener.getsockname(), timeout=5) as raw:
                    with control.wrap_socket(raw, server_hostname=host) as peer:
                        if peer.version() != "TLSv1.1":
                            raise AssertionError("legacy control negotiated another protocol")
                        peer.sendall(b"p\x00\xffg")
                        received = b""
                        while len(received) < 4:
                            part = peer.recv(4 - len(received))
                            if not part:
                                raise AssertionError("legacy control closed before echo")
                            received += part
                        if received != b"p\x00\xffg":
                            raise AssertionError("legacy control payload mismatch")
            else:
                result = subprocess.run([probe, str(listener.getsockname()[1]), host,
                                         str(ca), expected], capture_output=True, text=True, timeout=15)
                if result.returncode != 0:
                    raise AssertionError(result.stdout + result.stderr)
            server.result(timeout=10)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", required=True)
    parser.add_argument("--openssl", required=True)
    args = parser.parse_args()
    if not ssl.HAS_TLSv1_3:
        raise RuntimeError("This proof requires a Python SSL runtime with TLS 1.3 support")
    print(f"Independent TLS peer: {ssl.OPENSSL_VERSION}", flush=True)
    with tempfile.TemporaryDirectory(prefix="odbcpp-tls-proof-") as directory:
        root = pathlib.Path(directory)
        cert, key = certificate(args.openssl, root, "trusted")
        other, _ = certificate(args.openssl, root, "unrelated")
        legacy = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        legacy.minimum_version = legacy.maximum_version = ssl.TLSVersion.TLSv1_1
        legacy.set_ciphers("DEFAULT:@SECLEVEL=0")
        legacy.load_cert_chain(cert, key)
        run_case(None, legacy, cert, "localhost", "accept")
        run_case(args.probe, legacy, cert, "localhost", "protocol")
        print("TLSv1_1: control accepted; production client rejected downgrade", flush=True)
        for version in (ssl.TLSVersion.TLSv1_2, ssl.TLSVersion.TLSv1_3):
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.minimum_version = context.maximum_version = version
            context.load_cert_chain(cert, key)
            for host, ca, expected in (("localhost", cert, "accept"),
                                       ("127.0.0.1", cert, "accept"),
                                       ("wrong.example.test", cert, "hostname"),
                                       ("127.0.0.2", cert, "hostname"),
                                       ("localhost", other, "trust")):
                run_case(args.probe, context, ca, host, expected)
                print(f"{version.name}: {host}: {expected} passed", flush=True)


if __name__ == "__main__":
    main()
