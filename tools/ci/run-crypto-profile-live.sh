#!/usr/bin/env bash
set -euo pipefail
build_dir="${1:?build directory required}"
results="${2:?test result path required}"
: "${ODBCPP_POSTGRES_TLS_DIR:?TLS fixture directory required}"
base='DRIVER={ODBCPP PostgreSQL};SERVER=127.0.0.1;PORT=5432;DATABASE=postgres;UID=postgres;PWD=postgres;SSL=1;'
export ODBCPP_CRYPTO_PROFILE_TEST_CONNECTION="${base}SSLCAFILE=$ODBCPP_POSTGRES_TLS_DIR/server.crt;"
export ODBCPP_CRYPTO_PROFILE_WRONG_CA_CONNECTION="${base}SSLCAFILE=$ODBCPP_POSTGRES_TLS_DIR/wrong.crt;"
export ODBCPP_CRYPTO_PROFILE_WRONG_HOST_CONNECTION="${base/SERVER=127.0.0.1/SERVER=wrong-host.invalid}SSLCAFILE=$ODBCPP_POSTGRES_TLS_DIR/server.crt;"
"$build_dir/tests/it_crypto_profile_live" --gtest_output="xml:$results"
python3 - "$results" <<'PY'
import sys
import xml.etree.ElementTree as ET
root = ET.parse(sys.argv[1]).getroot()
expected = {'CryptoProfileLiveTest.' + name for name in (
    'CompletesVerifiedTlsScramQuery', 'RejectsUntrustedCertificate', 'RejectsHostnameMismatch')}
actual = {f'{case.attrib["classname"]}.{case.attrib["name"]}' for case in root.iter('testcase')}
if actual != expected or any(list(root.iter(tag)) for tag in ('failure', 'error', 'skipped')):
    raise RuntimeError('Mandatory crypto profile cases did not all pass without skips')
PY
