cmake_minimum_required(VERSION 3.21)

project(Dialectic VERSION 0.5.2)

math(EXPR DIALECTIC_PLUGIN_INFO_VERSION
    "${PROJECT_VERSION_MAJOR} * 10000 + ${PROJECT_VERSION_MINOR} * 100 + ${PROJECT_VERSION_PATCH}")

include(CTest)

# Set C++ standard (xNVSE requires C++20)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# NVSE plugin needs to be a DLL
add_library(${PROJECT_NAME} SHARED
    src/main.cpp
    src/Logger.cpp
    src/ActionManager.cpp
    src/HTTPManager.cpp
    src/ResponseRouter.cpp
    src/ResponseQueueFNV.cpp
    src/AudioManager.cpp
    src/SpeakManager.cpp
    src/AgentManager.cpp
    src/ActivationManager.cpp
    src/ActorEligibilityFNV.cpp
    src/ActorPositionResolverFNV.cpp
    src/SpatialPathProviderFNV.cpp
    src/SpatialAwarenessFNV.cpp
    src/SpatialSnapshotManagerFNV.cpp
    src/WorldContextFNV.cpp
    src/NearbyActorsFNV.cpp
    src/AutoGreetingFNV.cpp
    src/ActivityStatusFNV.cpp
    src/NearbyItemsFNV.cpp
    src/NearbyPoiFNV.cpp
    src/QuestJournalFNV.cpp
    src/PlayerInventoryManagerFNV.cpp
    src/LoadedPluginsFNV.cpp
    src/WorldDataSyncFNV.cpp
    src/ImportDataSyncFNV.cpp
    src/VoiceSampleOverridesFNV.cpp
    src/VoiceSampleResolverFNV.cpp
    src/VoiceSampleBatchUploadFNV.cpp
    src/BsaArchiveReader.cpp
    src/XNVSEAdapter.cpp
    src/FNVRuntime.cpp
    src/RuntimeGeneration.cpp
    src/RuntimeEventBus.cpp
    src/RuntimeSnapshot.cpp
    src/NativeComparisonTelemetry.cpp
    src/GameThreadDispatcher.cpp
    src/TaskManager.cpp
    src/Config.cpp
    src/Misc.cpp
    src/InputManager.cpp
    src/TargetManager.cpp
    src/TradeManager.cpp
    src/GameLoop.cpp
    src/Console.cpp
    src/IngameNotifier.cpp
    src/VersionCheck.cpp
    src/NPCDetector.cpp
    src/VoiceRecorder.cpp
)

# Keep the official SDK isolated behind XNVSEAdapter. Other modules include only
# Dialectic's stable runtime interfaces.
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/xnvse-sdk
)

# Compile definitions
target_compile_definitions(${PROJECT_NAME} PRIVATE
    _CRT_SECURE_NO_WARNINGS
    RUNTIME=1
    _SILENCE_ALL_CXX17_DEPRECATION_WARNINGS
    DIALECTIC_VERSION="${PROJECT_VERSION}"
    DIALECTIC_PLUGIN_INFO_VERSION=${DIALECTIC_PLUGIN_INFO_VERSION}
)

# Compiler options
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        /W3          # Warning level 3
        /MP          # Multi-processor compilation
        /std:c++20   # C++20 for xNVSE
        /Zc:__cplusplus # Report correct C++ version
        /bigobj      # Allow large object files
    )
endif()

if(BUILD_TESTING)
    add_executable(DialecticTaskManagerTests
        tests/TaskManagerTests.cpp
        src/TaskManager.cpp
    )
    target_include_directories(DialecticTaskManagerTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(DialecticTaskManagerTests PRIVATE _CRT_SECURE_NO_WARNINGS)
    if(MSVC)
        target_compile_options(DialecticTaskManagerTests PRIVATE /W3 /std:c++20 /Zc:__cplusplus)
    endif()
    add_test(NAME DialecticTaskManagerTests COMMAND DialecticTaskManagerTests)

    add_executable(DialecticGameThreadDispatcherTests
        tests/GameThreadDispatcherTests.cpp
        src/GameThreadDispatcher.cpp
    )
    target_include_directories(DialecticGameThreadDispatcherTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(DialecticGameThreadDispatcherTests PRIVATE _CRT_SECURE_NO_WARNINGS)
    if(MSVC)
        target_compile_options(DialecticGameThreadDispatcherTests PRIVATE /W3 /std:c++20 /Zc:__cplusplus)
    endif()
    add_test(NAME DialecticGameThreadDispatcherTests COMMAND DialecticGameThreadDispatcherTests)

    add_executable(DialecticTaskPipelineIntegrationTests
        tests/TaskPipelineIntegrationTests.cpp
        src/TaskManager.cpp
        src/GameThreadDispatcher.cpp
    )
    target_include_directories(DialecticTaskPipelineIntegrationTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(DialecticTaskPipelineIntegrationTests PRIVATE _CRT_SECURE_NO_WARNINGS)
    if(MSVC)
        target_compile_options(DialecticTaskPipelineIntegrationTests PRIVATE /W3 /std:c++20 /Zc:__cplusplus)
    endif()
    add_test(NAME DialecticTaskPipelineIntegrationTests COMMAND DialecticTaskPipelineIntegrationTests)

    add_executable(DialecticBsaArchiveReaderTests
        tests/BsaArchiveReaderTests.cpp
        src/BsaArchiveReader.cpp
    )
    target_include_directories(DialecticBsaArchiveReaderTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(DialecticBsaArchiveReaderTests PRIVATE _CRT_SECURE_NO_WARNINGS)
    if(MSVC)
        target_compile_options(DialecticBsaArchiveReaderTests PRIVATE /W3 /std:c++20 /Zc:__cplusplus)
    endif()
    add_test(NAME DialecticBsaArchiveReaderTests COMMAND DialecticBsaArchiveReaderTests)
endif()

# Link against Windows libraries
target_link_libraries(${PROJECT_NAME} PRIVATE
    ws2_32      # Winsock2
    winhttp     # WinHTTP
    winmm       # Windows Multimedia (audio)
    xaudio2     # XAudio2 playback
    psapi       # Process memory diagnostics
    ole32       # COM
    oleaut32    # COM automation
    shell32     # Known user folders for reliable logging
)

# Set output name to match NVSE plugin naming convention
set_target_properties(${PROJECT_NAME} PROPERTIES
    OUTPUT_NAME "dialectic"
    PREFIX ""
    SUFFIX ".dll"
)

# Copy to Fallout NV Data/NVSE/Plugins folder if available
if(DEFINED ENV{FNV_DATA_PATH})
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
            $<TARGET_FILE:${PROJECT_NAME}>
            "$ENV{FNV_DATA_PATH}/NVSE/Plugins/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
    )
endif()
