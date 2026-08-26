#include "ActorEligibilityFNV.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

namespace {

constexpr int kFormTypeTesNpc = 0x2A;
constexpr int kFormTypeTesCreature = 0x2B;

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string TrimLower(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    value = value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
    return Lower(value);
}

std::string CombinedText(const ActorEligibilityFNV::Metadata& metadata) {
    return Lower(metadata.name + " " +
        metadata.race + " " +
        metadata.voiceId + " " +
        metadata.voiceName + " " +
        metadata.baseId);
}

template <size_t N>
bool ContainsAny(const std::string& text, const std::array<const char*, N>& terms, const char** matched = nullptr) {
    for (const char* term : terms) {
        if (text.find(term) != std::string::npos) {
            if (matched) {
                *matched = term;
            }
            return true;
        }
    }
    return false;
}

bool LooksRobot(const std::string& text) {
    static constexpr std::array<const char*, 13> kTerms = {
        "robot",
        "protectron",
        "securitron",
        "mister handy",
        "mr handy",
        "mrhandy",
        "mister gutsy",
        "mr gutsy",
        "mrgutsy",
        "eyebot",
        "robobrain",
        "sentry bot",
        "sentrybot"
    };
    return ContainsAny(text, kTerms);
}

bool LooksSuperMutant(const std::string& text) {
    static constexpr std::array<const char*, 3> kTerms = {
        "super mutant",
        "supermutant",
        "nightkin"
    };
    return ContainsAny(text, kTerms);
}

bool LooksNonFeralGhoul(const std::string& text) {
    return text.find("ghoul") != std::string::npos &&
        text.find("feral") == std::string::npos;
}

bool LooksHuman(const std::string& text) {
    static constexpr std::array<const char*, 15> kTerms = {
        "caucasian",
        "african american",
        "hispanic",
        "asian",
        "human",
        "raider",
        "tribal",
        "maleadult",
        "femaleadult",
        "maleunique",
        "femaleunique",
        "maleold",
        "femaleold",
        "malechild",
        "femalechild"
    };
    return ContainsAny(text, kTerms);
}

// Some voiced story characters use TESCreature records despite being conversational NPCs.
bool LooksNamedStoryCharacter(const std::string& text) {
    static constexpr std::array<const char*, 3> kTerms = {
        "mr. house",
        "mr house",
        "mrhouse"
    };
    return ContainsAny(text, kTerms);
}

bool HasExplicitCreatureBlock(const std::string& text, std::string* reason) {
    static constexpr std::array<const char*, 26> kTerms = {
        "feral",
        "cyberdog",
        "dogmeat",
        "dog",
        "rex",
        "deathclaw",
        "cazador",
        "radscorpion",
        "gecko",
        "nightstalker",
        "bloatfly",
        "mantis",
        "mole rat",
        "molerat",
        "brahmin",
        "bighorner",
        "centaur",
        "spore carrier",
        "spore plant",
        "tunneler",
        "lakelurk",
        "fire ant",
        "giant ant",
        "radroach",
        "yao guai",
        "turret"
    };

    const char* matched = nullptr;
    if (!ContainsAny(text, kTerms, &matched)) {
        return false;
    }

    if (reason) {
        *reason = std::string("creature category is not auto-managed: ") + matched;
    }
    return true;
}

bool LooksGenericCreature(const std::string& text) {
    return text.find("creature") != std::string::npos ||
        text.find("animal") != std::string::npos;
}

bool HasAllowedConversationalCategory(const ActorEligibilityFNV::Metadata& metadata, const std::string& text) {
    if (metadata.baseTypeKnown && metadata.baseType == kFormTypeTesNpc) {
        return true;
    }

    return LooksHuman(text) ||
        LooksNonFeralGhoul(text) ||
        LooksSuperMutant(text) ||
        LooksRobot(text) ||
        LooksNamedStoryCharacter(text);
}

} // namespace

