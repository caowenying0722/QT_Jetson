#include "device_check.h"
#include "pwm_servo.h"

#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QTimer>

#ifdef Q_OS_LINUX
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace {
QString readSetting(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromLatin1(file.readAll().trimmed()) : QString{};
}

QString checkCamera(const QString& path) {
    if (!QFile::exists(path)) {
        return QStringLiteral("找不到视频设备 · %1").arg(path);
    }
#ifdef Q_OS_LINUX
    // QUERYCAP 只检查节点能力，摄像头预览可以继续运行。
    const auto node = QFile::encodeName(path);
    const int fd = ::open(node.constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        return QStringLiteral("无法打开设备 · %1").arg(QString::fromLocal8Bit(std::strerror(errno)));
    }
    v4l2_capability cap{};
    const int result = ::ioctl(fd, VIDIOC_QUERYCAP, &cap);
    const int error = errno;
    ::close(fd);
    if (result < 0) {
        return QStringLiteral("无法查询 V4L2 能力 · %1").arg(QString::fromLocal8Bit(std::strerror(error)));
    }
    const auto capabilities = cap.capabilities & V4L2_CAP_DEVICE_CAPS ? cap.device_caps : cap.capabilities;
    if (!(capabilities & (V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_VIDEO_CAPTURE_MPLANE))) {
        return QStringLiteral("节点无视频采集能力 · %1").arg(path);
    }
    return QStringLiteral("采集节点可访问 · %1").arg(QFileInfo(path).canonicalFilePath());
#else
    return QStringLiteral("设备路径存在 · %1").arg(path);
#endif
}

QString checkPwm(const PwmServo* servo) {
    const auto chip = servo ? servo->chipPath() : findPin33PwmChip();
    if (chip.isEmpty()) {
        return QStringLiteral("未找到 Pin 33 对应的 PWM5 控制器");
    }
    const auto path = QStringLiteral("%1/pwm%2").arg(chip).arg(servo ? servo->channel() : 0);
    if (!servo) {
        if (QFile::exists(path)) {
            return QStringLiteral("通道已被其他程序导出 · %1").arg(path);
        }
        if (!QFileInfo(chip + "/export").isWritable()) {
            return QStringLiteral("当前用户无法导出通道 · %1").arg(chip);
        }
        return QStringLiteral("找到控制器，通道未导出 · %1").arg(chip);
    }
    const auto enabled = readSetting(path + "/enable");
    const auto period = readSetting(path + "/period");
    const auto duty = readSetting(path + "/duty_cycle");
    if (enabled != "1" || period.isEmpty() || duty.isEmpty()) {
        return QStringLiteral("无法确认当前 PWM 输出配置 · %1").arg(path);
    }
    return QStringLiteral("驱动已启用 · 周期 %1 ns · 脉宽 %2 ns").arg(period, duty);
}
}

void showDeviceChecks(QWidget* parent, const QString& camera, const PwmServo* servo, const QLabel& latchFeedback) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("设备自检"));
    dialog.setMinimumWidth(520);
    auto* layout = new QFormLayout(&dialog);
    const auto addResult = [&](const QString& title, const char* name) {
        auto* label = new QLabel;
        label->setObjectName(QString::fromLatin1(name));
        label->setTextFormat(Qt::PlainText);
        label->setWordWrap(true);
        layout->addRow(title, label);
        return label;
    };
    auto* video = addResult(QStringLiteral("摄像头"), "cameraCheckResult");
    auto* gesture = addResult(QStringLiteral("I²C 手势"), "gestureCheckResult");
    auto* pwm = addResult(QStringLiteral("PWM 通道"), "pwmCheckResult");
    auto* pin = addResult(QStringLiteral("Pin 33 复用"), "pinCheckResult");
    auto* latch = addResult(QStringLiteral("锁舌反馈"), "latchFeedbackCheckResult");
    auto* note = new QLabel(QStringLiteral("检查设备与驱动状态。舵臂动作需现场确认。"));
    note->setWordWrap(true);
    layout->addRow(note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    auto* refresh = buttons->addButton(QStringLiteral("重新检查"), QDialogButtonBox::ActionRole);
    refresh->setObjectName("deviceCheckRefresh");
    layout->addRow(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    QProcess probe;
    QTimer timeout;
    timeout.setSingleShot(true);
    bool timedOut = false;
    const auto run = [&] {
        video->setText(checkCamera(camera));
        pwm->setText(checkPwm(servo));
        latch->setText(latchFeedback.text());
        pin->setText(pin33PwmConfiguredAtBoot() ? QStringLiteral("本次启动已回读 PWM5 · 0x00000405")
                                               : QStringLiteral("本次启动无复用服务成功记录"));
        // 复用现有的 ID 探测。保持 Qt 可响应，探测过程不会初始化手势或触发开锁。
        timedOut = false;
        refresh->setEnabled(false);
        gesture->setText(QStringLiteral("正在读取 /dev/i2c-7 芯片 ID…"));
        const auto helper = QDir(QCoreApplication::applicationDirPath()).filePath("gesture_paj7620.py");
        timeout.start(2500);
        probe.start(QStringLiteral("python3"), {helper, QStringLiteral("probe")});
    };
    QObject::connect(refresh, &QPushButton::clicked, &dialog, run);
    QObject::connect(&timeout, &QTimer::timeout, &dialog, [&] {
        timedOut = true;
        probe.kill();
    });
    QObject::connect(&probe, &QProcess::errorOccurred, &dialog, [&](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            timeout.stop();
            gesture->setText(QStringLiteral("手势助手启动失败 · %1").arg(probe.errorString()));
            refresh->setEnabled(true);
        }
    });
    QObject::connect(&probe, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), &dialog,
        [&](int code, QProcess::ExitStatus status) {
            timeout.stop();
            const auto reply = QJsonDocument::fromJson(probe.readAllStandardOutput()).object();
            if (timedOut) {
                gesture->setText(QStringLiteral("读取芯片 ID 超时"));
            } else if (status == QProcess::NormalExit && code == 0 &&
                       reply.value("event").toString() == "chip" && reply.value("id").toString() == "0x7620") {
                gesture->setText(QStringLiteral("PAJ7620U2 有应答 · 0x7620"));
            } else {
                const auto error = reply.value("message").toString();
                gesture->setText(error.isEmpty() ? QStringLiteral("手势助手未返回有效芯片 ID") : error);
            }
            probe.readAllStandardError();
            refresh->setEnabled(true);
        });
    run();
    dialog.exec();
    QObject::disconnect(&probe, nullptr, &dialog, nullptr);
    if (probe.state() != QProcess::NotRunning) {
        probe.kill();
        probe.waitForFinished(500);
    }
}
