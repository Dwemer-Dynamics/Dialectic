param(
    [string]$PluginRoot = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = 'Stop'
$failures = [System.Collections.Generic.List[string]]::new()

function Require-Path([string]$Path, [string]$Label) {
    if (-not (Test-Path -LiteralPath $Path)) {
        $failures.Add("Missing $Label`: $Path")
    }
}

function Require-Text([string]$Path, [string]$Pattern, [string]$Label) {
    if (-not (Select-String -LiteralPath $Path -Pattern $Pattern -Quiet)) {
        $failures.Add("Missing $Label in $Path")
    }
}

function Reject-Text([string[]]$Paths, [string]$Pattern, [string]$Label) {
    $matches = Select-String -Path $Paths -Pattern $Pattern
    if ($matches) {
        $locations = ($matches | ForEach-Object { "$($_.Path):$($_.LineNumber)" }) -join ', '
        $failures.Add("Found $Label at $locations")
    }
}

$sourceRoot = Join-Path $PluginRoot 'src'
$cmakeFile = Join-Path $PluginRoot 'CMakeLists.txt'
$repoRoot = Split-Path -Parent $PluginRoot
$mcmFile = Join-Path $repoRoot 'Mod\Data\MCM\Dialectic.json'
$iniFile = Join-Path $repoRoot 'Mod\Data\NVSE\Plugins\dialectic.ini'
$scriptRoot = Join-Path $repoRoot 'Mod\Data\NVSE\user_defined_functions\Dialectic'
$bootstrapFile = Join-Path $repoRoot 'Mod\Data\NVSE\Plugins\scripts\ln_DialecticBootstrap.txt'

Require-Path (Join-Path $PluginRoot 'vendor\xnvse-sdk\nvse\PluginAPI.h') 'pinned xNVSE SDK'
Require-Path (Join-Path $sourceRoot 'FNVRuntime.cpp') 'native runtime'
Require-Path (Join-Path $sourceRoot 'GameThreadDispatcher.cpp') 'game-thread dispatcher'
Require-Path (Join-Path $sourceRoot 'RuntimeSnapshot.cpp') 'runtime snapshot'
Require-Path (Join-Path $sourceRoot 'TaskManager.cpp') 'central task manager'
Require-Path (Join-Path $sourceRoot 'NativeComparisonTelemetry.cpp') 'native comparison telemetry'
Require-Path (Join-Path $sourceRoot 'SpatialSnapshotManagerFNV.cpp') 'incremental spatial snapshot manager'
Require-Path (Join-Path $sourceRoot 'DialecticInitialization.cpp') 'Dialectic initialization coordinator'
Require-Path (Join-Path $sourceRoot 'XNVSEAdapter.cpp') 'xNVSE adapter'
Require-Path (Join-Path $repoRoot 'docs\NATIVE_RUNTIME_MIGRATION.md') 'migration record'
Require-Path (Join-Path $repoRoot 'docs\NATIVE_RUNTIME_VALIDATION.md') 'runtime validation matrix'
Require-Path (Join-Path $PSScriptRoot 'report-runtime-bridges.ps1') 'exact bridge ownership reporter'
Require-Path (Join-Path $PSScriptRoot 'report-task-health.ps1') 'task-manager runtime health reporter'
Require-Path (Join-Path $PSScriptRoot 'report-ingame-acceptance.ps1') 'in-game acceptance reporter'

if (Test-Path -LiteralPath (Join-Path $sourceRoot 'NVSEInterfaces.h')) {
    $failures.Add('Handwritten NVSEInterfaces.h must not be restored')
}

$cppFiles = Get-ChildItem -Path $sourceRoot -File | Where-Object { $_.Extension -in '.cpp', '.h' }
$cppPaths = @($cppFiles.FullName)
Reject-Text $cppPaths '\.detach\s*\(' 'detached runtime worker'
Reject-Text @((Join-Path $sourceRoot 'GameLoop.cpp'), (Join-Path $sourceRoot 'HTTPManager.cpp')) 'std::thread' 'legacy subsystem thread'
$managedThreadOwners = @('TaskManager.cpp', 'VoiceRecorder.cpp')
$unexpectedThreads = $cppFiles |
    Where-Object { $managedThreadOwners -notcontains $_.Name } |
    Select-String -Pattern 'std::thread\s+[A-Za-z_]|std::thread\s*\('
if ($unexpectedThreads) {
    $locations = ($unexpectedThreads | ForEach-Object { "$($_.Path):$($_.LineNumber)" }) -join ', '
    $failures.Add("Unmanaged subsystem thread escaped task/device ownership at $locations")
}
Reject-Text $cppPaths 'NVSEInterfaces\.h' 'handwritten NVSE ABI include'
Reject-Text @((Join-Path $sourceRoot 'XNVSEAdapter.cpp')) 'ActorProcessManager|kActorProcessManagerAddress' 'unsafe ActorProcessManager actor traversal'
Reject-Text @((Join-Path $sourceRoot 'AgentManager.cpp')) 'SendJsonBlocking|SendPromptCriticalMetadataBlocking' 'blocking prompt metadata network path'
Reject-Text $cppPaths 'AIAgent_' 'legacy AIAgent export or bridge'
Reject-Text $cppPaths 'aiagent_textinput\.tmp' 'legacy text-input bridge'
Reject-Text $cppPaths 'rolecommand\|' 'delimiter rolecommand parser'
Reject-Text $cppPaths 'RegisterFrameCallback|RegisterHooks|IsGamePaused' 'retired no-op plugin API'
Require-Text (Join-Path $sourceRoot 'ActionManager.cpp') 'ExtractJsonStringArrayValue\(lineObject, "command_args"\)' 'structured action arguments'

$sdkIncludeOwners = @('main.cpp', 'XNVSEAdapter.cpp')
$unexpectedSdkIncludes = $cppFiles |
    Where-Object { $sdkIncludeOwners -notcontains $_.Name } |
    Select-String -Pattern '#include\s+["<]nvse/'
if ($unexpectedSdkIncludes) {
    $locations = ($unexpectedSdkIncludes | ForEach-Object { "$($_.Path):$($_.LineNumber)" }) -join ', '
    $failures.Add("Pinned SDK include escaped the adapter boundary at $locations")
}

$rawEngineOwners = @('XNVSEAdapter.cpp', 'XNVSEAdapter.h', 'FNVRuntime.cpp', 'FNVRuntime.h')
$unexpectedRawAccess = $cppFiles |
    Where-Object { $rawEngineOwners -notcontains $_.Name } |
    Select-String -Pattern '\b(PlayerCharacter|TESObjectREFR|ActorProcessManager|InterfaceManager|TESObjectCELL|TESWorldSpace)\b'
if ($unexpectedRawAccess) {
    $locations = ($unexpectedRawAccess | ForEach-Object { "$($_.Path):$($_.LineNumber)" }) -join ', '
    $failures.Add("Raw Fallout object type escaped the runtime adapter at $locations")
}

Require-Text $cmakeFile 'vendor/xnvse-sdk' 'vendored xNVSE include path'
Require-Text $cmakeFile 'src/FNVRuntime\.cpp' 'FNVRuntime build source'
Require-Text $cmakeFile 'src/TaskManager\.cpp' 'TaskManager build source'
Require-Text $cmakeFile 'src/SpatialSnapshotManagerFNV\.cpp' 'spatial snapshot manager build source'
Require-Text $cmakeFile 'src/DialecticInitialization\.cpp' 'Dialectic initialization coordinator build source'
Require-Text (Join-Path $sourceRoot 'FNVRuntime.cpp') 'xNVSE main-game-loop pump authoritative' 'native frame-pump marker'
Require-Text (Join-Path $sourceRoot 'GameLoop.cpp') 'ResponseQueueFNV::DispatchPending' 'game-thread response dispatch'
Require-Text (Join-Path $sourceRoot 'ResponseQueueFNV.cpp') 'RuntimeGeneration::IsCurrent\(item\.runtimeGeneration\)' 'runtime-generation response gate'
Require-Text (Join-Path $sourceRoot 'SpeakManager.cpp') 'runtime generation changed before playback' 'runtime-generation playback gate'
Require-Text (Join-Path $sourceRoot 'ActionManager.cpp') 'Dropped stale action' 'runtime-generation action gate'
Require-Text (Join-Path $sourceRoot 'TaskManager.cpp') 'CancelByActor' 'actor-scoped task cancellation'
Require-Text (Join-Path $sourceRoot 'TaskManager.cpp') 'CancelByTurn' 'turn-scoped task cancellation'
Require-Text (Join-Path $sourceRoot 'TaskManager.cpp') 'pending task=.*reached queue deadline' 'queue-aware task deadlines'
Require-Text (Join-Path $sourceRoot 'SpeakManager.cpp') 'TaskManager::Submit' 'managed TTS preparation'
Reject-Text @((Join-Path $sourceRoot 'SpeakManager.cpp')) 'DialecticServer/TTS\.php' 'obsolete unmanaged TTS endpoint'
Require-Text (Join-Path $sourceRoot 'VoiceRecorder.cpp') 'VoiceRecorder: device workers shut down' 'joined recording-device shutdown'
Require-Text (Join-Path $sourceRoot 'GameThreadDispatcher.cpp') 'kMinimumCriticalReserve' 'reserved task-completion dispatcher capacity'
Require-Text (Join-Path $sourceRoot 'TaskManager.cpp') 'EnqueueCritical\("task_completion"' 'critical game-thread task completion'
Require-Text (Join-Path $sourceRoot 'WorldDataSyncFNV.cpp') 'CollectLocationsFromPlugins\(plugins, seenFormIds, token\)' 'cancellable world-data scan'
Require-Text (Join-Path $sourceRoot 'Logger.cpp') 'std::mutex s_logMutex' 'thread-safe runtime logger'
Require-Text (Join-Path $sourceRoot 'TargetManager.cpp') 'RuntimeSnapshot::IsActorInScene' 'native target scene gate'
Require-Text (Join-Path $sourceRoot 'ActionManager.cpp') 'RuntimeSnapshot::IsActorInScene' 'native action scene gate'
Require-Text (Join-Path $sourceRoot 'NPCDetector.cpp') 'RuntimeSnapshot::IsActorInScene' 'native autoactivation scene gate'
Require-Text (Join-Path $sourceRoot 'ActorPositionResolverFNV.cpp') 'worldspaceFormId != position.worldspaceFormId' 'TTW worldspace boundary'
Require-Text (Join-Path $sourceRoot 'FNVRuntime.cpp') '\[NATIVE_COMPARE\] summary' 'comparison health summary'
Require-Text (Join-Path $sourceRoot 'FNVRuntime.cpp') '\[NATIVE_RUNTIME\] cell changed' 'cell transition telemetry'
Require-Text (Join-Path $sourceRoot 'FNVRuntime.cpp') '\[NATIVE_RUNTIME\] worldspace changed' 'TTW worldspace transition telemetry'
Require-Text (Join-Path $sourceRoot 'XNVSEAdapter.cpp') 'CaptureNativeNavScene' 'native current-cell navmesh capture'
Require-Text (Join-Path $sourceRoot 'XNVSEAdapter.cpp') 'GetSceneCells' 'attached exterior scene-cell discovery'
Require-Text (Join-Path $sourceRoot 'XNVSEAdapter.cpp') 'cell->worldSpace != playerCell->worldSpace' 'same-worldspace exterior boundary'
Require-Text (Join-Path $sourceRoot 'XNVSEAdapter.cpp') 'kCellLoadState_Attached' 'attached-cell load-state gate'
Require-Text (Join-Path $sourceRoot 'XNVSEAdapter.cpp') 'for \(TESObjectCELL\* sceneCell : GetSceneCells\(player->parentCell\)\)' 'shared actor and reference scene scan'
Require-Text (Join-Path $sourceRoot 'SpatialPathProviderFNV.cpp') 'BuildGraph' 'worker-built native navmesh graph'
Require-Text (Join-Path $sourceRoot 'SpatialPathProviderFNV.cpp') 'EvaluateNativeGraph' 'native path-distance authority'
Require-Text (Join-Path $sourceRoot 'SpatialPathProviderFNV.cpp') 'TaskManager::Enqueue\("spatial_navgraph"' 'bounded worker graph build'
Require-Text (Join-Path $sourceRoot 'SpatialSnapshotManagerFNV.cpp') 'kMaxCandidates = 32' 'bounded incremental spatial roster'
Require-Text (Join-Path $sourceRoot 'GameLoop.cpp') 'SpatialSnapshotManagerFNV::Update\(1\)' 'one-actor spatial prewarm budget'

# Combat dialogue must be a runtime policy, not an MCM-only setting.
Require-Text $mcmFile '"title": "Allow Combat Dialogue"' 'combat dialogue MCM option'
Require-Text $mcmFile '"configINI": "Behavior:EnableCombatDialogue"' 'combat dialogue MCM binding'
Require-Text (Join-Path $sourceRoot 'Config.cpp') 'key == "EnableCombatDialogue"' 'combat dialogue INI loader'
Require-Text (Join-Path $sourceRoot 'GameLoop.cpp') 'Config::enableCombatDialogue' 'combat dialogue configuration consumer'
Require-Text (Join-Path $sourceRoot 'GameLoop.cpp') 'EnforceCombatDialogueGate\(combatTargetFormId, "chatbox", true\)' 'chatbox combat gate'
Require-Text (Join-Path $sourceRoot 'GameLoop.cpp') 'EnforceCombatDialogueGate\(target\.formId, "conversation_start", true\)' 'conversation-start combat gate'
Require-Text (Join-Path $sourceRoot 'GameLoop.cpp') 'EnforceCombatDialogueGate\(g_conversationPartnerFormId, "player_message", true\)' 'typed-input combat gate'
Require-Text (Join-Path $sourceRoot 'GameLoop.cpp') 'EnforceCombatDialogueGate\(g_conversationPartnerFormId, "voice_input", !openMicTriggered\)' 'voice-input combat gate'
Require-Text (Join-Path $sourceRoot 'SpeakManager.cpp') 'IsCombatDialogueAllowed\(speakerFormId\)' 'rechat speaker combat gate'
Require-Text (Join-Path $sourceRoot 'SpeakManager.cpp') 'IsCombatDialogueAllowed\(targetFormId\)' 'rechat target combat gate'

# Dialectic initialization must share one action between the MCM and one-time prompt.
Require-Text $mcmFile '"title": "Initialize Dialectic"' 'combined Dialectic initialization button'
Require-Text $mcmFile '"configINI": "Tools:InitializeDialectic"' 'combined initialization MCM binding'
Require-Text $mcmFile '"value": "Dialectic/InitializeDialectic\.gek"' 'combined initialization MCM callback'
Reject-Text @($mcmFile) '"title": "Send Faction and Location Info"|"title": "Send All Voice Samples"' 'separate initialization buttons'
Require-Path (Join-Path $scriptRoot 'InitializeDialectic.gek') 'combined initialization MCM callback'
Require-Path (Join-Path $scriptRoot 'RunDialecticInitialization.gek') 'shared Dialectic initializer'
Require-Path (Join-Path $scriptRoot 'InitializationPromptTick.txt') 'first-run initialization prompt'
Require-Path (Join-Path $scriptRoot 'InitializationPromptSelect.gek') 'initialization prompt callback'
Require-Text (Join-Path $scriptRoot 'RunDialecticInitialization.gek') 'DialecticInitialize' 'coordinated initialization action'
Require-Text (Join-Path $sourceRoot 'main.cpp') 'kCommandInfo_DialecticInitialize' 'coordinated initialization command registration'
$mainSource = Get-Content -LiteralPath (Join-Path $sourceRoot 'main.cpp') -Raw
$initializeOpcodeIndex = $mainSource.IndexOf('&kCommandInfo_DialecticInitialize')
$previousLastOpcodeIndex = $mainSource.IndexOf('&kCommandInfo_DialecticHandleHotkeyUp')
if ($initializeOpcodeIndex -lt 0 -or $initializeOpcodeIndex -lt $previousLastOpcodeIndex) {
    $failures.Add('DialecticInitialize must remain appended after the legacy command ABI')
}
Require-Text (Join-Path $sourceRoot 'DialecticInitialization.cpp') 'Voices synced\.' 'voice completion notice'
Require-Text (Join-Path $sourceRoot 'WorldDataSyncFNV.cpp') 'Factions synced\.' 'faction completion notice'
Require-Text (Join-Path $sourceRoot 'WorldDataSyncFNV.cpp') 'Locations synced\.' 'location completion notice'
Require-Text (Join-Path $sourceRoot 'DialecticInitialization.cpp') 'Dialectic initialized\.' 'final initialization notice'
Require-Text (Join-Path $sourceRoot 'IngameNotifier.cpp') 'level == Level::Success \? 0U' 'happy Pip-Boy icon for successful initialization notices'
Reject-Text @(
    (Join-Path $scriptRoot 'RunDialecticInitialization.gek'),
    (Join-Path $scriptRoot 'SendVoiceSamples.gek'),
    (Join-Path $scriptRoot 'VoiceSampleUploadTick.txt'),
    (Join-Path $scriptRoot 'WorldDataSyncTick.txt'),
    (Join-Path $sourceRoot 'VoiceSampleBatchUploadFNV.cpp'),
    (Join-Path $sourceRoot 'WorldDataSyncFNV.cpp')
) 'Initialization started|Uploading Fallout voice samples|Sending Fallout faction|Uploading .*factions|Uploading .*locations|Scanning loaded Fallout' 'initialization progress popup'
Require-Text (Join-Path $scriptRoot 'InitializationPromptTick.txt') 'Setup:InitializationPromptVersion' 'persistent initialization prompt marker'
Require-Text (Join-Path $scriptRoot 'InitializationPromptTick.txt') 'PlayerRef\.GetParentCell' 'loaded player cell prompt gate'
Require-Text (Join-Path $scriptRoot 'InitializationPromptTick.txt') 'MessageBoxExAlt' 'initialization prompt message box'
Require-Text (Join-Path $scriptRoot 'InitializationPromptTick.txt') '\|OK\|Close"' 'initialization prompt OK and Close buttons'
Reject-Text @((Join-Path $scriptRoot 'InitializationPromptTick.txt')) 'Initialize Now|Not Now' 'retired initialization prompt choices'
Reject-Text @((Join-Path $scriptRoot 'InitializationPromptTick.txt')) 'if MenuMode' 'menu mode initialization prompt gate'
Require-Text (Join-Path $scriptRoot 'InitializationPromptSelect.gek') 'iButton != 0' 'initialization prompt Close action gate'
Require-Text (Join-Path $scriptRoot 'InitializationPromptSelect.gek') 'SetINIIntC "Setup:InitializationPromptVersion" 1' 'prompt marker after user selection'
Require-Text (Join-Path $scriptRoot 'InitializationPromptSelect.gek') 'MessageExAlt 4 "#4\|Dialectic initialization has started\. Please wait\."' 'happy initialization started notice'
Require-Text $bootstrapFile 'Dialectic/InitializationPromptTick\.txt' 'initialization prompt bootstrap schedule'

# The retired AI Agents MCM must stay removed while the core agent runtime remains intact.
Reject-Text @($mcmFile) '"listTitle": "AI Agents"|AgentManagementAction\.gek' 'retired AI Agents MCM wiring'
if (Test-Path -LiteralPath (Join-Path $repoRoot 'Mod\Data\NVSE\user_defined_functions\Dialectic\AgentManagementAction.gek')) {
    $failures.Add('Retired AI Agent MCM callback must not be restored')
}
Reject-Text @((Join-Path $sourceRoot 'GameLoop.cpp'), (Join-Path $sourceRoot 'GameLoop.h')) '\bManageAIAgents\b|BuildAgentNameSummary' 'retired AI Agent management entrypoint'
Reject-Text @((Join-Path $sourceRoot 'main.cpp')) 'GameLoop::ManageAIAgents' 'active AI Agent MCM command wiring'
Require-Text (Join-Path $sourceRoot 'main.cpp') 'Deprecated AI Agent MCM ABI slot' 'reserved AI Agent command ABI slot'
Require-Text (Join-Path $sourceRoot 'ActivationManager.cpp') 'ActivateNearbyActors' 'nearby agent activation implementation'
Require-Text (Join-Path $sourceRoot 'ActivationManager.cpp') 'DeactivateAllActors' 'all-agent removal implementation'
Require-Text (Join-Path $sourceRoot 'AgentManager.cpp') 'UnregisterAIAgent' 'canonical agent removal implementation'
Require-Text (Join-Path $sourceRoot 'TargetManager.cpp') 'g_currentTarget\.isAIAgent = false' 'target registry removal synchronization'

# Requested dead configuration and compatibility residue must not return.
Reject-Text $cppPaths '\bdefaultProfile\b|nearbyItemsSendOnHeldChange|dynamicProfileTimerEnabled|DialecticHaltAIActions' 'retired Dialectic code/config symbol'
Reject-Text @((Join-Path $sourceRoot 'GameLoop.cpp')) 'IsRecentDuplicatePlayerTtsLine' 'duplicate GameLoop player-TTS helper'
Reject-Text @((Join-Path $sourceRoot 'main.cpp')) 'case 30:' 'retired DynamicProfile Enabled setting ID'
Reject-Text @($iniFile) '^DefaultProfile=|^SendOnHeldChange=' 'retired Dialectic INI key'

if ($failures.Count -gt 0) {
    $failures | ForEach-Object { Write-Error $_ }
    exit 1
}

$bridgeCount = (Get-ChildItem -Path (Join-Path $repoRoot 'Mod\Data\NVSE\user_defined_functions\Dialectic') -File).Count
Write-Host "Native runtime static verification passed. Deployed user-defined scripts remaining: $bridgeCount"
