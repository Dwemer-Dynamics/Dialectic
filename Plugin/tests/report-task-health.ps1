param(
    [string]$LogPath = 'C:\Program Files (x86)\Steam\steamapps\common\Fallout New Vegas\dialectic.log',
    [switch]$RequireDialogueExercise
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $LogPath)) {
    Write-Error "Dialectic runtime log not found: $LogPath"
    exit 1
}

$lines = Get-Content -LiteralPath $LogPath
$startup = @($lines | Select-String -Pattern 'TaskManager: initialized workers=8 .*lanes=interactive,audio,gameplay,compute,background')
$runtime = @($lines | Select-String -Pattern '\[NATIVE_RUNTIME\].*tasks\(')
$types = @($lines | Select-String -Pattern '\[TASK_HEALTH\] types ')
$voice = @($lines | Select-String -Pattern '\[TASK_HEALTH\] voice ')
$failures = @($lines | Select-String -Pattern 'TaskManager: queue full|TaskManager: .*reached (queue )?deadline|TaskManager: worker=.* threw')

$problems = [System.Collections.Generic.List[string]]::new()
if ($startup.Count -eq 0) { $problems.Add('Final five-lane TaskManager startup marker is missing.') }
if ($runtime.Count -eq 0) { $problems.Add('No periodic native runtime task summary was emitted.') }
if ($types.Count -eq 0) { $problems.Add('No per-type task health summary was emitted.') }
if ($voice.Count -eq 0) { $problems.Add('No voice-device service health summary was emitted.') }
if ($failures.Count -gt 0) { $problems.Add("Task failures/saturation/deadlines were logged: $($failures.Count)") }

if ($RequireDialogueExercise) {
    if (-not ($types | Where-Object { $_.Line -match 'http:HTTPStream(?:Event)?\[' })) {
        $problems.Add('No player/dialogue HTTP stream task was observed.')
    }
    if (-not ($types | Where-Object { $_.Line -match 'audio_prepare\[' })) {
        $problems.Add('No managed TTS audio preparation task was observed.')
    }
}

Write-Host "Dialectic Task Health"
Write-Host "  Log: $LogPath"
Write-Host "  Startup markers: $($startup.Count)"
Write-Host "  Runtime summaries: $($runtime.Count)"
Write-Host "  Type summaries: $($types.Count)"
Write-Host "  Voice summaries: $($voice.Count)"
Write-Host "  Failure markers: $($failures.Count)"
if ($runtime.Count -gt 0) { Write-Host "  Latest runtime: $($runtime[-1].Line)" }
if ($types.Count -gt 0) { Write-Host "  Latest types: $($types[-1].Line)" }
if ($voice.Count -gt 0) { Write-Host "  Latest voice: $($voice[-1].Line)" }

if ($problems.Count -gt 0) {
    foreach ($problem in $problems) { Write-Error $problem }
    exit 1
}

Write-Host 'Task-manager runtime health passed.'
