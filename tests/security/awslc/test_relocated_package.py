"""Relocate an internal driver-only package and retain byte-bound evidence."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile


def run(command, env):
    result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=75)
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    return result.stdout


def inventory(package):
    package = package.resolve(strict=True)
    files = {}
    for path in sorted(package.rglob("*")):
        actual = path.resolve(strict=True)
        if not actual.is_relative_to(package):
            raise RuntimeError("Package symlink escaped root")
        if path.is_symlink() and not actual.is_file():
            raise RuntimeError("Package symlink must target a regular file")
        if path.is_dir():
            continue
        if not path.is_file() or path.suffix in (".h", ".a"):
            raise RuntimeError("Driver-only package contains an unsupported file")
        files[str(path.relative_to(package))] = {
            "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            "symlink": os.readlink(path) if path.is_symlink() else None,
            "mode": path.stat().st_mode & 0o777,
        }
    return files


def extract_verified(archive, destination, expected, root_name="odbcpp-awslc-proof"):
    if not re.fullmatch(r"[A-Za-z0-9_-]+", root_name):
        raise RuntimeError("Package archive root must be a simple directory name")
    # This consumes our own archive, with an independent pre-archive inventory.
    # The standard data filter rejects paths/links outside the extraction root.
    with tarfile.open(archive, "r:gz") as source:
        names = [member.name for member in source.getmembers()]
        if len(names) != len(set(names)):
            raise RuntimeError("Duplicate package archive member")
        source.extractall(destination, filter="data")
    package = destination / root_name
    if set(destination.iterdir()) != {package} or inventory(package) != expected:
        raise RuntimeError("Extracted package differs from staged inventory")
    return package


def validate_manifest(package, shared):
    manifest = json.loads((package / "package-manifest.json").read_text())
    expected_linkage = "BUNDLED_SHARED" if shared else "BUNDLED_STATIC"
    if (manifest.get("schemaVersion") != 1 or manifest.get("provider") != "AWS_LC"
            or manifest.get("linkage") != expected_linkage
            or manifest.get("fipsRequested") is not False
            or manifest.get("qualificationClaimed") is not False
            or manifest.get("sourceProvenanceVerified") is not False):
        raise RuntimeError("Package manifest profile or qualification mismatch")
    recipes = manifest.get("declaredSourceRecipes", {})
    if set(recipes) != {"aws-lc", "spdlog", "fmt"}:
        raise RuntimeError("Package source recipe inventory mismatch")
    for name in ("aws-lc", "spdlog"):
        recipe = recipes[name]
        if (not isinstance(recipe, dict)
                or not re.fullmatch(r"[0-9a-f]{64}", str(recipe.get("archiveSha256", "")))
                or type(recipe.get("sourceOverride")) is not bool):
            raise RuntimeError("Package source recipe is malformed: " + name)
    if (not re.fullmatch(r"[0-9a-f]{40}", str(recipes["aws-lc"].get("commit", "")))
            or not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", str(recipes["spdlog"].get("version", "")))
            or not isinstance(recipes["fmt"], dict)
            or recipes["fmt"].get("bundledBy") != "spdlog"
            or type(recipes["fmt"].get("versionNumber")) is not int
            or recipes["fmt"]["versionNumber"] <= 0):
        raise RuntimeError("Package source recipe identity is malformed")
    required = {"licenses/aws-lc/LICENSE", "licenses/aws-lc/NOTICE",
                "licenses/spdlog/LICENSE", "licenses/fmt/LICENSE"}
    licenses = manifest.get("licenseFiles", {})
    if set(licenses) != required:
        raise RuntimeError("Package manifest license inventory mismatch")
    for name, expected_hash in licenses.items():
        path = package / name
        if not path.is_file() or not path.stat().st_size:
            raise RuntimeError("Package license missing or empty: " + name)
        if hashlib.sha256(path.read_bytes()).hexdigest() != expected_hash:
            raise RuntimeError("Package license hash mismatch: " + name)
    return manifest


def validate_loaded_providers(paths, expected):
    actual = {Path(path).resolve(strict=True) for path in paths}
    selected = {Path(path).resolve(strict=True) for path in expected}
    if actual != selected:
        raise RuntimeError("Loaded AWS-LC provider origin mismatch")


def loaded_providers(driver, env):
    # Inspect a fresh host after dlopen; ldd alone cannot establish the libraries
    # actually mapped into a host with LD_PRELOAD overrides.
    probe = """
