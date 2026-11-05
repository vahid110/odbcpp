# Read the one source chosen by FindLogging, including FetchContent overrides.
# These hashes attest configure-time inputs only, not a cached binary's origin.
function Read-LoggingPackageInputs([string]$BuildDirectory) {
    $metadataPath = Join-Path $BuildDirectory 'odbcpp-logging-package-inputs.json'
    if (!(Test-Path -LiteralPath $metadataPath -PathType Leaf)) {
        throw 'Missing logging package metadata; reconfigure the exact clean source before packaging'
    }
    $metadata = Get-Content -LiteralPath $metadataPath -Raw | ConvertFrom-Json
    if ($metadata.schemaVersion -ne 1 -or $metadata.configurationEvidenceOnly -cne $true) {
        throw 'Invalid logging package metadata schema or evidence claim'
    }
    $source = [string]$metadata.sourceDirectory
    if (![IO.Path]::IsPathFullyQualified($source) -or !(Test-Path -LiteralPath $source -PathType Container)) {
        throw 'Missing or invalid configured logging source directory'
    }
    $members = @('LICENSE', 'include/spdlog/version.h',
        'include/spdlog/fmt/bundled/fmt.license.rst', 'include/spdlog/fmt/bundled/base.h')
    $recorded = @($metadata.files.PSObject.Properties.Name)
    if ($recorded.Count -ne $members.Count -or @(Compare-Object $members $recorded).Count -ne 0) {
        throw 'Invalid logging package input inventory'
    }
    foreach ($member in $members) {
        $path = Join-Path $source $member
        $expected = [string]$metadata.files.$member
        if ($expected -cnotmatch '^[0-9a-f]{64}$' -or
            !(Test-Path -LiteralPath $path -PathType Leaf) -or
            (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ine $expected) {
            throw "Missing or changed logging package input: $member; reconfigure and rebuild before packaging"
        }
    }
    function Read-VersionMacro([string]$Path, [string]$Name) {
        $matches = [regex]::Matches((Get-Content -LiteralPath $Path -Raw),
            '(?m)^#define[ \t]+' + [regex]::Escape($Name) + '[ \t]+(\d+)[ \t]*\r?$')
        if ($matches.Count -ne 1) { throw "Missing or ambiguous logging version macro: $Name" }
        return [int]$matches[0].Groups[1].Value
    }
    $versionHeader = Join-Path $source 'include/spdlog/version.h'
    $spdlogVersion = (@('MAJOR', 'MINOR', 'PATCH') | ForEach-Object {
        Read-VersionMacro $versionHeader "SPDLOG_VER_$_"
    }) -join '.'
    $fmtNumber = Read-VersionMacro (Join-Path $source 'include/spdlog/fmt/bundled/base.h') 'FMT_VERSION'
    $fmtVersion = '{0}.{1}.{2}' -f [math]::Floor($fmtNumber / 10000),
        [math]::Floor(($fmtNumber % 10000) / 100), ($fmtNumber % 100)
    return @{
        sourceDirectory = $source
        versions = @{ spdlog = $spdlogVersion; fmt = $fmtVersion }
        # Only this path-free record is included in package provenance.
        evidence = @{ configurationEvidenceOnly = $true; files = $metadata.files }
    }
}
