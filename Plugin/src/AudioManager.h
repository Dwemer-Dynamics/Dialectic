// AudioManager.h - Audio playback and recording for Dialectic

#pragma once

#include <string>

namespace AudioManager {

// 3D position vector
struct Vector3 {
    float x, y, z;
};

// Initialize audio manager
void Initialize();

// Shutdown audio manager
void Shutdown();

// Load WAV from memory
bool LoadWAV(const unsigned char* wavData, unsigned long dataSize);

// Playback controls
bool Play();
void Stop();
void Pause();
void Resume();

// Update 3D audio positioning
void Update(const Vector3& sourcePos, const Vector3& listenerPos,
            const Vector3& listenerForward);

// Toggle player-heard 3D panning. When disabled, playback stays centered/flat 2D.
void Set3DPlaybackEnabled(bool enabled);

// Strength multiplier for player-heard 3D panning. 1.0 is natural, higher is more obvious.
void Set3DPlaybackStrength(float strength);

// Source volume control (0.0 to 2.0; values above 1.0 amplify head voices)
void SetVolume(float volume);

// Check playback status
bool IsPlaying();
bool IsPaused();
double GetCurrentPlayTime();

// Voice recording
void StartRecording();
void StopRecording();
bool IsRecording();
std::string GetRecordedAudio();

} // namespace AudioManager