import ctypes,json,pathlib,sys
ctypes.CDLL(sys.argv[1])
paths = set()
for line in pathlib.Path('/proc/self/maps').read_text().splitlines():
    fields = line.split(None, 5)
    if len(fields) != 6:
        continue
    path = fields[5]
    if pathlib.Path(path).name.startswith(('libssl-awslc', 'libcrypto-awslc')):
        paths.add(path)
print(json.dumps(sorted(paths)))
"""
    return json.loads(run([sys.executable, "-c", probe, str(driver)], env))


def substitution_canaries(driver, resolved, root, env):
    external = root / "external-providers"
    external.mkdir()
    for soname, path in resolved:
        shutil.copy2(Path(path).resolve(strict=True), external / soname)
    expected = [path for _, path in resolved]
    detected = {}
    for mode in ("libraryPath", "preload"):
        injected = env.copy()
        if mode == "libraryPath":
            injected["LD_LIBRARY_PATH"] = str(external)
        else:
            injected["LD_PRELOAD"] = ":".join(
                str(external / soname) for soname, _ in resolved)
            if env.get("LD_PRELOAD"):
                injected["LD_PRELOAD"] += ":" + env["LD_PRELOAD"]
        paths = loaded_providers(driver, injected)
        # First prove the override was exercised, using known byte-identical
        # libraries rather than loading any untrusted replacement code.
        if not any(Path(path).resolve(strict=True).is_relative_to(external) for path in paths):
            raise RuntimeError("Loader substitution canary did not map an external provider")
        try:
            validate_loaded_providers(paths, expected)
        except RuntimeError as error:
            if str(error) != "Loaded AWS-LC provider origin mismatch":
                raise
            detected[mode] = True
        else:
            raise RuntimeError("External provider substitution escaped origin detection")
    return detected


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
        root = Path(directory).resolve(strict=True)
        staged = root / "staged"
        run([args.cmake, "--install", str(build), "--component", "awslc-proof",
             "--prefix", str(staged)], env)
        files = inventory(staged)
        archive = build / "awslc-driver-proof.tar.gz"
        with tarfile.open(archive, "w:gz") as output:
            output.add(staged, arcname="odbcpp-awslc-proof")
        shutil.rmtree(staged)
        package = extract_verified(archive, root / "extracted", files)
        driver = package / "lib/libawslc_odbc_driver.so"
        required = [driver, *(package / "licenses" / path for path in
                    ("aws-lc/LICENSE", "aws-lc/NOTICE", "spdlog/LICENSE", "fmt/LICENSE"))]
        for path in required:
            if not path.is_file() or not path.stat().st_size:
                raise RuntimeError(f"Missing package input: {path}")
        libraries = list((package / "lib").glob("lib*-awslc.so*"))
        shared = args.shared.upper() in ("ON", "TRUE", "1")
        manifest = validate_manifest(package, shared)
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
        validate_loaded_providers(loaded_providers(driver, env), [path for _, path in resolved])
        substitutions = substitution_canaries(driver, resolved, root, env) if shared else None
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
        evidence = {
            "scope": "internal-relocated-driver-proof", "files": files,
            "packageManifest": manifest,
            "archiveSha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
            "linkage": "shared" if shared else "static", "relocatedLiveTestsPassed": True,
            "extractedArchiveTested": True,
            "missingProviderRejected": True if shared else None,
            "hostileLoaderPathTested": shared,
            "loaderSubstitutionDetected": substitutions,
            "loaderSubstitutionPrevented": False, "qualificationClaimed": False,
        }
        (build / "relocation-evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")


if __name__ == "__main__":
    main()
