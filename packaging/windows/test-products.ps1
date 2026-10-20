# Inspect real MSI databases read-only. No installation, registry or database access.
param(
    [Parameter(Mandatory=$true)][string]$PostgreSqlPackage,
    [Parameter(Mandatory=$true)][string]$RedshiftPackage,
    [string]$PostgreSqlBaseline = ''
)
$ErrorActionPreference = 'Stop'
function Assert([bool]$Condition, [string]$Message) { if (!$Condition) { throw $Message } }
function Rows($Database, [string]$Query, [string[]]$Columns) {
    $view = $Database.OpenView($Query)
    try {
        $view.Execute()
        while ($null -ne ($record = $view.Fetch())) {
            try {
                $row = [ordered]@{}
                for ($i = 0; $i -lt $Columns.Count; $i++) { $row[$Columns[$i]] = $record.StringData($i + 1) }
                [pscustomobject]$row
            } finally { [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($record) }
        }
    } finally { $view.Close(); [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($view) }
}
function Read-Package($Installer, [string]$Path) {
    $path = (Resolve-Path -LiteralPath $Path).Path
    $database = $Installer.OpenDatabase($path, 0)
    try {
        $properties = @{}
        foreach ($row in Rows $database 'SELECT `Property`, `Value` FROM `Property`' @('name','value')) { $properties[$row.name] = $row.value }
        return @{
            path = $path; properties = $properties
            components = @(Rows $database 'SELECT `Component`, `ComponentId`, `Directory_`, `Attributes`, `KeyPath` FROM `Component`' @('name','guid','directory','attributes','keyPath'))
            directories = @(Rows $database 'SELECT `Directory`, `Directory_Parent`, `DefaultDir` FROM `Directory`' @('name','parent','target'))
            registry = @(Rows $database 'SELECT `Registry`, `Root`, `Key`, `Name`, `Value`, `Component_` FROM `Registry`' @('id','root','key','name','value','component'))
            searches = @(Rows $database 'SELECT `Signature_`, `Root`, `Key`, `Name`, `Type` FROM `RegLocator`' @('id','root','key','name','type'))
            files = @(Rows $database 'SELECT `File`, `Component_`, `FileName`, `FileSize` FROM `File`' @('id','component','name','bytes'))
        }
    } finally { [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($database) }
}
function Check-Product($Package, [string]$Name, [string]$Database, [string]$Upgrade, [string]$Registration) {
    Assert ($Package.properties.ProductName -ceq "ODBCPP $Name Beta (x64)") 'Incorrect package product name'
    Assert ($Package.properties.UpgradeCode -ieq "{$Upgrade}") 'Incorrect persistent upgrade identity'
    $folder = @($Package.directories | Where-Object name -eq 'INSTALLFOLDER')
    Assert ($folder.Count -eq 1 -and ($folder[0].target -split '\|')[-1] -ceq $Name -and $folder[0].parent -ceq 'VENDORFOLDER') 'Incorrect product install directory'
    $registrationRow = @($Package.components | Where-Object name -eq 'Registration')
    Assert ($registrationRow.Count -eq 1 -and $registrationRow[0].guid -ieq "{$Registration}") 'Incorrect persistent registration component identity'
    foreach ($row in $Package.components) { Assert (([int]$row.attributes -band 256) -eq 256) 'A package component is not x64' }
    $expected = @{
        Driver = @(('SOFTWARE\ODBC\ODBCINST.INI\ODBCPP '+$Name), '[INSTALLFOLDER]odbcpp.dll')
        Setup = @(('SOFTWARE\ODBC\ODBCINST.INI\ODBCPP '+$Name), '[INSTALLFOLDER]odbcpp_setup.dll')
        OwnedDriver = @(('SOFTWARE\ODBCPP\'+$Name), '[INSTALLFOLDER]odbcpp.dll')
        OwnedSetup = @(('SOFTWARE\ODBCPP\'+$Name), '[INSTALLFOLDER]odbcpp_setup.dll')
        Version = @(('SOFTWARE\ODBCPP\'+$Name), $Package.properties.ProductVersion)
    }
    Assert ($Package.registry.Count -eq 6 -and $Package.searches.Count -eq 4) 'Unexpected registration/search inventory'
    foreach ($entry in $expected.GetEnumerator()) {
        $rows = @($Package.registry | Where-Object name -ceq $entry.Key)
        Assert ($rows.Count -eq 1 -and $rows[0].root -eq '2' -and $rows[0].key -ceq $entry.Value[0] -and $rows[0].value -ceq $entry.Value[1] -and $rows[0].component -ceq 'Registration') 'Incorrect owned registration value'
    }
    $mapping = @($Package.registry | Where-Object name -ceq "ODBCPP $Name")
    Assert ($mapping.Count -eq 1 -and $mapping[0].key -ceq 'SOFTWARE\ODBC\ODBCINST.INI\ODBC Drivers' -and $mapping[0].root -eq '2' -and $mapping[0].value -ceq 'Installed') 'Incorrect installed-driver mapping'
    foreach ($row in $Package.searches) {
        $owned = $row.name -in @('OwnedDriver','OwnedSetup')
        $key = if ($owned) { 'SOFTWARE\ODBCPP\'+$Name } else { 'SOFTWARE\ODBC\ODBCINST.INI\ODBCPP '+$Name }
        Assert ($row.root -eq '2' -and $row.key -ceq $key -and ([int]$row.type -band 16) -eq 16) 'Incorrect product collision lookup'
    }
    $inventoryPath = Join-Path (Split-Path $Package.path -Parent) ('payload-'+$Package.properties.ProductVersion+'/inventory.json')
    $inventory = Get-Content -LiteralPath $inventoryPath -Raw | ConvertFrom-Json
    Assert ($inventory.targetDatabase -ceq $Database -and $inventory.driverName -ceq "ODBCPP $Name") 'Inventory product differs from MSI'
    foreach ($entry in $inventory.files) {
        $file = Join-Path (Split-Path $inventoryPath -Parent) $entry.file
        Assert ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ceq $entry.sha256) 'Payload hash differs from inventory'
    }
    $staged = @(Get-ChildItem -LiteralPath (Split-Path $inventoryPath -Parent) -File | Select-Object -ExpandProperty Name | Sort-Object)
    $packaged = @($Package.files | ForEach-Object { ($_.name -split '\|')[-1] } | Sort-Object)
    Assert (@(Compare-Object $staged $packaged).Count -eq 0) 'MSI file inventory differs from staged payload'
}
$installer = New-Object -ComObject WindowsInstaller.Installer
try {
    $pg = Read-Package $installer $PostgreSqlPackage
    $rs = Read-Package $installer $RedshiftPackage
    Check-Product $pg 'PostgreSQL' 'POSTGRESQL' 'BC23230D-B189-4CF8-81D2-11ED5F55FC6F' 'A40E29D2-6905-498C-959B-2DA3138061CF'
    Check-Product $rs 'Redshift' 'REDSHIFT' '7CF4F823-59A8-481B-B940-CD9AEF80A3EA' 'D0D5CB96-848E-4C0B-8422-1F64598EF0E2'
    Assert ($pg.properties.ProductCode -ine $rs.properties.ProductCode -and $pg.properties.UpgradeCode -ine $rs.properties.UpgradeCode) 'Products share installer identity'
    $overlap = @($pg.components | Where-Object { $_.guid -in $rs.components.guid })
    Assert ($overlap.Count -eq 0) 'Products share component GUIDs'
    if ($PostgreSqlBaseline) {
        $old = Read-Package $installer $PostgreSqlBaseline
        Assert ($old.properties.UpgradeCode -ceq $pg.properties.UpgradeCode) 'PostgreSQL upgrade lineage changed'
        foreach ($table in @('components','directories','registry','searches')) {
            $before = @($old[$table] | Sort-Object { $_ | ConvertTo-Json -Compress }) | ConvertTo-Json -Compress -Depth 5
            $after = @($pg[$table] | Sort-Object { $_ | ConvertTo-Json -Compress }) | ConvertTo-Json -Compress -Depth 5
            Assert ($before -ceq $after) "PostgreSQL $table changed against the original recipe"
        }
    }
    Write-Host 'MSI product identities, isolation, registry, x64 components and payload inventories passed.'
} finally { [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($installer) }
