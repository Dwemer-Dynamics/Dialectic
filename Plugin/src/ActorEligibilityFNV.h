#pragma once

#include <cstdint>
#include <string>

namespace ActorEligibilityFNV {

struct Metadata {
    std::string name;
    std::string race;
    std::string voiceId;
    std::string voiceName;
    std::string baseId;
    int baseType = 0;
    bool baseTypeKnown = false;
    bool isCreature = false;
    bool isCreatureKnown = false;
};

bool IsClearlyDisallowedCreature(const Metadata& metadata, std::string* reason = nullptr);
bool IsTargetableActorIdentity(const Metadata& metadata, std::string* reason = nullptr);
bool IsManualActivationAllowed(const Metadata& metadata, std::string* reason = nullptr);
bool IsAutoActivationAllowed(const Metadata& metadata, std::string* reason = nullptr);
bool IsRechatAllowed(const Metadata& metadata, bool manuallyActivated, std::string* reason = nullptr);

} // namespace ActorEligibilityFNV
