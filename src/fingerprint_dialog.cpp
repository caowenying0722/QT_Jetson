#include "fingerprint_dialog.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace {
bool isSimulation(const QString& port) {
    return QFileInfo(port).canonicalFilePath().startsWith(QStringLiteral("/dev/pts/"));
}
}

FingerprintDialog::FingerprintDialog(const QString& port, QWidget* parent) : QDialog(parent) {
    setObjectName("fingerprintDialog");
    setWindowTitle(QStringLiteral("串口指纹"));
    resize(520, 230);
    auto* layout = new QVBoxLayout(this);
    auto* fields = new QFormLayout;
    port_ = new QLineEdit(port, this);
    port_->setObjectName("fingerprintPort");
    port_->setPlaceholderText(QStringLiteral("/dev/serial/by-id/…"));
    fields->addRow(QStringLiteral("串口设备"), port_);
    slot_ = new QSpinBox(this);
    slot_->setObjectName("fingerprintSlot");
    slot_->setRange(0, 199);
    slot_->setValue(1);
    fields->addRow(QStringLiteral("录入槽位"), slot_);
    layout->addLayout(fields);

    mode_ = new QLabel(this);
    mode_->setTextFormat(Qt::PlainText);
    layout->addWidget(mode_);
    const auto updateMode = [this](const QString& path) {
        mode_->setText(isSimulation(path.trimmed()) ? QStringLiteral("伪终端演练 · 57600 8N1")
                                                    : QStringLiteral("UART 指纹模块 · 57600 8N1"));
    };
    connect(port_, &QLineEdit::textChanged, this, updateMode);
    updateMode(port_->text());

    status_ = new QLabel(QStringLiteral("输入设备路径后点「连接检查」"), this);
    status_->setObjectName("fingerprintStatus");
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    layout->addWidget(status_, 1);

    auto* actions = new QHBoxLayout;
    probe_ = new QPushButton(QStringLiteral("连接检查"), this);
    probe_->setObjectName("fingerprintProbe");
    probe_->setDefault(true);
    enroll_ = new QPushButton(QStringLiteral("录入指纹"), this);
    enroll_->setObjectName("fingerprintEnroll");
    enroll_->setAutoDefault(false);
    verify_ = new QPushButton(QStringLiteral("验证指纹"), this);
    verify_->setObjectName("fingerprintVerify");
    verify_->setAutoDefault(false);
    auto* close = new QPushButton(QStringLiteral("关闭"), this);
    for (auto* button : {probe_, enroll_, verify_, close}) {
        actions->addWidget(button);
    }
    layout->addLayout(actions);
    connect(probe_, &QPushButton::clicked, this, [this] { startOperation("probe"); });
    connect(enroll_, &QPushButton::clicked, this, [this] { startOperation("enroll"); });
    connect(verify_, &QPushButton::clicked, this, [this] { startOperation("verify"); });
    connect(close, &QPushButton::clicked, this, &FingerprintDialog::reject);
    connect(&process_, &QProcess::readyReadStandardOutput, this, &FingerprintDialog::readMessages);
    connect(&process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, &FingerprintDialog::finishOperation);
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            status_->setText(QStringLiteral("无法启动串口助手：%1").arg(process_.errorString()));
            operation_.clear();
            setBusy(false);
        }
    });
}

FingerprintDialog::~FingerprintDialog() {
    process_.disconnect(this);
    if (process_.state() != QProcess::NotRunning) {
        process_.kill();
        process_.waitForFinished(500);
    }
}

QString FingerprintDialog::port() const {
    return port_->text().trimmed();
}

void FingerprintDialog::reject() {
    // 关闭时不再接受迟到的验证结果。串口助手退出时会释放设备。
    operation_.clear();
    if (process_.state() != QProcess::NotRunning) {
        process_.kill();
    }
    QDialog::reject();
}

void FingerprintDialog::startOperation(const QString& operation) {
    if (process_.state() != QProcess::NotRunning) {
        return;
    }
    if (port().isEmpty()) {
        status_->setText(QStringLiteral("请填写指纹模块的串口路径"));
        return;
    }
    operation_ = operation;
    simulation_ = isSimulation(port());
    malformed_ = false;
    error_.clear();
    result_ = {};
    QStringList args{"-u", QCoreApplication::applicationDirPath() + "/fingerprint_uart.py", "--port", port()};
    if (operation == "enroll") {
        args << "--slot" << QString::number(slot_->value());
    }
    args << operation;
    // 等手指、读串口都在子进程中执行，Qt 只收进度，摄像头窗口可以继续刷新。
    setBusy(true);
    status_->setText(QStringLiteral("正在检查串口…"));
    process_.start(QStringLiteral("python3"), args);
}

void FingerprintDialog::readMessages() {
    while (process_.canReadLine()) {
        const auto message = QJsonDocument::fromJson(process_.readLine()).object();
        const auto event = message.value("event").toString();
        if (event == "prompt") {
            status_->setText(message.value("text").toString());
        } else if (event == "error") {
            error_ = message.value("text").toString();
        } else if (event == "result" && result_.isEmpty()) {
            result_ = message;
        } else {
            malformed_ = true;
        }
    }
}

void FingerprintDialog::finishOperation(int exitCode, QProcess::ExitStatus exitStatus) {
    if (operation_.isEmpty()) {
        return;
    }
    readMessages();
    const QString operation = operation_;
    operation_.clear();
    setBusy(false);
    if (exitStatus != QProcess::NormalExit || exitCode != 0 || malformed_ ||
        result_.value("operation").toString() != operation) {
        status_->setText(error_.isEmpty() ? QStringLiteral("串口操作失败，请检查设备和串口助手")
                                         : error_);
        return;
    }
    if (operation == "probe") {
        const int capacity = result_.value("capacity").toInt(-1);
        if (capacity >= 1 && capacity <= 1024) {
            slot_->setMaximum(capacity - 1);
            status_->setText(QStringLiteral("连接成功 · 容量 %1 个槽位").arg(capacity));
            return;
        }
    } else if (operation == "enroll") {
        if (result_.value("slot").toInt(-1) == slot_->value()) {
            status_->setText(QStringLiteral("录入成功 · 槽位 %1").arg(slot_->value()));
            return;
        }
    } else if (operation == "verify" && result_.value("matched").isBool()) {
        if (!result_.value("matched").toBool()) {
            status_->setText(QStringLiteral("拒绝 · 指纹未匹配"));
            return;
        }
        const int slot = result_.value("slot").toInt(-1);
        const int score = result_.value("score").toInt(-1);
        if (slot >= 0 && slot < 1024 && score >= 0) {
            // 只有本次验证正常结束且模块明确匹配，才向主窗口发一次开锁事件。
            status_->setText(QStringLiteral("验证通过 · 槽位 %1 · 模块分数 %2").arg(slot).arg(score));
            emit verified(slot, simulation_);
            return;
        }
    }
    status_->setText(QStringLiteral("串口助手返回的数据不完整"));
}

void FingerprintDialog::setBusy(bool busy) {
    for (auto* widget : {static_cast<QWidget*>(port_), static_cast<QWidget*>(slot_),
                         static_cast<QWidget*>(probe_), static_cast<QWidget*>(enroll_),
                         static_cast<QWidget*>(verify_)}) {
        widget->setEnabled(!busy);
    }
}
