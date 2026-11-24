#include "face_gesture_challenge.h"

#include <iostream>

int main() {
    using namespace std::chrono_literals;
    const FaceGestureChallenge::Clock::time_point start{};
    FaceGestureChallenge challenge;
    if (challenge.consume(1, start)) {
        std::cerr << "A gesture without a face unlocked.\n";
        return 1;
    }

    const auto first = challenge.start(start, start + 1s);
    if (!first || !challenge.consume(first->id, start + 29s) || challenge.consume(first->id, start + 29s)) {
        std::cerr << "A valid challenge must unlock exactly once.\n";
        return 1;
    }

    const auto expiring = challenge.start(start, start + 10s);
    if (!expiring || challenge.consume(expiring->id, start + 30s) || challenge.start(start, start + 31s)) {
        std::cerr << "Expiry must use the time the camera captured the face.\n";
        return 1;
    }

    const auto previous = challenge.start(start + 31s, start + 32s);
    const auto replacement = challenge.start(start + 33s, start + 34s);
    if (!previous || !replacement || challenge.consume(previous->id, start + 35s) ||
        !challenge.consume(replacement->id, start + 35s)) {
        std::cerr << "An old gesture dialog must not consume a replacement challenge.\n";
        return 1;
    }

    const auto cancelled = challenge.start(start + 36s, start + 37s);
    challenge.cancel();
    if (!cancelled || challenge.consume(cancelled->id, start + 38s)) {
        std::cerr << "A cancelled challenge unlocked.\n";
        return 1;
    }
    return 0;
}
