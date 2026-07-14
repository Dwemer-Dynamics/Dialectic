param(
    [string]$RepositoryRoot = (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)),
    [string]$OutputCsv = ''
)

$ErrorActionPreference = 'Stop'
$scanRoots = @(
    (Join-Path $RepositoryRoot 'Plugin\src'),
    (Join-Path $RepositoryRoot 'Mod\Data\NVSE\user_defined_functions\Dialectic')
)
$rootPath = (Resolve-Path -LiteralPath $RepositoryRoot).Path
$references = @{}

foreach ($file in Get-ChildItem -Path $scanRoots -Recurse -File) {
    $content = Get-Content -LiteralPath $file.FullName -Raw
    $relative = $file.FullName.Substring($rootPath.Length + 1)
    foreach ($match in [regex]::Matches($content, 'dialectic_[A-Za-z0-9_.-]+\.(?:tmp|txt)')) {
        $bridge = $match.Value.ToLowerInvariant()
        if (-not $references.ContainsKey($bridge)) {
            $references[$bridge] = [System.Collections.Generic.HashSet[string]]::new(
                [StringComparer]::OrdinalIgnoreCase)
        }
        [void]$references[$bridge].Add($relative)
    }
}

$rows = foreach ($entry in $references.GetEnumerator() | Sort-Object Name) {
    $owners = @($entry.Value | Sort-Object)
    $dllOwners = @($owners | Where-Object { $_ -like 'Plugin\src\*' })
    $scriptOwners = @($owners | Where-Object { $_ -like 'Mod\Data\NVSE\user_defined_functions\Dialectic\*' })
    [pscustomobject]@{
        Bridge = $entry.Name
        DllOwners = $dllOwners -join '; '
        ScriptOwners = $scriptOwners -join '; '
        StartupCleanupReference = [bool]($dllOwners -contains 'Plugin\src\main.cpp')
    }
}

if ($OutputCsv) {
    $rows | Export-Csv -LiteralPath $OutputCsv -NoTypeInformation -Encoding UTF8
    Write-Host "Wrote $($rows.Count) bridge rows to $OutputCsv"
} else {
    $rows
}
