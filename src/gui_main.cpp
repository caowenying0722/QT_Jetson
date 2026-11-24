#include "demo_window.h"
#include "pwm_servo.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTimer>

#include <exception>
#include <iostream>
#include <stdexcept>
#include <utility>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("smartlock_gui"));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Jetson 人脸门禁"));
    parser.addHelpOption();
    const QCommandLineOption models(QStringList{"m", "models"}, "模型目录", "dir", "assets/models");
    const QCommandLineOption database(QStringList{"d", "db"}, "模板库文件", "file", "data/faces.json");
    const QCommandLineOption camera("camera", "启动指定 V4L2 摄像头", "device");
    const QCommandLineOption measure("measure-seconds", "测量 5–120 秒，输出 JSON 后退出", "seconds");
    const QCommandLineOption fingerprint("fingerprint-port", "预填指纹串口设备", "device");
    const QCommandLineOption servoPwm("servo-pwm", "使用指定的 Linux PWM 控制器驱动舵机", "pwmchip-path");
    const QCommandLineOption servoChannel("servo-channel", "控制器内的 PWM 通道索引", "index", "0");
    const QCommandLineOption servoPin33("servo-pin33", "使用启动服务配置的 Pin 33 PWM5 驱动舵机");
    const QCommandLineOption latchGpio("latch-gpio", "启用低电平到位反馈（需已接限位开关与外部上拉）", "/dev/gpiochipN:offset");
    parser.addOptions({models, database, camera, measure, fingerprint, servoPwm, servoChannel, servoPin33, latchGpio});
    parser.addPositionalArgument("image", "启动时打开的图片", "[image]");
    parser.process(app);
    const auto images = parser.positionalArguments();
    const bool automated = parser.isSet(measure);
    bool validSeconds = false;
    const int measureSeconds = automated ? parser.value(measure).toInt(&validSeconds) : 20;
    bool validChannel = false;
    const auto channel = parser.value(servoChannel).toUInt(&validChannel);
    const auto gpio = QRegularExpression(QStringLiteral("^(/dev/gpiochip[0-9]+):([0-9]+)$"))
                          .match(parser.value(latchGpio));
    bool validOffset = false;
    const auto offset = gpio.captured(2).toUInt(&validOffset);
    if (images.size() > 1 || (parser.isSet(camera) && !images.isEmpty()) ||
        (automated && (!parser.isSet(camera) || !validSeconds || measureSeconds < 5 || measureSeconds > 120)) ||
        (parser.isSet(servoPin33) && parser.isSet(servoPwm)) || (parser.isSet(servoChannel) && !parser.isSet(servoPwm)) ||
        (parser.isSet(latchGpio) && (!validOffset || (!parser.isSet(servoPin33) && !parser.isSet(servoPwm)))) ||
        (parser.isSet(servoPwm) && (parser.value(servoPwm).trimmed().isEmpty() || !validChannel))) {
        parser.showHelp(1);
    }

    try {
        DemoWindow window(parser.value(models), parser.value(database));
        window.setFingerprintPort(parser.value(fingerprint));
        LatchFeedback::Reader limitSwitch;
        if (parser.isSet(latchGpio)) {
            limitSwitch = openLatchSwitch(gpio.captured(1), offset);
        }
        if (parser.isSet(servoPwm)) {
            window.enableServoPwm(parser.value(servoPwm), channel);
        } else if (parser.isSet(servoPin33)) {
            if (!pin33PwmConfiguredAtBoot()) {
                throw std::runtime_error("Pin 33 PWM5 尚未通过本次启动的复用检查，请查看 smartlock-pin33-pwm5.service");
            }
            const auto chip = findPin33PwmChip();
            if (chip.isEmpty()) {
                throw std::runtime_error("找不到 Pin 33 对应的 32c0000.pwm 控制器");
            }
            window.enableServoPwm(chip, 0);
        }
        if (limitSwitch) {
            window.enableLatchFeedback(std::move(limitSwitch));
        }
        QObject::connect(&window, &DemoWindow::measurementReady, &app, [&app, automated](const QJsonObject& metrics) {
            std::cout << QJsonDocument(metrics).toJson(QJsonDocument::Compact).constData() << std::endl;
            if (automated) {
                app.exit(metrics.value("processed_frames").toInt() > 0 ? 0 : 1);
            }
        });
        QObject::connect(&window, &DemoWindow::cameraFailed, &app, [&app, automated](const QString& error) {
            std::cerr << error.toStdString() << '\n';
            if (automated) {
                app.exit(1);
            }
        });
        QObject::connect(&window, &DemoWindow::liveEvent, &app, [](const QString& event) {
            // 状态变化写到终端，便于核对实机；图片和模板名称留在窗口内。
            std::cerr << "live " << event.toStdString() << std::endl;
        });
        window.show();
        if (!images.isEmpty()) {
            window.openImage(images.front());
        }
        if (parser.isSet(camera)) {
            QTimer::singleShot(0, &window, [&window, device = parser.value(camera), measureSeconds] {
                window.startCamera(device, measureSeconds);
            });
        }
        return app.exec();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
