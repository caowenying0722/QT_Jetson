#include "pwm_servo.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QThread>

#include <stdexcept>

namespace {
constexpr int periodNs = 20000000;
constexpr int lockedNs = 1500000;
constexpr int unlockedNs = 2000000;

[[noreturn]] void fail(const QString& path, const QString& reason) {
    throw std::runtime_error(QStringLiteral("%1 · %2").arg(path, reason).toStdString());
}

QByteArray readAttribute(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        fail(path, file.errorString());
    }
    const auto value = file.readAll().trimmed();
    if (file.error() != QFileDevice::NoError) {
        fail(path, file.errorString());
    }
    return value;
}

void writeAttribute(const QString& path, unsigned value) {
    QFile file(path);
    // ExistingOnly 防止路径填错时创建普通文件，Unbuffered 让内核写入错误直接返回。
    const auto bytes = QByteArray::number(value) + '\n';
    if (!file.open(QIODevice::WriteOnly | QIODevice::ExistingOnly | QIODevice::Unbuffered) ||
        file.write(bytes) != bytes.size()) {
        fail(path, file.errorString());
    }
}
}

QString findPin33PwmChip() {
    const QDir pwm(QStringLiteral("/sys/class/pwm"));
    for (const auto& chip : pwm.entryList({QStringLiteral("pwmchip*")}, QDir::Dirs)) {
        const auto path = pwm.filePath(chip);
        const QFileInfo controller(QFileInfo(path + "/device").canonicalFilePath());
        if (controller.fileName() == "32c0000.pwm") {
            return path;
        }
    }
    return {};
}

bool pin33PwmConfiguredAtBoot() {
    QFile record(QStringLiteral("/run/jetson-smartlock/pin33-pwm5"));
    return record.open(QIODevice::ReadOnly) && record.readAll().trimmed() == "0x00000405";
}

PwmServo::PwmServo(const QString& chipPath, unsigned channel)
    : chipPath_(QDir(chipPath).absolutePath()),
      channelPath_(QDir(chipPath_).filePath(QStringLiteral("pwm%1").arg(channel))), channel_(channel) {
    bool valid = false;
    const auto count = readAttribute(chipPath_ + "/npwm").toUInt(&valid);
    if (!valid || channel_ >= count) {
        fail(chipPath_, QStringLiteral("PWM 通道超出范围"));
    }
    if (QFile::exists(channelPath_)) {
        fail(channelPath_, QStringLiteral("通道已导出，请先停止占用它的程序"));
    }

    writeAttribute(chipPath_ + "/export", channel_);
    exported_ = true;
    try {
        // udev 可能在属性出现之后才授权普通用户写入。
        const auto writable = [this] {
            return QFileInfo(channelPath_ + "/period").isWritable() &&
                   QFileInfo(channelPath_ + "/duty_cycle").isWritable() &&
                   QFileInfo(channelPath_ + "/enable").isWritable();
        };
        for (int attempt = 0; attempt < 50 && !writable(); ++attempt) {
            QThread::msleep(10);
        }
        if (readAttribute(channelPath_ + "/polarity") != "normal") {
            fail(channelPath_, QStringLiteral("此舵机需要 normal 极性"));
        }
        if (readAttribute(channelPath_ + "/enable") == "1") {
            fail(channelPath_, QStringLiteral("通道仍在输出，请先关闭旧输出"));
        }
        const auto oldDuty = readAttribute(channelPath_ + "/duty_cycle").toULongLong(&valid);
        if (!valid) {
            fail(channelPath_, QStringLiteral("无法读取原 PWM 脉宽"));
        }
        if (oldDuty > periodNs) {
            writeAttribute(channelPath_ + "/duty_cycle", 0);
        }
        // 先建立 50 Hz 和回锁脉宽，再启用硬件输出。
        writeAttribute(channelPath_ + "/period", periodNs);
        lock();
        enabled_ = true;
        writeAttribute(channelPath_ + "/enable", 1);
    } catch (...) {
        release();
        throw;
    }
}

PwmServo::~PwmServo() { release(); }

void PwmServo::unlock() { writeAttribute(channelPath_ + "/duty_cycle", unlockedNs); }

void PwmServo::lock() { writeAttribute(channelPath_ + "/duty_cycle", lockedNs); }

void PwmServo::release() noexcept {
    if (enabled_) {
        try {
            lock();
            // 退出时仍发一段回位脉冲，给舵机留出移动时间。
            QThread::msleep(350);
        } catch (const std::exception& error) {
            qWarning() << "PWM relock on exit:" << error.what();
        }
        try {
            writeAttribute(channelPath_ + "/enable", 0);
        } catch (const std::exception& error) {
            qWarning() << "PWM disable on exit:" << error.what();
        }
    }
    if (exported_) {
        try {
            writeAttribute(chipPath_ + "/unexport", channel_);
        } catch (const std::exception& error) {
            qWarning() << "PWM release on exit:" << error.what();
        }
    }
}
