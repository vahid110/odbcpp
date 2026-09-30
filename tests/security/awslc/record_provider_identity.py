"""Retain non-secret identity/default-policy evidence from the production adapter."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def validate(lines, manifest):
    if len(lines) != 7:
        raise RuntimeError("Incomplete provider identity record")
    provider, compile_version, runtime_version, fips, minimum, peer, hostname = lines
    expected = manifest.get("compileVersion", "")
    if (provider != "AWS_LC" or not expected.startswith("AWS-LC ")
            or compile_version != f"OpenSSL 1.1.1 (compatible; {expected})"
            or runtime_version != expected
            or (fips, minimum, peer, hostname) != ("0", "771", "1", "1")):
        raise RuntimeError("Provider identity or TLS policy mismatch")
    return {
        "provider": provider, "compileVersion": compile_version,
        "runtimeVersion": runtime_version, "activeFips": False,
        "policyScope": "TLS probe defaults with explicit CA file in live cases",
        "minimumTlsVersion": "1.2", "verifyPeer": True, "verifyHostname": True,
        "qualificationClaimed": False,
    }


def main():
    parser = argparse.ArgumentParser()
    for name in ("probe", "manifest", "output"):
        parser.add_argument("--" + name, required=True)
    args = parser.parse_args()
    probe = Path(args.probe).resolve(strict=True)
    manifest = Path(args.manifest).resolve(strict=True)
    result = subprocess.run([str(probe), "--identity"], check=True,
                            capture_output=True, text=True, timeout=15)
    evidence = validate(result.stdout.splitlines(), json.loads(manifest.read_text()))
    evidence["probeSha256"] = hashlib.sha256(probe.read_bytes()).hexdigest()
    evidence["manifestSha256"] = hashlib.sha256(manifest.read_bytes()).hexdigest()
    Path(args.output).write_text(json.dumps(evidence, indent=2) + "\n")


if __name__ == "__main__":
    main()
