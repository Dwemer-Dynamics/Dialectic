param()

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path

function Fail([string]$Message) {
    throw "Dialectic release-tree verification failed: $Message"
}

function Read-Signature([byte[]]$Bytes, [int]$Offset) {
    return [Text.Encoding]::ASCII.GetString($Bytes, $Offset, 4)
}

function Count-PluginRange([byte[]]$Bytes, [int]$Start, [int]$End, [int]$Depth) {
    $position = $Start
    $count = 0

    while ($position + 24 -le $End) {
        $signature = Read-Signature $Bytes $position
        if ($signature -eq "GRUP") {
            $size = [BitConverter]::ToUInt32($Bytes, $position + 4)
            if ($size -lt 24 -or $position + $size -gt $End) {
                Fail "invalid GRUP bounds at ESP offset $position"
            }
            # HEDR counts top-level groups, but not nested child groups.
            if ($Depth -eq 0) {
                $count += 1
            }
            $count += Count-PluginRange $Bytes ($position + 24) ($position + [int]$size) ($Depth + 1)
            $position += [int]$size
            continue
        }

        $size = [BitConverter]::ToUInt32($Bytes, $position + 4)
        if ($position + 24 + $size -gt $End) {
            Fail "invalid $signature record bounds at ESP offset $position"
        }
        # The TES4 file header itself is not included in HEDR's record count.
        if ($signature -ne 'TES4') {
            $count += 1
        }
        $position += 24 + [int]$size
    }

    if ($position -ne $End) {
        Fail "ESP parse stopped at $position instead of $End"
    }

    return $count
}

