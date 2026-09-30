"""Register only the proof driver and run common unixODBC TLS acceptance."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def quoted(value):
    return "{" + value.replace("}", "}}") + "}"


with tempfile.TemporaryDirectory(prefix="odbcpp-awslc-dm-") as directory:
    root = Path(directory)
    driver = str(Path(sys.argv[2]).resolve(strict=True))
    (root / "odbcinst.ini").write_text(
        "[ODBCPP PostgreSQL]\nDriver=" + driver + "\nDriverUnicodeType=UTF16\n")
    (root / "odbc.ini").write_text("")
    env = os.environ.copy()
    env.update(ODBCSYSINI=directory, ODBCINI=str(root / "odbc.ini"), ODBCINSTINI="odbcinst.ini")
    common = "DRIVER={ODBCPP PostgreSQL};DATABASE=postgres;SSL=1;"
    for key, variable in (("PORT", "ODBCPP_AWSLC_PG_PORT"), ("UID", "ODBCPP_AWSLC_PG_USER"),
                          ("PWD", "ODBCPP_AWSLC_PG_PASSWORD")):
        common += key + "=" + quoted(env[variable]) + ";"
    for variable, host, ca in (
        ("ODBCPP_CRYPTO_PROFILE_TEST_CONNECTION", "127.0.0.1", "ODBCPP_AWSLC_PG_CA"),
        ("ODBCPP_CRYPTO_PROFILE_WRONG_CA_CONNECTION", "127.0.0.1", "ODBCPP_AWSLC_PG_WRONG_CA"),
        ("ODBCPP_CRYPTO_PROFILE_WRONG_HOST_CONNECTION", "localhost", "ODBCPP_AWSLC_PG_CA")):
        env[variable] = common + "SERVER=" + host + ";SSLCAFILE=" + quoted(env[ca]) + ";"
    result = subprocess.run([sys.argv[1]], env=env, timeout=60)
    raise SystemExit(result.returncode)
