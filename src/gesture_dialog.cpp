#include "gesture_dialog.h"

#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

GestureDialog::GestureDialog(QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("手势门禁"));
    setMinimumWidth(460);
    auto* layout = new QVBoxLayout(this);
    auto* instructions = new QLabel(QStringLiteral(
        "感应面前约 5–10 厘米。先试读方向，再验证「左 → 上 → 顺时针」。\n"
        "每轮监听 20 秒。每笔后停约半秒，5 秒内做下一笔。"));
    layout->addWidget(instructions);
    status_ = new QLabel(QStringLiteral("请选择操作"));
    status_->setObjectName("gestureStatus");
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    layout->addWidget(status_);
    lastGesture_ = new QLabel;
    lastGesture_->setTextFormat(Qt::PlainText);
    layout->addWidget(lastGesture_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    read_ = buttons->addButton(QStringLiteral("读取手势"), QDialogButtonBox::ActionRole);
    verify_ = buttons->addButton(QStringLiteral("开始验证"), QDialogButtonBox::ActionRole);
    read_->setObjectName("gestureRead");
    verify_->setObjectName("gestureVerify");
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(this, &QDialog::finished, this, [this] {
        closing_ = true;
        if (helper_.state() != QProcess::NotRunning) {
            helper_.kill();
        }
    });
    connect(read_, &QPushButton::clicked, this, [this] { start(false); });
    connect(verify_, &QPushButton::clicked, this, [this] { start(true); });
    connect(&helper_, &QProcess::readyReadStandardOutput, this, &GestureDialog::readEvents);
    connect(&helper_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, &GestureDialog::finish);
    connect(&helper_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (!closing_ && error == QProcess::FailedToStart) {
            timeout_.stop();
            status_->setText(QStringLiteral("无法启动手势助手 · %1").arg(helper_.errorString()));
            setBusy(false);
        }
    });
    timeout_.setSingleShot(true);
    connect(&timeout_, &QTimer::timeout, this, [this] {
        problem_ = QStringLiteral("手势助手响应超时");
        helper_.kill();
    });
}

GestureDialog::~GestureDialog() {
    if (helper_.state() != QProcess::NotRunning) {
        closing_ = true;
        helper_.kill();
        helper_.waitForFinished(500);
    }
}

void GestureDialog::setBusy(bool busy) {
    read_->setEnabled(!busy);
    verify_->setEnabled(!busy);
}

void GestureDialog::setVerificationFeedback(const QString& text) {
    status_->setText(text);
}

void GestureDialog::start(bool verify) {
    if (helper_.state() != QProcess::NotRunning) {
        return;
    }
    verifying_ = verify;
    unlocked_ = false;
    receivedResult_ = false;
    pending_.clear();
    problem_.clear();
    lastGesture_->clear();
    status_->setText(QStringLiteral("正在初始化 PAJ7620U2…"));
    setBusy(true);
    const auto path = QDir(QCoreApplication::applicationDirPath()).filePath("gesture_paj7620.py");
    // I²C 轮询运行在子进程里，Qt 事件循环继续负责绘制和响应关闭按钮。
    timeout_.start(25000);
    helper_.start(QStringLiteral("python3"),
                  {path, verify ? QStringLiteral("unlock") : QStringLiteral("watch")});
}

void GestureDialog::readEvents() {
    pending_ += helper_.readAllStandardOutput();
    // QProcess 一次可能给半行，也可能给多行。只解析完整的一行 JSON。
    int end;
    while ((end = pending_.indexOf('\n')) >= 0) {
        const auto event = QJsonDocument::fromJson(pending_.left(end)).object();
        pending_.remove(0, end + 1);
        const auto type = event.value("event").toString();
        if (type == "ready") {
            status_->setText(verifying_ ? QStringLiteral("等待「左」 · 0/3")
                                        : QStringLiteral("正在读取手势…"));
        } else if (type == "gesture") {
            lastGesture_->setText(QStringLiteral("最近动作　%1 · %2")
                .arg(event.value("name").toString(), event.value("flags").toString()));
            if (verifying_) {
                const auto step = event.value("step").toInt();
                if (step == 1) {
                    status_->setText(QStringLiteral("等待「上」 · 1/3"));
                } else if (step == 2) {
                    status_->setText(QStringLiteral("等待「顺时针」 · 2/3"));
                } else if (step == 0) {
                    status_->setText(QStringLiteral("序列重新开始 · 等待「左」 · 0/3"));
                }
            }
        } else if (type == "reset") {
            status_->setText(QStringLiteral("动作间隔超时 · 等待「左」 · 0/3"));
        } else if (type == "result") {
            receivedResult_ = event.value("unlocked").isBool();
            unlocked_ = event.value("unlocked").toBool();
        } else if (type == "error") {
            problem_ = event.value("message").toString();
        }
    }
}

void GestureDialog::finish(int exitCode, QProcess::ExitStatus exitStatus) {
    timeout_.stop();
    if (closing_) {
        return;
    }
    readEvents();
    setBusy(false);
    // 读取模式、断线和异常退出都不能触发开锁。
    if (!problem_.isEmpty() || exitStatus != QProcess::NormalExit || exitCode != 0 ||
        !receivedResult_ || !pending_.isEmpty()) {
        status_->setText(problem_.isEmpty() ? QStringLiteral("手势助手未正常完成") : problem_);
    } else if (verifying_ && unlocked_) {
        status_->setText(QStringLiteral("授权通过 · 模拟开锁"));
        emit verified();
    } else {
        status_->setText(verifying_ ? QStringLiteral("拒绝 · 本轮未完成手势序列")
                                    : QStringLiteral("读取结束"));
    }
}
