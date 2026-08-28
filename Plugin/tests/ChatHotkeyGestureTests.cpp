#include "ChatHotkeyGesture.h"

#include <cstdlib>
#include <iostream>

namespace {
void Expect(std::uint8_t actual, std::uint8_t expected, const char* scenario) {
    if (actual != expected) {
        std::cerr << scenario << ": expected " << int(expected) << ", got " << int(actual) << '\n';
        std::exit(1);
    }
}
}

int main() {
    using namespace InputManager;

    ChatHotkeyGesture text{700};
    text.Press(0);
    Expect(text.Consume(100), NoGesture, "Text does not open on press");
    text.Release(150);
    Expect(text.Consume(150), Tap, "Text opens on a short release");

    text.Press(1000);
    text.Press(1300); // Key repeat must not restart the clock.
    Expect(text.Consume(1699), NoGesture, "Text hold threshold lower boundary");
    Expect(text.Consume(1700), Hold, "Text wait fires while held");
    Expect(text.Consume(2000), NoGesture, "Wait fires once per hold");
    text.Release(2200);
    Expect(text.Consume(2200), NoGesture, "Wait release does not open textbox");

    text.Press(3000);
    text.Release(3700);
    Expect(text.Consume(4000), Hold, "Slow frame still identifies text hold from key edges");

    ChatHotkeyGesture voice{350, 350};
    voice.Press(0);
    voice.Release(100);
    Expect(voice.Consume(449), NoGesture, "Single tap waits for second tap");
    Expect(voice.Consume(450), Tap, "Single tap clears only after window expires");
    Expect(voice.Consume(600), NoGesture, "Single tap fires once");

    voice.Press(1000);
    voice.Release(1080);
    voice.Press(1150);
    voice.Release(1230);
    Expect(voice.Consume(1500), DoubleTap, "Two taps between frames only wait, never clear or record");

    voice.Press(2000);
    Expect(voice.Consume(2349), NoGesture, "Voice is not recorded before hold threshold");
    Expect(voice.Consume(2350), Hold, "Voice starts at hold threshold");
    voice.Release(3000);
    Expect(voice.Consume(3400), NoGesture, "Voice release does not clear dialogue");

    voice.Press(4000);
    voice.Release(4050);
    voice.Press(4200);
    Expect(voice.Consume(4550), Hold, "Second press held becomes voice, not double tap");
    voice.Release(4600);
    Expect(voice.Consume(5000), NoGesture, "Second hold cancels the first pending tap");

    voice.Press(6000);
    voice.Release(6050);
    voice.Press(6400);
    Expect(voice.Consume(6400), Tap, "Press at expired window starts a new gesture");
    voice.Release(6450);
    Expect(voice.Consume(6800), Tap, "New isolated tap has its own window");

    voice.Press(7000);
    voice.Release(7350);
    Expect(voice.Consume(7400), Hold, "Late voice release is not mistaken for clear dialogue");

    voice.Press(8000);
    voice.Release(8050);
    voice.Cancel();
    Expect(voice.Consume(9000), NoGesture, "Menu, focus, load or rebind cancels a deferred tap");
    text.Press(8000);
    text.Cancel();
    text.Release(9000);
    Expect(text.Consume(9000), NoGesture, "Cancelled press cannot fire on release");
    voice.Press(10000);
    voice.Release(10050);
    voice.Press(10200);
    voice.Release(10250);
    voice.Cancel();
    Expect(voice.Consume(11000), NoGesture, "Cancellation also clears queued double-tap actions");
    std::cout << "Chat hotkey gesture checks passed\n";
}
