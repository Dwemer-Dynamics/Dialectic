// ActivityStatusFNV.h - Live actor activity snapshots for DialecticServer prompts

#pragma once

namespace ActivityStatusFNV {

void Update();
void SendNow(bool force = false);

} // namespace ActivityStatusFNV
