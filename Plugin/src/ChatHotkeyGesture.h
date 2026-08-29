#pragma once

#include <cstdint>

namespace InputManager {

enum ChatGesture : std::uint8_t { NoGesture = 0, Tap = 1, DoubleTap = 2, Hold = 4 };

// Recognize physical key edges before menus capture input or recording starts.
// The caller supplies monotonic milliseconds so boundary cases are testable without the game.
class ChatHotkeyGesture {
public:
    explicit ChatHotkeyGesture(std::uint64_t holdMs, std::uint64_t doubleTapMs = 0)
        : holdMs_(holdMs), doubleTapMs_(doubleTapMs) {}

    void Press(std::uint64_t now) {
        if (pressed_) return;
        ExpireTap(now);
        secondTap_ = tapPending_;
        tapPending_ = false;
        pressed_ = true;
        holdSent_ = false;
        pressedAt_ = now;
    }

    void Release(std::uint64_t now) {
        if (!pressed_) return;
        UpdateHold(now);
        pressed_ = false;
        if (!holdSent_) {
            if (secondTap_) {
                pending_ |= DoubleTap;
            } else if (doubleTapMs_ == 0) {
                pending_ |= Tap;
            } else {
                tapPending_ = true;
                releasedAt_ = now;
            }
        }
        secondTap_ = false;
    }

    std::uint8_t Consume(std::uint64_t now) {
        ExpireTap(now);
        UpdateHold(now);
        const auto result = pending_;
        pending_ = NoGesture;
        return result;
    }

    void Cancel() {
        pressed_ = false;
        holdSent_ = false;
        secondTap_ = false;
        tapPending_ = false;
        pending_ = NoGesture;
    }

private:
    void ExpireTap(std::uint64_t now) {
        if (tapPending_ && now - releasedAt_ >= doubleTapMs_) {
            tapPending_ = false;
            pending_ |= Tap;
        }
    }

    void UpdateHold(std::uint64_t now) {
        if (pressed_ && !holdSent_ && now - pressedAt_ >= holdMs_) {
            holdSent_ = true;
            secondTap_ = false;
            pending_ |= Hold;
        }
    }

    const std::uint64_t holdMs_;
    const std::uint64_t doubleTapMs_;
    std::uint64_t pressedAt_{0};
    std::uint64_t releasedAt_{0};
    bool pressed_{false};
    bool holdSent_{false};
    bool secondTap_{false};
    bool tapPending_{false};
    std::uint8_t pending_{NoGesture};
};

} // namespace InputManager
