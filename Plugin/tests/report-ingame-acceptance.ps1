param(
    [string]$PluginLog = 'C:\Program Files (x86)\Steam\steamapps\common\Fallout New Vegas\dialectic.log',
    [string]$DeployedDll = 'C:\Modlists\Fallout TTW\mods\Dialectic_dev\NVSE\Plugins\dialectic.dll',
    [switch]$Strict
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $PluginLog)) {
    throw "Dialectic plugin log not found: $PluginLog"
}
if (-not (Test-Path -LiteralPath $DeployedDll)) {
    throw "Deployed Dialectic DLL not found: $DeployedDll"
}

$logFile = Get-Item -LiteralPath $PluginLog
$dllFile = Get-Item -LiteralPath $DeployedDll
$isFresh = $logFile.LastWriteTimeUtc -gt $dllFile.LastWriteTimeUtc

Write-Host "Dialectic in-game acceptance report"
Write-Host "DLL: $($dllFile.FullName) [$($dllFile.LastWriteTime.ToString('s'))]"
Write-Host "Log: $($logFile.FullName) [$($logFile.LastWriteTime.ToString('s'))]"

if (-not $isFresh) {
    Write-Warning 'STALE: Fallout has not produced a Dialectic log newer than the deployed DLL.'
    exit 2
}

$content = Get-Content -LiteralPath $PluginLog -Raw
$checks = @(
    [pscustomobject]@{ Name = 'Plugin loaded'; Pattern = 'Plugin loaded successfully' },
    [pscustomobject]@{ Name = 'Native frame pump'; Pattern = '\[NATIVE_RUNTIME\] xNVSE main-game-loop pump authoritative' },
    [pscustomobject]@{ Name = 'Actor/reference discovery'; Pattern = '\[NATIVE_RUNTIME\].*actors=[1-9][0-9]*.*refs=[1-9][0-9]*' },
    [pscustomobject]@{ Name = 'Auto activation'; Pattern = 'ActivationManager: auto activating' },
    [pscustomobject]@{ Name = 'AI dialogue queued'; Pattern = 'SpeakManager: QueueDialogue received' },
    [pscustomobject]@{ Name = 'TTS audio playback'; Pattern = 'SpeakManager: Audio playback started' },
    [pscustomobject]@{ Name = 'Passive subtitle path'; Pattern = 'SpeakManager: (Native passive subtitle updated|Passive subtitle using script fallback)' },
    [pscustomobject]@{ Name = 'Lip-sync path'; Pattern = 'SpeakManager: Lip sync started' },
    [pscustomobject]@{ Name = 'Rechat launch'; Pattern = 'SpeakManager: Launching rechat' },
    [pscustomobject]@{ Name = 'Rechat request'; Pattern = 'SpeakManager: Rechat request' },
    [pscustomobject]@{ Name = 'Action execution'; Pattern = '(\[NATIVE_ACTION\].*(completed|started|opened|launched)|ActionManager: Sending funcret)' },
    [pscustomobject]@{ Name = 'Menu playback pause'; Pattern = 'SpeakManager: Paused AI dialogue because menu/Pip-Boy is open' },
    [pscustomobject]@{ Name = 'Menu playback resume'; Pattern = 'SpeakManager: Resumed AI dialogue after menu/Pip-Boy closed' },
    [pscustomobject]@{ Name = 'Auto Greeting dispatch'; Pattern = 'AutoGreetingFNV: dispatching' },
    [pscustomobject]@{ Name = 'Cell transition'; Pattern = '\[NATIVE_RUNTIME\] cell changed' },
    [pscustomobject]@{ Name = 'TTW worldspace transition'; Pattern = '\[NATIVE_RUNTIME\] worldspace changed' }
)

$results = foreach ($check in $checks) {
    [pscustomobject]@{
        Check = $check.Name
        Status = if ($content -match $check.Pattern) { 'Observed' } else { 'Not observed' }
    }
}

$results | Format-Table -AutoSize

Write-Host ''
Write-Host 'Manual visual/audio checks still required:'
Write-Host '- Relationship context and updates are correct in the resulting NPC response.'
Write-Host '- Auto Greeting occurs only after a valid absence and cancels on interruption.'
Write-Host '- Nearby actors/items match the visible scene across exterior cell boundaries.'
Write-Host '- Subtitle text, lip-sync timing, facing, TTS voice, and 3D audio are perceptually correct.'
Write-Host '- Actions produce the intended in-game state, not only a successful transport log.'
Write-Host '- Dialogue stays paused for the entire time a pause menu or Pip-Boy is open.'
Write-Host '- Rechat stops after a cell change and TTW travel does not retain stale actors.'

$missing = @($results | Where-Object { $_.Status -ne 'Observed' })
if ($Strict -and $missing.Count -gt 0) {
    exit 1
}

Write-Host ''
Write-Host "Observed $($results.Count - $missing.Count)/$($results.Count) runtime markers."
