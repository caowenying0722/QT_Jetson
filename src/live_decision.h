#pragma once

#include <QString>

#include <chrono>
#include <cstdint>
#include <optional>

class LiveDecision {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr int requiredMatches = 3;
    static constexpr auto rearmDelay = std::chrono::seconds(1);

    struct State {
        QString authorizedName;
        int matchingFrames = 0;
        bool shouldUnlock = false;
    };

    // 有脸但匹配失败时，matchedName 为空，facePresent 仍为真。
    // 使用采集序号和时间，界面刷新不会影响判断。
    State observe(const QString& matchedName, bool facePresent, std::uint64_t frameNumber,
                  Clock::time_point capturedAt);
    void forgetTemplate(const QString& name);

private:
    QString candidateName_;
    int matchingFrames_ = 0;
    std::uint64_t previousFrame_ = 0;
    QString authorizedName_;
    std::optional<Clock::time_point> deniedSince_;
    bool unlockedDuringPresence_ = false;
    std::optional<Clock::time_point> absentSince_;
};
