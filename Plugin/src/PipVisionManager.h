#pragma once

namespace PipVisionManager {

// Starts one manual PipVision capture when no capture is already in flight.
bool RequestCapture();
void Update();
void Shutdown();

} // namespace PipVisionManager
