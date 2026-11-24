#include "demo_window.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using namespace std::chrono_literals;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void waitForText(const QLabel& label, const QString& expected, int timeoutMs) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!label.text().contains(expected) && elapsed.elapsed() < timeoutMs) {
        QApplication::processEvents();
        QThread::msleep(5);
    }
    require(label.text().contains(expected), "UI did not report the expected outcome.");
}

void runAttempt(QPushButton& button, const QLabel& status, const QByteArray& scenario,
                const QString& expected) {
    qputenv("GESTURE_GUI_TEST_SCENARIO", scenario);
    button.click();
    waitForText(status, expected, 2000);
}

QByteArray readFixture(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll().trimmed() : QByteArray{};
}

void writeFixture(const QString& path, const QByteArray& value) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(value) == value.size(), "Cannot create PWM fixture.");
}

void enableTestPwm(DemoWindow& window, QDir chip) {
    require(chip.mkpath("staged"), "Cannot create PWM attribute directory.");
    writeFixture(chip.filePath("npwm"), "1");
    writeFixture(chip.filePath("export"), "");
    writeFixture(chip.filePath("unexport"), "");
    writeFixture(chip.filePath("staged/period"), "0");
    writeFixture(chip.filePath("staged/duty_cycle"), "0");
    writeFixture(chip.filePath("staged/polarity"), "normal");
    writeFixture(chip.filePath("staged/enable"), "0");
    // 看到 export 写入后再暴露属性，模拟 Linux 创建 pwm0 的过程。
    std::thread kernel([chip]() mutable {
        for (int attempt = 0; attempt < 50; ++attempt) {
            if (readFixture(chip.filePath("export")) == "0") {
                chip.rename("staged", "pwm0");
                break;
            }
            QThread::msleep(10);
        }
    });
    try {
        window.enableServoPwm(chip.absolutePath(), 0);
    } catch (...) {
        kernel.join();
        throw;
    }
    kernel.join();
    require(readFixture(chip.filePath("pwm0/period")) == "20000000" &&
            readFixture(chip.filePath("pwm0/duty_cycle")) == "1500000" &&
            readFixture(chip.filePath("pwm0/enable")) == "1", "PWM startup did not configure the locked position.");
}

void checkDeviceDiagnostic(DemoWindow& window, const QString& missingCamera, bool pwm,
                           const QString& expectedFeedback = QStringLiteral("未启用")) {
    auto* entry = window.findChild<QPushButton*>("deviceCheckButton");
    auto* path = window.findChild<QLineEdit*>("cameraDevicePath");
    require(entry && path, "Device diagnostic entry or camera path is missing.");
    path->setText(missingCamera);
    qputenv("GESTURE_GUI_TEST_SCENARIO", "accepted");
    bool passed = false;
    QString failure;
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog) {
            return;
        }
        try {
            auto* camera = dialog->findChild<QLabel*>("cameraCheckResult");
            auto* gesture = dialog->findChild<QLabel*>("gestureCheckResult");
            auto* pwmResult = dialog->findChild<QLabel*>("pwmCheckResult");
            auto* latch = dialog->findChild<QLabel*>("latchFeedbackCheckResult");
            auto* refresh = dialog->findChild<QPushButton*>("deviceCheckRefresh");
            require(camera && gesture && pwmResult && latch && refresh, "Device check results are missing.");
            require(camera->text().contains(QStringLiteral("找不到视频设备")), "Missing camera was not reported.");
            require(latch->text().contains(expectedFeedback), "Self-check did not report the current latch feedback.");
            waitForText(*gesture, QStringLiteral("0x7620"), 2000);
            if (pwm) {
                require(pwmResult->text().contains("1500000"), "Device check did not read the configured PWM.");
            }
            qputenv("GESTURE_GUI_TEST_SCENARIO", "probe_failed");
            refresh->click();
            waitForText(*gesture, QStringLiteral("Remote I/O error"), 2000);
            passed = true;
        } catch (const std::exception& error) {
            failure = QString::fromUtf8(error.what());
        }
        dialog->reject();
    });
    entry->click();
    if (!passed) {
        throw std::runtime_error(failure.isEmpty() ? "Device diagnostic did not open" : failure.toStdString());
    }
}

