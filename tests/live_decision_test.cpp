#include "live_decision.h"

#include <exception>
#include <iostream>
#include <stdexcept>

namespace {
using namespace std::chrono_literals;

void expect(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// 用指定的帧号和时间重放边界条件，测试无需等待真实的一秒。
LiveDecision::State frame(LiveDecision& decision, const char* name, std::uint64_t number,
                          std::chrono::milliseconds offset, bool facePresent = true) {
    return decision.observe(QString::fromLatin1(name), facePresent, number,
                            LiveDecision::Clock::time_point{} + offset);
}

void confirmAndRearm() {
    LiveDecision decision;
    expect(frame(decision, "A", 1, 0ms).matchingFrames == 1, "first match should start confirmation");
    expect(!frame(decision, "A", 2, 50ms).shouldUnlock, "two matches must not unlock");
    expect(frame(decision, "", 3, 100ms).matchingFrames == 0, "failure must clear the streak");
    expect(frame(decision, "A", 4, 150ms).matchingFrames == 1, "the next match must start over");
    expect(!frame(decision, "A", 5, 200ms).shouldUnlock, "the second new match must wait");
    const auto confirmed = frame(decision, "A", 6, 250ms);
    expect(confirmed.shouldUnlock && confirmed.authorizedName == "A", "third match must unlock");
    expect(!frame(decision, "A", 7, 300ms).shouldUnlock, "continuing to match must not unlock again");

    expect(frame(decision, "", 8, 350ms).authorizedName == "A", "a short failure must keep authorization");
    expect(frame(decision, "", 9, 1349ms).authorizedName == "A", "999ms must keep authorization");
    expect(!frame(decision, "A", 10, 1350ms).shouldUnlock, "recovering must cancel the failure timer");
    expect(frame(decision, "", 11, 1400ms, false).authorizedName == "A", "next failure must start a new timer");
    expect(frame(decision, "", 12, 2399ms, false).authorizedName == "A", "999ms of absence must hold");
    expect(frame(decision, "", 13, 2400ms, false).authorizedName.isEmpty(), "1000ms of absence must rearm");

    expect(!frame(decision, "A", 14, 2450ms).shouldUnlock, "re-entry needs a fresh streak");
    expect(!frame(decision, "A", 15, 2500ms).shouldUnlock, "re-entry still needs a third frame");
    expect(frame(decision, "A", 16, 2550ms).shouldUnlock, "third re-entry frame must unlock again");
}

void changeIdentity() {
    LiveDecision decision;
    frame(decision, "A", 1, 0ms);
    frame(decision, "A", 2, 50ms);
    expect(frame(decision, "B", 3, 100ms).matchingFrames == 1, "identities must not share a streak");
    expect(!frame(decision, "B", 4, 150ms).shouldUnlock, "second B frame must wait");
    expect(frame(decision, "B", 5, 200ms).shouldUnlock, "third B frame must unlock");
    expect(frame(decision, "A", 6, 250ms).authorizedName == "B", "short identity mismatch must hold");
    const auto waiting = frame(decision, "A", 7, 1250ms);
    expect(waiting.authorizedName.isEmpty() && waiting.matchingFrames == 1 && !waiting.shouldUnlock,
           "a different identity needs a fresh streak after rearming");
    frame(decision, "A", 8, 1300ms);
    const auto confirmed = frame(decision, "A", 9, 1350ms);
    expect(confirmed.authorizedName == "A" && !confirmed.shouldUnlock,
           "the new identity may be confirmed but must not unlock before an empty scene");
}

void skipFrame() {
    LiveDecision decision;
    frame(decision, "A", 1, 0ms);
    frame(decision, "A", 2, 50ms);
    expect(frame(decision, "A", 4, 150ms).matchingFrames == 1, "skipped frame must break the streak");
    frame(decision, "A", 5, 200ms);
    expect(frame(decision, "A", 6, 250ms).shouldUnlock, "three consecutive captures must unlock");
}

void remainInView() {
    LiveDecision decision;
    frame(decision, "A", 1, 0ms);
    frame(decision, "A", 2, 50ms);
    expect(frame(decision, "A", 3, 100ms).shouldUnlock, "initial appearance must unlock");
    frame(decision, "", 4, 150ms);
    expect(frame(decision, "", 5, 1300ms).authorizedName.isEmpty(), "prolonged low scores must clear the display");
    frame(decision, "A", 6, 1350ms);
    frame(decision, "A", 7, 1400ms);
    const auto recovered = frame(decision, "A", 8, 1450ms);
    expect(recovered.authorizedName == "A" && !recovered.shouldUnlock,
           "reconfirming a face that remained in view must not open again");

    frame(decision, "", 9, 1500ms, false);
    frame(decision, "", 10, 2300ms, false);
    frame(decision, "", 11, 2350ms); // A detected face restarts the absence timer even if it is unknown.
    frame(decision, "", 12, 2400ms, false);
    frame(decision, "", 14, 3500ms, false); // A skipped frame breaks evidence of continuous absence.
    frame(decision, "A", 15, 3550ms);
    frame(decision, "A", 16, 3600ms);
    expect(!frame(decision, "A", 17, 3650ms).shouldUnlock,
           "interrupted or unobserved absence must not rearm the lock");
}

void revokeTemplate() {
    LiveDecision decision;
    frame(decision, "A", 1, 0ms);
    frame(decision, "A", 2, 50ms);
    decision.forgetTemplate("B");
    expect(frame(decision, "A", 3, 100ms).shouldUnlock, "deleting another name must keep the active streak");
    decision.forgetTemplate("A");
    expect(frame(decision, "", 4, 150ms).authorizedName.isEmpty(), "deleting a template must clear held authorization");
    frame(decision, "A", 5, 200ms);
    frame(decision, "A", 6, 250ms);
    decision.forgetTemplate("A");
    expect(frame(decision, "A", 7, 300ms).matchingFrames == 1, "deletion must discard earlier votes for that name");
    frame(decision, "A", 8, 350ms);
    const auto confirmed = frame(decision, "A", 9, 400ms);
    expect(confirmed.authorizedName == "A" && !confirmed.shouldUnlock,
           "deleting and re-enrolling during the same appearance must not unlock again");
}
}

int main() {
    try {
        confirmAndRearm();
        changeIdentity();
        skipFrame();
        remainInView();
        revokeTemplate();
        std::cout << "Passed: three matches, transient failures, timed absence, re-entry, identity changes, "
                     "skipped frames, prolonged low scores while still present, template revocation\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
