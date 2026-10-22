"""Private input staging for the local CTest checkpoint.

CTest owns test execution and results. This module only prepares connection
environments and tracks the exact files that the resource owner must remove.
Never put its returned environment values in public logs or manifests.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import re


LOG_OPTIONS = {
    "LogLevel": "Debug", "LogFormat": "Json", "LogSink": "Stderr",
    "LogAsync": "false", "LogQueries": "false",
}


def logged_connection(value: str) -> str:
    return value.rstrip(";") + ";" + "".join(
        key + "=" + setting + ";" for key, setting in LOG_OPTIONS.items())


class LoggingFactory:
    """Enable the owned driver's existing logger while retaining input formats."""

    def __init__(self, factory):
        self.factory = factory

    def __getattr__(self, name):
        return getattr(self.factory, name)

    def build_profile_connection(self, config, profile, **kwargs):
        value = self.factory.build_profile_connection(config, profile, **kwargs)
        # Package tests consume a closed connection-file format. Both owned and
        # vendor DM files must retain the builder's exact fields for preflight.
        kind = profile["private_input"]["kind"]
        return value if kind in ("OWN_DM_FILE", "VENDOR_DM_FILE") else logged_connection(value)

    def dsn_documents(self, *args, **kwargs):
        dsn, registration = self.factory.dsn_documents(*args, **kwargs)
        # The existing builder returns one RedshiftProd section and one driver
        # registration. Add to the DSN so SQLConnect and SQLDriverConnect agree.
        dsn += "".join(key + "=" + value + "\n" for key, value in LOG_OPTIONS.items())
        return dsn, registration


def stage_case_environments(directory, blueprint, specs, profiles, connections,
                            private_environments, *, two_database_values=None):
    """Create private JSON environment files; return an index and cleanup custody.

    Runtime profiles must already contain actual fixture identities. Inputs are
    held by the lifecycle owner, not supplied from ambient environment variables.
    """
    directory = Path(directory)
    if not directory.is_absolute() or directory.exists():
        raise ValueError("New absolute private environment directory required")
    if len(blueprint["cases"]) != len(specs) or not specs:
        raise ValueError("Complete CTest inventory required")
    directory.mkdir(mode=0o700)
    result = dict(blueprint, profiles={}, state="PRIVATE_ENVIRONMENTS_STAGED")
    custody = []
    staged_payloads = {}
    try:
        for case, spec in zip(blueprint["cases"], specs):
            if (case["id"] != spec["ctest_id"] or
                    case["gtest_filter"] != spec["case"] or
                    case["image"] != spec["image"] or
                    case["profile"] != spec["ctest_profile"]):
                raise ValueError("CTest case/profile mismatch")
            key = str(spec["profile"])
            profile = profiles[key]
            env = {"PATH": "/usr/bin:/bin", "LANG": "C", "LC_ALL": "C"}
            if profile["private_input"]["kind"] != "OWN_DSN":
                env.update({name: connections[key]
                            for name in profile["connection_environment"]})
            env.update(profile["variables"])
            env.update(private_environments[key])
            if spec["env"] is not None:
                env[spec["env"]] = spec["marker"]
            if spec["case"] == "RedshiftMetadataWorkflowRealTest.MetadataToPreparedQueryShowConfigured":
                env.update(ODBCPP_REDSHIFT_TEST_SCHEMA="odbcpp_fixture",
                           ODBCPP_REDSHIFT_TEST_TABLE="pilot_rows")
            env.update((two_database_values or {}).get(spec["key"], {}))
            if any(not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name) or
                   not isinstance(value, str) or any(c in value for c in "\0\r\n")
                   for name, value in env.items()):
                raise ValueError("Invalid private test environment")
            name = case["profile"]
            if not re.fullmatch(r"[A-Za-z0-9_-]+", name):
                raise ValueError("Invalid CTest profile identifier")
            path = directory / (name + ".json")
            body = json.dumps({"environment": [key + "=" + value
                                               for key, value in env.items()]}).encode('utf-8')
            if name in staged_payloads:
                previous, held = staged_payloads[name]
                if body != previous:
                    raise ValueError("Shared profile has different environment payloads")
                fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
                try:
                    info = os.fstat(fd)
                    if ((info.st_dev, info.st_ino) != (held['device'], held['inode']) or
                            os.read(fd, len(body) + 1) != body):
                        raise RuntimeError("Shared environment file changed")
                finally:
                    os.close(fd)
                continue
            fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
            info = os.fstat(fd)
            held = {"path": str(path), "device": info.st_dev, "inode": info.st_ino}
            custody.append(held)
            with os.fdopen(fd, "wb") as stream:
                if stream.write(body) != len(body):
                    raise OSError("Short private environment write")
                stream.flush()
                os.fsync(stream.fileno())
            staged_payloads[name] = (body, held)
            result["profiles"][name] = {"environment_file": str(path)}
    except BaseException:
        # These files have no readers before this function returns.
        close_case_environments(custody, directory)
        raise
    return result, custody


def close_case_environments(custody, directory):
    """Called only after the owner has reaped CMake/CTest and their descendants."""
    for row in reversed(custody):
        path = Path(row["path"])
        info = path.lstat()
        if (info.st_dev, info.st_ino) != (row["device"], row["inode"]) or path.is_symlink():
            raise RuntimeError("Private environment file identity changed")
        path.unlink()
    Path(directory).rmdir()


def generated_environment_custody(build_directory):
    """Capture CMake's additional secret-bearing file after configure is reaped.

    This standalone project generates one CTestTestfile. Logs and XML remain
    private evidence; they are not removed by this input-retirement function.
    """
    path = Path(build_directory) / "CTestTestfile.cmake"
    info = path.lstat()
    import stat
    if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid() or
            info.st_nlink != 1 or stat.S_IMODE(info.st_mode) != 0o600):
        raise RuntimeError("Generated test environment is not a private regular file")
    return {"path": str(path), "device": info.st_dev, "inode": info.st_ino,
            "size": info.st_size, "mtime_ns": info.st_mtime_ns}


def close_generated_environment(custody):
    """Retire generated connection values only after all CTest readers exit."""
    path = Path(custody["path"])
    info = path.lstat()
    if (path.is_symlink() or
            (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns) !=
            (custody["device"], custody["inode"], custody["size"], custody["mtime_ns"])):
        raise RuntimeError("Generated test environment identity changed")
    path.unlink()