void checkFeedbackTiming() {
    using State = LatchFeedback::State;
    const LatchFeedback::Clock::time_point start{};
    std::optional<bool> pressed = true;
    LatchFeedback feedback([&] { return pressed; });
    feedback.relockSent(start);
    require(feedback.poll(start) == State::Waiting, "Initial level was trusted before debounce.");
    require(feedback.poll(start + 60ms) == State::AtLimit, "Stable initial position not reported.");

    feedback.unlockSent();
    pressed = false;
    require(feedback.poll(start + 100ms) == State::Checking, "A one-sample release was trusted.");
    pressed = true;
    feedback.poll(start + 120ms);
    feedback.relockSent(start + 3000ms);
    require(feedback.poll(start + 3000ms) == State::Waiting, "A stuck contact falsely confirmed.");
    require(feedback.poll(start + 4500ms) == State::DidNotRelease, "Missing departure not reported.");

    feedback.unlockSent();
    pressed = false;
    feedback.poll(start + 5000ms);
    require(feedback.poll(start + 5060ms) == State::Released, "Stable departure was missed.");
    feedback.relockSent(start + 8000ms);
    require(feedback.poll(start + 9500ms) == State::TimedOut, "Missing arrival did not time out.");
    pressed = std::nullopt;
    require(feedback.poll(start + 9510ms) == State::ReadError, "GPIO failure did not invalidate feedback.");
    pressed = true;
    require(feedback.poll(start + 9600ms) == State::TimedOut, "Unstable recovery falsely confirmed.");
    require(feedback.poll(start + 9660ms) == State::AtLimit, "Late arrival after recovery was missed.");
}
}

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    try {
        const bool pwm = app.arguments().size() == 3 && app.arguments().last() == "--pwm";
        require(app.arguments().size() == 2 || pwm, "Usage: gesture_gui_test <assets-dir> [--pwm]");
        const QDir assets(app.arguments().at(1));
        QTemporaryDir temp;
        require(temp.isValid(), "Could not create temporary database.");
        const auto database = temp.filePath("faces.json");
        const QDir chip(temp.filePath("pwmchip-test"));
        const auto duty = chip.filePath("pwm0/duty_cycle");
        auto owner = std::make_unique<DemoWindow>(assets.filePath("models"), database);
        auto& window = *owner;
        if (pwm) {
            checkFeedbackTiming();
            enableTestPwm(window, chip);
        }
        window.show();
        checkDeviceDiagnostic(window, temp.filePath("missing-video"), pwm);
        auto* open = window.findChild<QPushButton*>("gestureButton");
        auto* log = window.findChild<QPlainTextEdit*>("eventLog");
        auto* combined = window.findChild<QCheckBox*>("combinedVerification");
        auto* latch = window.findChild<QLabel*>("latchStatus");
        require(open && log && combined && latch, "Gesture entry, log or access controls are missing.");
        // 定时器避免错误地弹出手势窗口时测试卡在模态事件循环中。
        bool unexpectedDialog = false;
        QTimer closeUnexpectedDialog;
        closeUnexpectedDialog.setSingleShot(true);
        QObject::connect(&closeUnexpectedDialog, &QTimer::timeout, &window, [&] {
            if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
                unexpectedDialog = true;
                dialog->reject();
            }
        });
        combined->setChecked(true);
        closeUnexpectedDialog.start(0);
        open->click();
        closeUnexpectedDialog.stop();
        require(!unexpectedDialog && log->toPlainText().isEmpty(), "Combined mode began without a live face.");
        combined->setChecked(false);

        bool completed = false;
        QString failure;
        QTimer::singleShot(0, &window, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog) {
                return;
            }
            try {
                auto* read = dialog->findChild<QPushButton*>("gestureRead");
                auto* verify = dialog->findChild<QPushButton*>("gestureVerify");
                auto* status = dialog->findChild<QLabel*>("gestureStatus");
                require(read && verify && status, "Gesture controls are missing.");

                // 用测试子进程输出覆盖 Qt 的四种分支。真实 I²C 通信由实机另行验证。
                runAttempt(*read, *status, "accepted", QStringLiteral("读取结束"));
                runAttempt(*verify, *status, "denied", QStringLiteral("拒绝"));
                runAttempt(*verify, *status, "failed", QStringLiteral("未正常完成"));
                require(log->toPlainText().isEmpty(), "Read, denial or failed process unlocked.");
                if (pwm) {
                    require(QFile::rename(duty, duty + ".saved"), "Cannot inject PWM write failure.");
                    runAttempt(*verify, *status, "accepted", QStringLiteral("未成功"));
                    require(UnlockHistory(database).newestFirst().isEmpty(), "Failed PWM write recorded an unlock.");
                    require(QFile::rename(duty + ".saved", duty), "Cannot restore the PWM attribute.");
                    waitForText(*latch, QStringLiteral("回锁指令已发送"), 2500);
                }
                runAttempt(*verify, *status, "accepted", pwm ? QStringLiteral("PWM 开锁指令已发送")
                                                               : QStringLiteral("授权通过"));
                const auto event = pwm ? QStringLiteral("PWM 开锁指令已发送 | 手势 · 左 → 上 → 顺时针")
                                       : QStringLiteral("模拟开锁 | 手势 · 左 → 上 → 顺时针");
                require(log->toPlainText().count(event) == 1, "Successful verification must log one unlock.");
                if (pwm) {
                    require(readFixture(duty) == "2000000", "Valid gesture did not write the unlock pulse.");
                    require(QFile::rename(duty, duty + ".saved"), "Cannot inject a relock failure.");
                    waitForText(*latch, QStringLiteral("回锁失败"), 4500);
                    require(QFile::rename(duty + ".saved", duty), "Cannot restore PWM for relock retry.");
                } else {
                    require(latch->text().contains(QStringLiteral("已解锁")), "Valid gesture did not open the simulated latch.");
                }
                const auto entries = UnlockHistory(database).newestFirst();
                const auto method = pwm ? QStringLiteral("手势（PWM 指令）") : QStringLiteral("手势");
                require(entries.size() == 1 && entries.front().method == method &&
                        entries.front().subject == QStringLiteral("左 → 上 → 顺时针"),
                        "Only the successful gesture should persist in history.");
                completed = true;
            } catch (const std::exception& error) {
                failure = QString::fromUtf8(error.what());
            }
            dialog->reject();
        });
        open->click();
        if (!completed) {
            throw std::runtime_error(failure.isEmpty() ? "Gesture dialog did not complete" : failure.toStdString());
        }
        waitForText(*latch, pwm ? QStringLiteral("回锁指令已发送") : QStringLiteral("已上锁"), 4500);
        if (pwm) {
            require(readFixture(duty) == "1500000", "Relock did not write the locked pulse.");
            bool pressed = true;
            window.enableLatchFeedback([&] { return pressed; });
            auto* feedback = window.findChild<QLabel*>("latchFeedbackStatus");
            require(feedback, "Latch feedback status is missing.");
            waitForText(*feedback, QStringLiteral("锁舌已到位"), 500);
            checkDeviceDiagnostic(window, temp.filePath("missing-video"), true, QStringLiteral("锁舌已到位"));
            QTimer::singleShot(0, &window, [&] {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                if (!dialog) {
                    return;
                }
                try {
                    auto* verify = dialog->findChild<QPushButton*>("gestureVerify");
                    auto* status = dialog->findChild<QLabel*>("gestureStatus");
                    require(verify && status, "Gesture verification controls are missing.");
                    runAttempt(*verify, *status, "accepted", QStringLiteral("PWM 开锁指令已发送"));
                    require(feedback->text().contains(QStringLiteral("正在确认")), "Old arrival survived unlock.");
                    pressed = false;
                    waitForText(*feedback, QStringLiteral("触点释放"), 500);
                } catch (const std::exception& error) {
                    failure = QString::fromUtf8(error.what());
                }
                dialog->reject();
            });
            open->click();
            require(failure.isEmpty() && !pressed, "Could not simulate departure in a new unlock cycle.");
            waitForText(*feedback, QStringLiteral("等待锁舌到位"), 4000);
            pressed = true;
            waitForText(*feedback, QStringLiteral("锁舌已到位"), 500);
            require(log->toPlainText().count(QStringLiteral("锁舌反馈 · 锁舌已到位")) == 2,
                    "Session log did not record confirmed arrival.");
            owner.reset();
            require(readFixture(chip.filePath("pwm0/enable")) == "0" &&
                    readFixture(chip.filePath("unexport")) == "0", "Shutdown did not release its PWM channel.");
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
