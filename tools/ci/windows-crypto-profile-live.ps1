$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true
$tls = Join-Path $env:RUNNER_TEMP 'odbcpp-postgres-tls'
foreach ($file in @('server.crt', 'wrong.crt')) {
    if (!(Test-Path (Join-Path $tls $file))) { throw "TLS fixture missing: $file" }
}
$base = 'DRIVER={ODBCPP PostgreSQL};SERVER=127.0.0.1;PORT=5432;DATABASE=postgres;UID=postgres;PWD=postgres;SSL=1;'
$env:ODBCPP_CRYPTO_PROFILE_TEST_CONNECTION = "${base}SSLCAFILE=$tls/server.crt;"
$env:ODBCPP_CRYPTO_PROFILE_WRONG_CA_CONNECTION = "${base}SSLCAFILE=$tls/wrong.crt;"
# IP-only SAN; localhost reaches the fixture while requiring another peer identity.
$env:ODBCPP_CRYPTO_PROFILE_WRONG_HOST_CONNECTION = "$($base.Replace('SERVER=127.0.0.1', 'SERVER=localhost'))SSLCAFILE=$tls/server.crt;"
& ./build-windows/tests/Release/it_crypto_profile_live.exe --gtest_output=xml:windows-crypto-profile-live.xml
[xml]$report = Get-Content windows-crypto-profile-live.xml -Raw
$cases = @($report.SelectNodes('//testcase'))
$expected = @('CompletesVerifiedTlsScramQuery', 'RejectsUntrustedCertificate', 'RejectsHostnameMismatch')
$actual = @($cases | ForEach-Object { $_.name })
if ($cases.Count -ne 3 -or @(Compare-Object $expected $actual).Count -ne 0 -or
    @($report.SelectNodes('//failure | //error | //skipped')).Count -ne 0) {
    throw 'Mandatory Windows TLS cases did not all pass without skips'
}
