param([Parameter(Mandatory=$true)][ValidatePattern('^[0-9a-f]{40}$')][string]$ExpectedHead)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
if (!$IsWindows -or $env:GITHUB_SHA -cne $ExpectedHead) { throw 'head_or_platform_refused' }
if (Get-ChildItem Env: | Where-Object Name -Like 'GTEST_*') { throw 'nondefault_gtest_environment_refused' }
$build=[IO.Path]::GetFullPath((Join-Path $env:GITHUB_WORKSPACE 'build-windows'))
$exe=[IO.Path]::GetFullPath((Join-Path $build 'tests/Release/test_backend_contract.exe'))
$original=Join-Path $build 'windows-openssl-unit.xml'
$log=Join-Path $build 'Testing/Temporary/LastTest.log'
foreach ($f in @($exe,$original,$log)) {
  $item=Get-Item -LiteralPath $f
  if ($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'input_identity_refused' }
}
if ((Get-Item $original).Length -gt 16777216) { throw 'original_xml_limit' }
$settings=[Xml.XmlReaderSettings]::new();$settings.DtdProcessing=[Xml.DtdProcessing]::Prohibit
$settings.XmlResolver=$null;$settings.MaxCharactersInDocument=16777216
$reader=[Xml.XmlReader]::Create($original,$settings)
try { $xml=[Xml.XmlDocument]::new();$xml.XmlResolver=$null;$xml.Load($reader) } finally { $reader.Dispose() }
function Test-BackendDiagnosticEligibility([Xml.XmlDocument]$Report) {
  # CTest's original suite remains failed. Only decide whether this specific
  # backend diagnostic applies; never rerun it for an unrelated GUI failure.
  $root=$Report.DocumentElement
  # The XML adapter's .Name resolves CTest's name attribute, not the node name.
  if ($null -eq $root -or $root.get_Name() -cne 'testsuite') { throw 'original_suite_shape_refused' }
  $cases=@($root.SelectNodes('testcase'))
  if ($cases.Count -eq 0 -or $cases.Count -ne $Report.SelectNodes('//testcase').Count -or
      $root.GetAttribute('tests') -cnotmatch '^[1-9][0-9]*$' -or
      $root.GetAttribute('tests') -cne [string]$cases.Count) { throw 'original_case_inventory_refused' }
  $names=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
  $failed=0
  foreach ($case in $cases) {
    $name=$case.GetAttribute('name')
    if ($name -cnotmatch '^[A-Za-z_][A-Za-z0-9_]*$' -or !$names.Add($name)) { throw 'original_case_name_refused' }
    $failures=$case.SelectNodes('failure').Count
    $skips=$case.SelectNodes('skipped').Count
    $status=$case.GetAttribute('status')
    if ($failures -gt 1 -or $skips -gt 1 -or $case.SelectNodes('error').Count -ne 0 -or
        ($failures -eq 1 -and ($skips -ne 0 -or $status -cne 'fail')) -or
        ($failures -eq 0 -and $skips -eq 0 -and $status -cne 'run') -or
        ($skips -eq 1 -and $status -cne 'notrun')) { throw 'original_case_result_refused' }
    $failed+=$failures
  }
  if ($failed -eq 0 -or $root.GetAttribute('failures') -cne [string]$failed) { throw 'original_failure_count_refused' }
  $backend=@($cases | Where-Object { $_.GetAttribute('name') -ceq 'test_backend_contract' })
  if ($backend.Count -ne 1 -or $backend[0].SelectNodes('skipped').Count -ne 0) { throw 'original_backend_failure_unbound' }
  return $backend[0].SelectNodes('failure').Count -eq 1
}
if (!(Test-BackendDiagnosticEligibility $xml)) {
  Write-Output 'backend_diagnostic_not_applicable: original suite failed; test_backend_contract passed; no child started'
  exit 1
}
$outdir=Join-Path $build 'windows-backend-direct-exit009'
if (Test-Path -LiteralPath $outdir) { throw 'duplicate_diagnostic_refused' }
$null=New-Item -ItemType Directory -Path $outdir
$sid=[Security.Principal.WindowsIdentity]::GetCurrent().User
$acl=[Security.AccessControl.DirectorySecurity]::new();$acl.SetOwner($sid);$acl.SetAccessRuleProtection($true,$false)
$rule=[Security.AccessControl.FileSystemAccessRule]::new($sid,'FullControl','ContainerInherit,ObjectInherit','None','Allow')
$acl.AddAccessRule($rule);Set-Acl -LiteralPath $outdir -AclObject $acl
$checked=Get-Acl -LiteralPath $outdir
if (!$checked.AreAccessRulesProtected -or $checked.GetOwner([Security.Principal.SecurityIdentifier]).Value -cne $sid.Value) { throw 'private_owner_acl_refused' }
function Write-NewJson([string]$Name,$Value) {
  $path=Join-Path $outdir $Name
  $bytes=[Text.UTF8Encoding]::new($false).GetBytes(($Value | ConvertTo-Json -Depth 8))
  if ($bytes.Length -gt 16384) { throw 'metadata_limit' }
  $f=[IO.File]::Open($path,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
  try { $f.Write($bytes,0,$bytes.Length);$f.Flush($true) } finally { $f.Dispose() }
}
$exeHash=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant()
$binding=@{head=$ExpectedHead;exe=$exe;exe_sha256=$exeHash;original_ctest_xml_sha256=(Get-FileHash $original).Hash.ToLowerInvariant();original_ctest_log_sha256=(Get-FileHash $log).Hash.ToLowerInvariant();script_sha256=(Get-FileHash $PSCommandPath).Hash.ToLowerInvariant();arguments=@();case_count_expected=311;tree_unknown=$true;qualified=$false}
Write-NewJson 'binding.json' $binding
$p=[Diagnostics.Process]::new();$info=[Diagnostics.ProcessStartInfo]::new();$info.FileName=$exe
$info.WorkingDirectory=$build;$info.UseShellExecute=$false;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
$p.StartInfo=$info;$clock=[Diagnostics.Stopwatch]::StartNew();$started=$false;$parentReaped=$false;$stopReason=$null;$killRequested=$false;$bytes=0L;$exitSigned=$null;$exitHex=$null;$channels=@();$errors=[Collections.Generic.List[string]]::new()
try {
  if ((Get-FileHash $exe).Hash.ToLowerInvariant() -cne $exeHash) { throw 'prestart_executable_changed' }
  if (!$p.Start()) { throw 'process_start_false' };$started=$true
  Write-NewJson 'intent.json' @{pid=$p.Id;head=$ExpectedHead;exe_sha256=$exeHash;utc=[DateTimeOffset]::UtcNow.ToString('o');wall_seconds=30;runtime_seconds=25;cleanup_seconds_reserved=5;unfiltered=$true;tree_unknown=$true}
  foreach ($name in @('stdout','stderr')) {
    $stream=if ($name -eq 'stdout') { $p.StandardOutput.BaseStream } else { $p.StandardError.BaseStream }
    $buffer=[byte[]]::new(8192);$file=[IO.File]::Open((Join-Path $outdir "$name.bin"),[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
    $channels+= [pscustomobject]@{Stream=$stream;Buffer=$buffer;File=$file;Task=$stream.ReadAsync($buffer,0,$buffer.Length);EOF=$false}
  }
  while ($true) {
    foreach ($c in $channels) {
      if (!$c.EOF -and $c.Task.IsCompleted) {
        $n=$c.Task.GetAwaiter().GetResult()
        if ($n -eq 0) { $c.EOF=$true } else {
          if ($bytes+$n -gt (16777216-65536)) { $stopReason='output_limit';break }
          $c.File.Write($c.Buffer,0,$n);$bytes+=$n;$c.Task=$c.Stream.ReadAsync($c.Buffer,0,$c.Buffer.Length)
        }
      }
    }
    if ($stopReason) { break }
    if ($p.HasExited -and @($channels | Where-Object { !$_.EOF }).Count -eq 0) { $parentReaped=$p.WaitForExit(0);break }
    if ($clock.Elapsed.TotalSeconds -ge 25) { $stopReason='runtime_or_pipe_drain_deadline';break }
    Start-Sleep -Milliseconds 10
  }
} catch { $errors.Add($_.Exception.GetType().FullName);$stopReason='owned_operation_failure' }
finally {
  if ($started) {
    try {
      if (!$p.HasExited) { $killRequested=$true;$p.Kill($true) }
      $remaining=[Math]::Max(0,[Math]::Min(5000,[int]((30-$clock.Elapsed.TotalSeconds)*1000)))
      $parentReaped=$p.WaitForExit($remaining)
      if ($parentReaped) {
        $exitSigned=[int]$p.ExitCode
        $exitHex=[BitConverter]::ToUInt32([BitConverter]::GetBytes($exitSigned),0).ToString('X8')
      }
    } catch { $errors.Add($_.Exception.GetType().FullName);$parentReaped=$false }
  }
  foreach ($c in $channels) { try { $c.Stream.Dispose();$c.File.Flush($true);$c.File.Dispose() } catch { $errors.Add($_.Exception.GetType().FullName) } }
  $postHash=$null
  try { $postHash=(Get-FileHash $exe).Hash.ToLowerInvariant();if ($postHash -cne $exeHash) { $stopReason='post_executable_changed' } } catch { $errors.Add($_.Exception.GetType().FullName);$stopReason='post_hash_unknown' }
  if ($clock.Elapsed.TotalSeconds -gt 30) { $stopReason='overall_deadline_exceeded' }
  $resultPublished=$false;$disposeCompleted=$false
  try {
    Write-NewJson 'result.json' @{head=$ExpectedHead;exe_sha256=$exeHash;post_exe_sha256=$postHash;started=$started;parent_exited_and_waited=$parentReaped;exit_signed=$exitSigned;exit_hex=$exitHex;elapsed_seconds=$clock.Elapsed.TotalSeconds;output_bytes=$bytes;stop_reason=$stopReason;kill_tree_requested=$killRequested;tree_unknown=$true;overall_cleanup_verified=$false;qualified=$false;original_ctest_failed=$true;errors=@($errors)}
    $resultPublished=$true
  } finally {
    try { $p.Dispose();$disposeCompleted=$true }
    finally {
      # Observe after the original result publication and Process.Dispose.
      # This receipt does not recursively audit its own synchronous publication.
      $afterPublication=$clock.Elapsed.TotalSeconds
      Write-NewJson 'after-publication-clock.json' @{head=$ExpectedHead;exe_sha256=$exeHash;result_published=$resultPublished;process_dispose_completed=$disposeCompleted;elapsed_after_result_and_dispose_seconds=$afterPublication;deadline_failure=($afterPublication -gt 30);sync_calls_nonpreemptible=$true;tree_unknown=$true;overall_cleanup_verified=$false;qualified=$false;original_ctest_failed=$true}
    }
  }
}
# This is diagnostic evidence, never a retry qualification or a healed original CTest.
exit 1
