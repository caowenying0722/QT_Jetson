#pragma once

#include <QString>

QString findPin33PwmChip();
bool pin33PwmConfiguredAtBoot();

// 独占一个 Linux PWM 通道。period 和 duty_cycle 在 sysfs 中都以纳秒为单位。
class PwmServo {
public:
    PwmServo(const QString& chipPath, unsigned channel);
    ~PwmServo();
    PwmServo(const PwmServo&) = delete;
    PwmServo& operator=(const PwmServo&) = delete;

    void unlock();
    void lock();
    const QString& chipPath() const { return chipPath_; }
    unsigned channel() const { return channel_; }

private:
    void release() noexcept;

    QString chipPath_;
    QString channelPath_;
    unsigned channel_;
    bool exported_ = false;
    bool enabled_ = false;
};
