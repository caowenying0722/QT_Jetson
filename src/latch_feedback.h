#pragma once

#include <QString>

#include <chrono>
#include <functional>
#include <optional>

class LatchFeedback {
public:
    using Clock = std::chrono::steady_clock;
    // true 表示锁舌压下了限位开关，空值表示 GPIO 读取失败。
    using Reader = std::function<std::optional<bool>()>;
    enum class State { Checking, Released, Waiting, AtLimit, TimedOut, DidNotRelease, ReadError };

    explicit LatchFeedback(Reader read);
    void unlockSent();
    void relockSent(Clock::time_point now = Clock::now());
    State poll(Clock::time_point now = Clock::now());

private:
    enum class Phase { Idle, Opening, Returning };
    Reader read_;
    Phase phase_ = Phase::Idle;
    std::optional<bool> candidate_;
    std::optional<bool> stable_;
    Clock::time_point candidateSince_;
    Clock::time_point deadline_;
    bool requiresDeparture_ = false;
    bool observedDeparture_ = false;
};

LatchFeedback::Reader openLatchSwitch(const QString& chipPath, unsigned lineOffset);