namespace ActorEligibilityFNV {

bool IsTargetableActorIdentity(const Metadata& metadata, std::string* reason) {
    const std::string name = TrimLower(metadata.name);
    if (name.empty() || name == "message" || name == "<no name>" ||
        name == "none" || name == "null") {
        if (reason) {
            *reason = "form does not have a targetable actor identity";
        }
        return false;
    }

    if (metadata.baseTypeKnown && metadata.baseType != kFormTypeTesNpc &&
        metadata.baseType != kFormTypeTesCreature) {
        if (reason) {
            *reason = "form type is not an NPC or creature";
        }
        return false;
    }
    return true;
}

bool IsManualActivationAllowed(const Metadata& metadata, std::string* reason) {
    return IsTargetableActorIdentity(metadata, reason);
}

bool IsClearlyDisallowedCreature(const Metadata& metadata, std::string* reason) {
    const std::string text = CombinedText(metadata);
    if (HasExplicitCreatureBlock(text, reason)) {
        return true;
    }

    if (metadata.isCreatureKnown && metadata.isCreature && !HasAllowedConversationalCategory(metadata, text)) {
        if (reason) {
            *reason = "creature form is not an allowed conversational category";
        }
        return true;
    }

    if (metadata.baseTypeKnown && metadata.baseType == kFormTypeTesCreature &&
        !HasAllowedConversationalCategory(metadata, text)) {
        if (reason) {
            *reason = "TESCreature form is not an allowed conversational category";
        }
        return true;
    }

    if (LooksGenericCreature(text) && !HasAllowedConversationalCategory(metadata, text)) {
        if (reason) {
            *reason = "generic creature metadata is not an allowed conversational category";
        }
        return true;
    }

    return false;
}

bool IsAutoActivationAllowed(const Metadata& metadata, std::string* reason) {
    if (!IsTargetableActorIdentity(metadata, reason)) {
        return false;
    }
    const std::string text = CombinedText(metadata);
    if (HasExplicitCreatureBlock(text, reason)) {
        return false;
    }

    if (HasAllowedConversationalCategory(metadata, text)) {
        return true;
    }

    if (metadata.isCreatureKnown && metadata.isCreature) {
        if (reason) {
            *reason = "creatures are disabled for auto activation";
        }
        return false;
    }

    if (metadata.baseTypeKnown && metadata.baseType == kFormTypeTesCreature) {
        if (reason) {
            *reason = "TESCreature forms are disabled for auto activation";
        }
        return false;
    }

    if (LooksGenericCreature(text)) {
        if (reason) {
            *reason = "generic creature metadata is disabled for auto activation";
        }
        return false;
    }

    if (reason) {
        *reason = "actor is not in an auto-managed conversational category";
    }
    return false;
}

bool IsRechatAllowed(const Metadata& metadata, bool manuallyActivated, std::string* reason) {
    if (!IsTargetableActorIdentity(metadata, reason)) {
        return false;
    }
    if (manuallyActivated) {
        return true;
    }

    const std::string text = CombinedText(metadata);
    if (HasExplicitCreatureBlock(text, reason)) {
        return false;
    }

    if (metadata.isCreatureKnown && metadata.isCreature && !HasAllowedConversationalCategory(metadata, text)) {
        if (reason) {
            *reason = "creatures are disabled for rechat";
        }
        return false;
    }

    if (metadata.baseTypeKnown && metadata.baseType == kFormTypeTesCreature &&
        !HasAllowedConversationalCategory(metadata, text)) {
        if (reason) {
            *reason = "TESCreature forms are disabled for rechat";
        }
        return false;
    }

    if (LooksGenericCreature(text) && !HasAllowedConversationalCategory(metadata, text)) {
        if (reason) {
            *reason = "generic creature metadata is disabled for rechat";
        }
        return false;
    }

    return true;
}

} // namespace ActorEligibilityFNV
