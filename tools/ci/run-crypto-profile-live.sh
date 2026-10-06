#!/usr/bin/env bash
set -euo pipefail
build_dir="${1:?build directory required}"
results="${2:?test result path required}"
: "${ODBCPP_POSTGRES_TLS_DIR:?TLS fixture directory required}"
base="DRIVER={ODBCPP PostgreSQL};SERVER=127.0.0.1;PORT=${PGPORT:-5432};DATABASE=postgres;UID=postgres;PWD=postgres;SSL=1;"
wrong_host="${ODBCPP_CRYPTO_PROFILE_WRONG_HOST:-wrong-host.invalid}"
export ODBCPP_CRYPTO_PROFILE_TEST_CONNECTION="${base}SSLCAFILE=$ODBCPP_POSTGRES_TLS_DIR/server.crt;"
export ODBCPP_CRYPTO_PROFILE_WRONG_CA_CONNECTION="${base}SSLCAFILE=$ODBCPP_POSTGRES_TLS_DIR/wrong.crt;"
export ODBCPP_CRYPTO_PROFILE_WRONG_HOST_CONNECTION="${base/SERVER=127.0.0.1/SERVER=$wrong_host}SSLCAFILE=$ODBCPP_POSTGRES_TLS_DIR/server.crt;"
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

# Private AUTH-to-backend proof uses the same explicitly selected TLS fixture.
: "${PGPORT:?Explicit disposable fixture port required for ordinary Material TLS}"
material_results="${results%.xml}-auth-material.xml"
"$build_dir/tests/it_auth_pg_material_tls" --gtest_output="xml:$material_results"
python3 - "$material_results" <<'PYRESULT'
import sys
import xml.etree.ElementTree as ET
root = ET.parse(sys.argv[1]).getroot()
expected = {'OrdinaryPgMaterialTls.' + name for name in (
    'VerifiedScramMaterialConnectQueryOwnsAfterDisconnect',
    'NativeRefusalRetiresAndFreshMaterialRecovers')}
cases = list(root.iter('testcase'))
actual = {f'{case.attrib["classname"]}.{case.attrib["name"]}' for case in cases}
if len(cases) != 2 or actual != expected or any(list(root.iter(tag)) for tag in ('failure', 'error', 'skipped')):
    raise RuntimeError('Mandatory ordinary Material TLS cases did not all pass without skips')
PYRESULT
