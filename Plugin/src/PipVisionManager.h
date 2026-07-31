#pragma once

namespace PipVisionManager {

// Starts one manual PipVision capture when no capture is already in flight.
bool RequestCapture();
void Shutdown();

} // namespace PipVisionManager
