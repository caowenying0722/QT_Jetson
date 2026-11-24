#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

// 实时人脸只发放一次限时资格。手势结果必须对应打开对话框时的同一轮资格。
class FaceGestureChallenge {
public:
    using Clock = std::chrono::steady_clock;
    struct Ticket {
        std::uint64_t id;
        Clock::time_point expiresAt;
    };

    std::optional<Ticket> start(Clock::time_point faceCapturedAt, Clock::time_point now) {
        const auto expiresAt = faceCapturedAt + std::chrono::seconds(30);
        // Qt 卡顿时可能迟到一条识别结果。旧帧不能重新开启完整的验证窗口。
        if (expiresAt <= now) {
            return std::nullopt;
        }
        ticket_ = Ticket{++lastId_, expiresAt};
        return ticket_;
    }

    std::optional<Ticket> active(Clock::time_point now) const {
        return ticket_ && now < ticket_->expiresAt ? ticket_ : std::nullopt;
    }

    bool consume(std::uint64_t id, Clock::time_point now) {
        const auto ticket = active(now);
        if (!ticket || ticket->id != id) {
            return false;
        }
        cancel();
        return true;
    }

    void cancel() { ticket_.reset(); }

private:
    std::optional<Ticket> ticket_;
    std::uint64_t lastId_ = 0;
};
