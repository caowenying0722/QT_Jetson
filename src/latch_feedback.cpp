#include "latch_feedback.h"

#include <QFile>

#include <memory>
#include <stdexcept>
#include <system_error>
#include <utility>

#ifdef Q_OS_LINUX
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/gpio.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

using namespace std::chrono_literals;

LatchFeedback::LatchFeedback(Reader read) : read_(std::move(read)) {}

void LatchFeedback::unlockSent() {
    if (phase_ != Phase::Opening) {
        observedDeparture_ = false;
    }
    requiresDeparture_ = true;
    phase_ = Phase::Opening;
}

void LatchFeedback::relockSent(Clock::time_point now) {
    phase_ = Phase::Returning;
    deadline_ = now + 1500ms;
}

LatchFeedback::State LatchFeedback::poll(Clock::time_point now) {
    const auto raw = read_();
    if (!raw) {
        candidate_.reset();
        stable_.reset();
        return State::ReadError;
    }
    // 机械触点可能抖动。新电平至少保持 60 ms，才替换上一条可信读数。
    if (raw != candidate_) {
        candidate_ = raw;
        candidateSince_ = now;
    } else if (now - candidateSince_ >= 60ms) {
        stable_ = candidate_;
    }

    if (phase_ == Phase::Opening) {
        if (stable_ == false && raw == false) {
            observedDeparture_ = true;
            return State::Released;
        }
        return State::Checking;
    }
    if (phase_ == Phase::Returning) {
        // 开过锁后必须先观察到触点释放，再接受回锁到位。一直闭合的触点不能通过。
        if (stable_ == true && raw == true && (!requiresDeparture_ || observedDeparture_)) {
            phase_ = Phase::Idle;
            return State::AtLimit;
        }
        if (now >= deadline_) {
            return requiresDeparture_ && !observedDeparture_ && stable_ == true
                ? State::DidNotRelease : State::TimedOut;
        }
        return State::Waiting;
    }
    if (!stable_) {
        return State::Checking;
    }
    return *stable_ ? State::AtLimit : State::Released;
}

#ifdef Q_OS_LINUX
namespace {
class GpioInput {
public:
    GpioInput(const QString& path, unsigned offset) {
        const int chip = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_CLOEXEC);
        if (chip < 0) {
            throw std::system_error(errno, std::generic_category(), "Open GPIO chip " + path.toStdString());
        }
        gpiohandle_request request{};
        request.lineoffsets[0] = offset;
        request.lines = 1;
        // 常开触点闭合后拉到地。内核把低电平转换成逻辑值 1。
        request.flags = GPIOHANDLE_REQUEST_INPUT | GPIOHANDLE_REQUEST_ACTIVE_LOW;
        std::strcpy(request.consumer_label, "smartlock-limit");
        const int result = ::ioctl(chip, GPIO_GET_LINEHANDLE_IOCTL, &request);
        const int error = errno;
        ::close(chip);
        if (result < 0) {
            throw std::system_error(error, std::generic_category(),
                                    "Request GPIO input " + path.toStdString() + ":" + std::to_string(offset));
        }
        fd_ = request.fd;
    }
    ~GpioInput() { ::close(fd_); }

    std::optional<bool> read() const {
        gpiohandle_data values{};
        if (::ioctl(fd_, GPIOHANDLE_GET_LINE_VALUES_IOCTL, &values) < 0) {
            return std::nullopt;
        }
        return values.values[0] != 0;
    }

private:
    int fd_;
};
}
#endif

LatchFeedback::Reader openLatchSwitch(const QString& chipPath, unsigned lineOffset) {
#ifdef Q_OS_LINUX
    // 使用 Jetson 当前内核支持的 GPIO 字符设备 ABI，无需启动额外进程。
    const auto input = std::make_shared<GpioInput>(chipPath, lineOffset);
    return [input] { return input->read(); };
#else
    (void)chipPath;
    (void)lineOffset;
    throw std::runtime_error("锁舌 GPIO 反馈需要 Linux GPIO 字符设备");
#endif
}
