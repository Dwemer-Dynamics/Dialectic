// NearbyItemsFNV.h - Structured nearby item snapshots for DialecticServer prompts

#pragma once

namespace NearbyItemsFNV {

void Update();
void SendNow(bool force = false);

} // namespace NearbyItemsFNV