Push-Location $repoRoot
try {
    $trackedIndex = @(git ls-files)
    if ($LASTEXITCODE -ne 0 -or $trackedIndex.Count -eq 0) {
        Fail "unable to read tracked files"
    }
    $tracked = @($trackedIndex | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf })

    $forbiddenPatterns = @(
        '^Mod/Source/',
        '^Mod/Data/Dialectic/voice_samples/',
        '^Plugin/(?:out|build|\.vs)/',
        '\.(?:dll|pdb|obj|ilk|exe|lib|exp|log|tmp|bak|orig)$'
    )
    foreach ($pattern in $forbiddenPatterns) {
        $matches = @($tracked | Where-Object { $_ -match $pattern })
        if ($matches.Count -gt 0) {
            Fail "generated or obsolete files are tracked for '$pattern': $($matches -join ', ')"
        }
    }

    $runnerFiles = @(Get-ChildItem -LiteralPath 'Mod\Data\NVSE\Plugins\scripts' -File)
    if ($runnerFiles.Count -ne 1 -or $runnerFiles[0].Name -ne 'ln_DialecticBootstrap.txt') {
        Fail "exactly one load-time script runner must own bridge startup"
    }

    $searchFiles = @(Get-ChildItem -LiteralPath . -Recurse -File | Where-Object {
        $_.FullName -notmatch '[\\/](?:\.git|out|build)[\\/]' -and
        $_.Extension -in '.cpp', '.h', '.txt', '.gek', '.json', '.ini', '.md', '.ps1', '.xml'
    })
    $udfFiles = @(Get-ChildItem -LiteralPath 'Mod\Data\NVSE\user_defined_functions\Dialectic' -File)
    foreach ($udf in $udfFiles) {
        $references = @($searchFiles |
            Where-Object { $_.FullName -ne $udf.FullName } |
            Select-String -Pattern $udf.BaseName -SimpleMatch -ErrorAction SilentlyContinue)
        if ($references.Count -eq 0) {
            Fail "unreferenced NVSE UDF: $($udf.Name)"
        }
    }

    $bootstrap = Get-Content -Raw -LiteralPath $runnerFiles[0].FullName
    $bootstrapScripts = @([regex]::Matches($bootstrap, 'CompileScript\s+"Dialectic/([^"]+)"') |
        ForEach-Object { $_.Groups[1].Value })
    $duplicates = @($bootstrapScripts | Group-Object | Where-Object Count -gt 1)
    if ($duplicates.Count -gt 0) {
        Fail "bootstrap starts a script more than once: $(($duplicates.Name) -join ', ')"
    }

    $cmake = Get-Content -Raw -LiteralPath 'Plugin\CMakeLists.txt'
    foreach ($source in (Get-ChildItem -LiteralPath 'Plugin\src' -Filter '*.cpp')) {
        $relative = "src/$($source.Name)"
        if (-not $cmake.Contains($relative)) {
            Fail "C++ source is not included by CMake: $relative"
        }
    }

    $voiceRows = @(Import-Csv -LiteralPath 'Mod\Data\Dialectic\fallout_builtin_voices.csv')
    if ($voiceRows.Count -eq 0) {
        Fail "voice mapping CSV is empty"
    }

    $requiredVoiceColumns = @('voicetype', 'voicefile', 'transcript')
    $voiceColumns = @($voiceRows[0].PSObject.Properties.Name)
    foreach ($column in $requiredVoiceColumns) {
        if ($column -notin $voiceColumns) {
            Fail "voice mapping CSV is missing required column: $column"
        }
    }

    $listedVoiceTypes = @{}
    foreach ($row in $voiceRows) {
        $voiceType = ([string]$row.voicetype).Trim()
        $voiceFile = ([string]$row.voicefile).Trim().Replace('\', '/')
        if ([string]::IsNullOrWhiteSpace($voiceType) -or [string]::IsNullOrWhiteSpace($voiceFile)) {
            Fail "voice mapping rows require non-empty voicetype and voicefile values"
        }
        if ([IO.Path]::IsPathRooted($voiceFile) -or $voiceFile -match '(^|/)\.\.(/|$)') {
            Fail "voice mapping must use a relative game path: $voiceFile"
        }
        if ([IO.Path]::GetExtension($voiceFile).ToLowerInvariant() -notin '.ogg', '.wav', '.xwm', '.fuz') {
            Fail "voice mapping uses an unsupported audio extension: $voiceFile"
        }

        $voiceTypeKey = $voiceType.ToLowerInvariant()
        if ($listedVoiceTypes.ContainsKey($voiceTypeKey)) {
            Fail "voice mapping contains duplicate voicetype: $voiceType"
        }
        $listedVoiceTypes[$voiceTypeKey] = $true
    }

    foreach ($jsonFile in ($tracked | Where-Object { $_ -like '*.json' })) {
        try {
            Get-Content -Raw -LiteralPath $jsonFile | ConvertFrom-Json | Out-Null
        } catch {
            Fail "invalid JSON in ${jsonFile}: $($_.Exception.Message)"
        }
    }

    $espPath = Resolve-Path 'Mod\Data\Dialectic.esp'
    $espBytes = [IO.File]::ReadAllBytes($espPath)
    if ((Read-Signature $espBytes 0) -ne 'TES4' -or (Read-Signature $espBytes 24) -ne 'HEDR') {
        Fail "Dialectic.esp has an invalid TES4 header"
    }
    $asciiEsp = [Text.Encoding]::ASCII.GetString($espBytes)
    if ($asciiEsp.Contains('SCPT') -or $asciiEsp.Contains('DialecticBridgeScript')) {
        Fail "Dialectic.esp still contains legacy compiled scripts"
    }
    $parsedCount = Count-PluginRange $espBytes 0 $espBytes.Length 0
    $headerCount = [BitConverter]::ToUInt32($espBytes, 34)
    if ($headerCount -ne $parsedCount) {
        Fail "Dialectic.esp header count $headerCount does not match parsed count $parsedCount"
    }

    Write-Host 'Dialectic release-tree verification passed'
    Write-Host "Tracked files: $($tracked.Count)"
    Write-Host "Runtime UDFs: $($udfFiles.Count)"
    Write-Host "Voice mappings: $($voiceRows.Count)"
    Write-Host "ESP records/groups: $headerCount"
} finally {
    Pop-Location
}
