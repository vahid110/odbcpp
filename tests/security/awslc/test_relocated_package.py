"""Relocate an internal driver-only package and retain byte-bound evidence."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tarfile
import tempfile


def run(command, env):
    result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=75)
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    return result.stdout


def main():
    parser = argparse.ArgumentParser()
    for name in ("build", "cmake", "readelf", "shared", "test"):
        parser.add_argument("--" + name, required=True)
    args = parser.parse_args()
    build = Path(args.build).resolve(strict=True)
    env = os.environ.copy()
    env.pop("LD_LIBRARY_PATH", None)
    # The system OpenSSL preload remains; no AWS-LC path is supplied to the loader.
    with tempfile.TemporaryDirectory(prefix="odbcpp-package-") as directory:
        root = Path(directory)
        staged = root / "staged"
        run([args.cmake, "--install", str(build), "--component", "awslc-proof",
             "--prefix", str(staged)], env)
        package = root / "relocated"
        staged.rename(package)
        driver = package / "lib/libawslc_odbc_driver.so"
        required = [driver, *(package / "licenses" / path for path in
                    ("aws-lc/LICENSE", "aws-lc/NOTICE", "spdlog/LICENSE", "fmt/LICENSE"))]
        for path in required:
            if not path.is_file() or not path.stat().st_size:
                raise RuntimeError(f"Missing package input: {path}")
        libraries = list((package / "lib").glob("lib*-awslc.so*"))
        shared = args.shared.upper() in ("ON", "TRUE", "1")
        if bool(libraries) != shared:
            raise RuntimeError("Packaged provider files do not match requested linkage")
        for path in [driver, *libraries]:
            dynamic = run([args.readelf, "-d", str(path)], env)
            paths = re.findall(r"\((?:RUNPATH|RPATH)\).*?\[([^]]*)\]", dynamic)
            if paths != ["$ORIGIN"]:
                raise RuntimeError(f"Unexpected runtime paths for {path.name}: {paths}")
        resolution = run(["ldd", str(driver)], env)
        resolved = re.findall(r"(lib(?:ssl|crypto)-awslc[^\s]*)\s+=>\s+(\S+)", resolution)
        if shared and len(resolved) != 2:
            raise RuntimeError("Expected exactly two packaged AWS-LC dependencies:\n" + resolution)
        if not shared and resolved:
            raise RuntimeError("Static driver depends on shared AWS-LC")
        for _, path in resolved:
            if not Path(path).resolve(strict=True).is_relative_to(package):
                raise RuntimeError("Provider resolved outside relocated package: " + path)
        run([sys.executable, str(Path(__file__).with_name("test_odbc_driver.py")),
             args.test, str(driver)], env)
        if shared:
            # Remove each actual SONAME target in turn, including its symlink
            # target. A new process must fail to load, never find a build-tree copy.
            for soname, path in resolved:
                library = Path(path).resolve(strict=True)
                hidden = root / (library.name + ".hidden")
                library.rename(hidden)
                try:
                    result = subprocess.run([sys.executable, "-c",
                        "import ctypes,sys; ctypes.CDLL(sys.argv[1])", str(driver)],
                        env=env, capture_output=True, text=True, timeout=15)
                    if result.returncode == 0 or soname not in result.stderr:
                        raise RuntimeError("Missing provider did not fail as expected: " + result.stderr)
                finally:
                    hidden.rename(library)
        files = {}
        for path in sorted(package.rglob("*")):
            if path.is_dir():
                continue
            actual = path.resolve(strict=True)
            if not actual.is_relative_to(package):
                raise RuntimeError("Package symlink escaped root")
            if path.suffix in (".h", ".a"):
                raise RuntimeError("Driver-only package contains development files")
            files[str(path.relative_to(package))] = {
                "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                "symlink": os.readlink(path) if path.is_symlink() else None,
            }
        archive = build / "awslc-driver-proof.tar.gz"
        with tarfile.open(archive, "w:gz") as output:
            output.add(package, arcname="odbcpp-awslc-proof")
        evidence = {
            "scope": "internal-relocated-driver-proof", "files": files,
            "archiveSha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
            "linkage": "shared" if shared else "static", "relocatedLiveTestsPassed": True,
            "missingProviderRejected": True if shared else None,
            "hostileLoaderPathTested": False, "qualificationClaimed": False,
        }
        (build / "relocation-evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")


if __name__ == "__main__":
    main()
