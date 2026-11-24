#include "demo_window.h"
#include "camera_session.h"
#include "device_check.h"
#include "fingerprint_dialog.h"
#include "gesture_dialog.h"
#include "pwm_servo.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <opencv2/imgproc.hpp>

#include <exception>
#include <utility>

namespace {
constexpr char neutral[] = "#31435c";
constexpr char passed[] = "#146447";
constexpr char denied[] = "#a12b31";
constexpr char defaultCamera[] = "/dev/v4l/by-id/usb-Generic_USB2.0_PC_CAMERA-video-index0";
constexpr int unlockHoldMs = 3000;
constexpr int relockRetryMs = 1000;

QString metricText(const QJsonObject& metrics, const char* key) {
    const auto value = metrics.value(QLatin1String(key));
    return value.isDouble() ? QString::number(value.toDouble(), 'f', 1) : QStringLiteral("—");
}

QString statusText(Decision decision) {
    switch (decision) {
    case Decision::Authorized: return QStringLiteral("授权通过 · 模拟开锁");
    case Decision::Unknown: return QStringLiteral("拒绝 · 未知用户");
    case Decision::NoEnrollment: return QStringLiteral("拒绝 · 请先录入用户");
    case Decision::NoFace: return QStringLiteral("拒绝 · 未检测到人脸");
    case Decision::MultipleFaces: return QStringLiteral("拒绝 · 检测到多张人脸");
    }
    return QStringLiteral("拒绝 · 无法确认状态");
}
}

DemoWindow::DemoWindow(const QString& modelsDir, const QString& databasePath)
    : modelsDir_(QDir(modelsDir).absolutePath()),
      databasePath_(QFileInfo(databasePath).absoluteFilePath()),
      service_(modelsDir_, databasePath_), unlockHistory_(databasePath_), faceColor_(neutral) {
    setObjectName("demoWindow");
    setWindowTitle(QStringLiteral("Jetson 人脸门禁"));
    setMinimumSize(850, 520);
    resize(960, 620);
    setStyleSheet(QStringLiteral(
        "QWidget#demoWindow { background: #f3f6fa; }"
        "QLabel { color: #1d2939; }"
        "QLineEdit, QPlainTextEdit { background: white; color: #1d2939;"
        " border: 1px solid #bcc8d8; border-radius: 6px; padding: 7px; }"
        "QPushButton { color: #183351; background: #e8eef7; border: 1px solid #9caec5;"
        " border-radius: 6px; padding: 8px 12px; }"
        "QPushButton:disabled { color: #536176; background: #edf1f5; }"
        "QPushButton#checkButton:enabled { color: white; background: #204f84; }"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(24, 20, 24, 20);
    root->setSpacing(12);
    auto* title = new QLabel(QStringLiteral("人脸门禁"));
    QFont titleFont = title->font();
    titleFont.setPointSize(22);
    titleFont.setBold(true);
    title->setFont(titleFont);
    auto* header = new QHBoxLayout;
    header->addWidget(title, 1);
    auto* devices = new QPushButton(QStringLiteral("设备自检…"));
    devices->setObjectName("deviceCheckButton");
    header->addWidget(devices);
    root->addLayout(header);
    connect(devices, &QPushButton::clicked, this, [this] {
        showDeviceChecks(this, cameraPath_->text().trimmed(), servo_.get(), *latchFeedbackStatus_);
    });

    auto* cameraControls = new QHBoxLayout;
    cameraControls->addWidget(new QLabel(QStringLiteral("视频设备")));
    cameraPath_ = new QLineEdit(QString::fromLatin1(defaultCamera));
    cameraPath_->setObjectName("cameraDevicePath");
    cameraControls->addWidget(cameraPath_, 1);
    cameraButton_ = new QPushButton(QStringLiteral("启动摄像头"));
    cameraControls->addWidget(cameraButton_);
    snapshotButton_ = new QPushButton(QStringLiteral("截帧用于录入"));
    snapshotButton_->setEnabled(false);
    cameraControls->addWidget(snapshotButton_);
    root->addLayout(cameraControls);

    auto* columns = new QHBoxLayout;
    columns->setSpacing(18);
    root->addLayout(columns, 1);

    auto* imageColumn = new QVBoxLayout;
    columns->addLayout(imageColumn, 3);
    auto* imageActions = new QHBoxLayout;
    imageColumn->addLayout(imageActions);
    chooseImageButton_ = new QPushButton(QStringLiteral("选择图片…"));
    imageActions->addWidget(chooseImageButton_);
    auto* manageButton = new QPushButton(QStringLiteral("人脸模板…"));
    manageButton->setObjectName("manageTemplatesButton");
    imageActions->addWidget(manageButton);
    auto* fingerprintButton = new QPushButton(QStringLiteral("指纹…"));
    fingerprintButton->setObjectName("fingerprintButton");
    imageActions->addWidget(fingerprintButton);
    auto* gestureButton = new QPushButton(QStringLiteral("手势…"));
    gestureButton->setObjectName("gestureButton");
    imageActions->addWidget(gestureButton);
    imageActions->addStretch();
    preview_ = new QLabel(QStringLiteral("选择一张图片开始"));
    preview_->setMinimumSize(390, 285);
    // 忽略 pixmap 的原始尺寸作为布局建议，让窗口可以正常缩小。
    preview_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    preview_->setAlignment(Qt::AlignCenter);
    preview_->setStyleSheet(QStringLiteral(
        "background: #e8eef5; border: 1px solid #bdcada; border-radius: 8px;"));
    imageColumn->addWidget(preview_, 1);
    fileName_ = new QLabel(QStringLiteral("尚未选择文件"));
    fileName_->setTextFormat(Qt::PlainText);
    fileName_->setWordWrap(true);
    imageColumn->addWidget(fileName_);

    auto* controls = new QVBoxLayout;
    controls->setSpacing(10);
    columns->addLayout(controls, 2);
    auto* nameLabel = new QLabel(QStringLiteral("录入名称"));
    controls->addWidget(nameLabel);
    name_ = new QLineEdit;
    name_->setObjectName("nameInput");
    name_->setPlaceholderText(QStringLiteral("例如 样例 A"));
    name_->setMaxLength(64);
    nameLabel->setBuddy(name_);
    controls->addWidget(name_);

    auto* actions = new QHBoxLayout;
    enrollButton_ = new QPushButton(QStringLiteral("录入当前图片"));
    enrollButton_->setObjectName("enrollButton");
    checkButton_ = new QPushButton(QStringLiteral("验证当前图片"));
    checkButton_->setObjectName("checkButton");
    enrollButton_->setEnabled(false);
    checkButton_->setEnabled(false);
    actions->addWidget(enrollButton_);
    actions->addWidget(checkButton_);
    controls->addLayout(actions);

    status_ = new QLabel;
    status_->setObjectName("decisionStatus");
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    details_ = new QLabel;
    details_->setObjectName("decisionDetails");
    details_->setTextFormat(Qt::PlainText);
    details_->setWordWrap(true);
    controls->addWidget(status_);
    controls->addWidget(details_);

    combinedVerification_ = new QCheckBox(QStringLiteral("实时人脸＋手势联合验证"));
    combinedVerification_->setObjectName("combinedVerification");
    controls->addWidget(combinedVerification_);
    combinedStatus_ = new QLabel;
    combinedStatus_->setTextFormat(Qt::PlainText);
    combinedStatus_->setWordWrap(true);
    combinedStatus_->hide();
    controls->addWidget(combinedStatus_);
    latchStatus_ = new QLabel(QStringLiteral("模拟锁舌 · 已上锁"));
    latchStatus_->setObjectName("latchStatus");
    controls->addWidget(latchStatus_);
    latchFeedbackStatus_ = new QLabel(QStringLiteral("未启用 · 需要限位开关"));
    latchFeedbackStatus_->setObjectName("latchFeedbackStatus");
    latchFeedbackStatus_->setTextFormat(Qt::PlainText);
    latchFeedbackStatus_->setWordWrap(true);
    latchFeedbackStatus_->hide();
    controls->addWidget(latchFeedbackStatus_);

    metrics_ = new QLabel;
    metrics_->setTextFormat(Qt::PlainText);
    metrics_->setWordWrap(true);
    metrics_->hide();
    controls->addWidget(metrics_);

    auto* logHeader = new QHBoxLayout;
    logHeader->addWidget(new QLabel(QStringLiteral("本次会话记录")), 1);
    auto* historyButton = new QPushButton(QStringLiteral("开锁历史…"));
    historyButton->setObjectName("unlockHistoryButton");
    logHeader->addWidget(historyButton);
    controls->addLayout(logHeader);
    events_ = new QPlainTextEdit;
    events_->setObjectName("eventLog");
    events_->setReadOnly(true);
    events_->setMaximumBlockCount(100);
    controls->addWidget(events_, 1);
    showStatus(QStringLiteral("等待输入"), QStringLiteral("模型已加载 · CPU"), neutral);

    connect(chooseImageButton_, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("选择测试图片"),
            QStringLiteral("assets/samples"), QStringLiteral("图片 (*.jpg *.jpeg *.png *.bmp)"));
        if (!path.isEmpty()) {
            openImage(path);
        }
    });
    connect(enrollButton_, &QPushButton::clicked, this, &DemoWindow::enroll);
    connect(checkButton_, &QPushButton::clicked, this, &DemoWindow::check);
    connect(manageButton, &QPushButton::clicked, this, &DemoWindow::manageTemplates);
    connect(fingerprintButton, &QPushButton::clicked, this, &DemoWindow::openFingerprint);
    connect(gestureButton, &QPushButton::clicked, this, &DemoWindow::openGesture);
    connect(historyButton, &QPushButton::clicked, this, &DemoWindow::openUnlockHistory);
    faceGestureTimer_ = new QTimer(this);
    faceGestureTimer_->setSingleShot(true);
    faceGestureTimer_->setTimerType(Qt::PreciseTimer);
    connect(faceGestureTimer_, &QTimer::timeout, this, [this] {
        cancelFaceGestureChallenge();
        combinedStatus_->setText(QStringLiteral("验证超时，请离开画面后重新入镜"));
    });
    connect(combinedVerification_, &QCheckBox::toggled, this, [this, fingerprintButton](bool enabled) {
        policyStartedAt_ = FaceGestureChallenge::Clock::now();
        cancelFaceGestureChallenge();
        combinedStatus_->setVisible(enabled);
        fingerprintButton->setEnabled(!enabled);
    });
    relockTimer_ = new QTimer(this);
    relockTimer_->setSingleShot(true);
    relockTimer_->setTimerType(Qt::PreciseTimer);
    connect(relockTimer_, &QTimer::timeout, this, &DemoWindow::relock);
    latchFeedbackTimer_ = new QTimer(this);
    latchFeedbackTimer_->setInterval(20);
    latchFeedbackTimer_->setTimerType(Qt::PreciseTimer);
    connect(latchFeedbackTimer_, &QTimer::timeout, this, &DemoWindow::pollLatchFeedback);
    cameraTimer_ = new QTimer(this);
    cameraTimer_->setTimerType(Qt::PreciseTimer);
    cameraTimer_->setInterval(16);
    connect(cameraTimer_, &QTimer::timeout, this, &DemoWindow::pollCamera);
    connect(cameraButton_, &QPushButton::clicked, this, [this] {
        if (camera_) {
            stopCamera();
            showStatus(QStringLiteral("摄像头已停止"), QStringLiteral("可以重新启动或选择图片"), neutral);
        } else {
            startCamera(cameraPath_->text().trimmed());
        }
    });
    connect(snapshotButton_, &QPushButton::clicked, this, [this] {
        // 停在最后一次预览对应的帧上，让操作者先检查构图，再按录入。
        stopCamera();
        faces_.clear();
        renderPreview();
        fileName_->setText(QStringLiteral("摄像头截帧"));
        showStatus(QStringLiteral("已截取一帧"), QStringLiteral("输入名称后点「录入当前图片」"), neutral);
    });
}

DemoWindow::~DemoWindow() = default;

void DemoWindow::loadPreview(cv::Mat image) {
    cv::Mat rgb;
    cv::cvtColor(image, rgb, cv::COLOR_BGR2RGB);
    // QImage 视图引用临时 cv::Mat 的像素。复制后，Qt 可在下一次绘制事件继续使用。
    previewImage_ = QImage(rgb.data, rgb.cols, rgb.rows, static_cast<int>(rgb.step),
                          QImage::Format_RGB888).copy();
    image_ = std::move(image);
}

void DemoWindow::openImage(const QString& path) {
    if (camera_) {
        stopCamera();
    }
    faces_.clear();
    try {
        loadPreview(loadImage(path));
        fileName_->setText(QFileInfo(path).fileName());
        showStatus(QStringLiteral("已载入图片"), QStringLiteral("可录入或验证当前图片"), neutral);
    } catch (const std::exception& error) {
        image_.release();
        previewImage_ = QImage();
        fileName_->setText(QStringLiteral("尚未选择有效文件"));
        showStatus(QStringLiteral("图片读取失败"), QString::fromUtf8(error.what()), denied);
    }
    enrollButton_->setEnabled(!image_.empty());
    checkButton_->setEnabled(!image_.empty());
    renderPreview();
}

void DemoWindow::enroll() {
    faces_.clear();
    try {
        const QString label = name_->text().trimmed();
        faces_ = {service_.enroll(label, image_)};
        faceColor_ = QColor(passed);
        showStatus(QStringLiteral("录入成功"), QStringLiteral("已保存 %1 的人脸特征").arg(label), passed);
        logEvent(QStringLiteral("已录入 %1").arg(label));
    } catch (const std::exception& error) {
        showStatus(QStringLiteral("录入失败"), QString::fromUtf8(error.what()), denied);
    }
    renderPreview();
}

void DemoWindow::check() {
    faces_.clear();
    try {
        QElapsedTimer timer;
        timer.start();
        const auto result = service_.check(image_);
        const double elapsedMs = timer.nsecsElapsed() / 1e6;
        displayCheck(result, elapsedMs);

        if (result.decision == Decision::Authorized && !combinedVerification_->isChecked()) {
            recordSimulatedUnlock(QStringLiteral("图片人脸"), result.name);
        } else if (result.decision != Decision::Authorized) {
            logEvent(statusText(result.decision));
        }
    } catch (const std::exception& error) {
        showStatus(QStringLiteral("验证失败"), QString::fromUtf8(error.what()), denied);
    }
    renderPreview();
}

void DemoWindow::setFingerprintPort(const QString& port) {
    fingerprintPort_ = port;
}

void DemoWindow::enableServoPwm(const QString& chipPath, unsigned channel) {
    if (servo_) {
        return;
    }
    servo_ = std::make_unique<PwmServo>(chipPath, channel);
    relockTimer_->stop();
    latchStatus_->setText(QStringLiteral("PWM 锁舌 · 回锁指令已发送"));
    latchFeedbackStatus_->setVisible(true);
}

void DemoWindow::enableLatchFeedback(LatchFeedback::Reader reader) {
    latchFeedback_.emplace(std::move(reader));
    lastFeedbackState_.reset();
    latchFeedback_.value().relockSent();
    pollLatchFeedback();
    latchFeedbackTimer_->start();
}

void DemoWindow::openFingerprint() {
    if (combinedVerification_->isChecked()) {
        return;
    }
    FingerprintDialog dialog(fingerprintPort_, this);
    connect(&dialog, &FingerprintDialog::verified, this, [this](int slot, bool simulation) {
        const auto method = simulation ? QStringLiteral("指纹（演练）") : QStringLiteral("指纹");
        const auto subject = QStringLiteral("槽位 %1").arg(slot);
        const auto description = QStringLiteral("%1 · %2").arg(method, subject);
        if (simulation) {
            recordSimulatedUnlock(method, subject, description);
        } else {
            requestUnlock(method, subject, description);
        }
    });
    dialog.exec();
    fingerprintPort_ = dialog.port();
}

void DemoWindow::manageTemplates() {
    QDialog dialog(this);
    dialog.setObjectName("templateDialog");
    dialog.setWindowTitle(QStringLiteral("人脸模板"));
    dialog.resize(440, 300);
    auto* layout = new QVBoxLayout(&dialog);
    auto* summary = new QLabel(&dialog);
    summary->setWordWrap(true);
    summary->setTextFormat(Qt::PlainText);
    layout->addWidget(summary);
    auto* list = new QListWidget(&dialog);
    list->setObjectName("templateList");
    layout->addWidget(list);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    auto* close = buttons->button(QDialogButtonBox::Close);
    close->setText(QStringLiteral("关闭"));
    close->setDefault(true);
    auto* remove = buttons->addButton(QStringLiteral("删除所选"), QDialogButtonBox::ActionRole);
    remove->setObjectName("removeTemplateButton");
    remove->setAutoDefault(false);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(list, &QListWidget::currentItemChanged, &dialog,
            [remove](QListWidgetItem* current) { remove->setEnabled(current != nullptr); });

    const auto refresh = [this, list, summary, remove] {
        list->clear();
        remove->setEnabled(false);
        try {
            const auto names = service_.templateNames();
            list->addItems(names);
            summary->setText(names.isEmpty() ? QStringLiteral("尚未录入人脸模板")
                                            : QStringLiteral("已录入 %1 个模板").arg(names.size()));
        } catch (const std::exception& error) {
            summary->setText(QStringLiteral("读取失败：%1").arg(QString::fromUtf8(error.what())));
        }
    };

    connect(remove, &QPushButton::clicked, &dialog, [this, &dialog, list, summary, refresh] {
        if (!list->currentItem()) {
            return;
        }
        // 先复制选中的名称。确认框默认取消；只有明确点「删除」才进入写入路径。
        const QString name = list->currentItem()->text();
        QMessageBox confirm(QMessageBox::Question, QStringLiteral("删除人脸模板"),
                            QStringLiteral("确定删除「%1」的人脸模板吗？").arg(name),
                            QMessageBox::Yes | QMessageBox::Cancel, &dialog);
        confirm.setObjectName("removeTemplateConfirmation");
        confirm.setTextFormat(Qt::PlainText);
        confirm.setDefaultButton(QMessageBox::Cancel);
        confirm.setEscapeButton(QMessageBox::Cancel);
        confirm.button(QMessageBox::Yes)->setText(QStringLiteral("删除"));
        confirm.button(QMessageBox::Cancel)->setText(QStringLiteral("取消"));
        if (confirm.exec() != QMessageBox::Yes) {
            return;
        }

        try {
            const bool removed = removeManagedTemplate(name);
            refresh();
            if (!removed) {
                summary->setText(QStringLiteral("该模板已不存在，列表已刷新"));
            }
        } catch (const std::exception& error) {
            summary->setText(QStringLiteral("删除失败：%1").arg(QString::fromUtf8(error.what())));
        }
    });
    refresh();
    dialog.exec();
}

bool DemoWindow::removeManagedTemplate(const QString& name) {
    const bool removed = camera_ ? camera_->removeTemplate(service_, name) : service_.removeTemplate(name);
    if (!removed) {
        return false;
    }
    logEvent(QStringLiteral("已删除模板 | %1").arg(name));
    if (pendingFaceName_ == name) {
        cancelFaceGestureChallenge();
    }
    if (!camera_ || lastLiveName_ == name) {
        faces_.clear();
        renderPreview();
        showStatus(camera_ ? QStringLiteral("等待授权") : QStringLiteral("模板已删除"), name, neutral);
        if (camera_) {
            lastLiveName_ = QString{};
            emit liveEvent(QStringLiteral("decision=waiting"));
        }
    }
    return true;
}

void DemoWindow::startCamera(const QString& device, int measureSeconds) {
    if (camera_) {
        return;
    }
    try {
        camera_ = std::make_unique<CameraSession>(device, modelsDir_, databasePath_, measureSeconds);
    } catch (const std::exception& error) {
        const auto message = QString::fromUtf8(error.what());
        showStatus(QStringLiteral("摄像头启动失败"), message, denied);
        emit cameraFailed(message);
        return;
    }
    image_.release();
    previewImage_ = QImage();
    faces_.clear();
    lastLiveName_.reset();
    cameraPath_->setText(device);
    cameraPath_->setEnabled(false);
    chooseImageButton_->setEnabled(false);
    enrollButton_->setEnabled(false);
    checkButton_->setEnabled(false);
    cameraButton_->setText(QStringLiteral("停止摄像头"));
    fileName_->setText(QFileInfo(device).fileName());
    showStatus(QStringLiteral("等待摄像头画面"), QStringLiteral("后台正在启动采集和识别"), neutral);
    metrics_->setText(QStringLiteral("预热 2 秒，随后测量 %1 秒").arg(measureSeconds));
    metrics_->show();
    renderPreview();
    cameraTimer_->start();
}

void DemoWindow::stopCamera() {
    cancelFaceGestureChallenge();
    cameraTimer_->stop();
    camera_.reset();
    cameraPath_->setEnabled(true);
    chooseImageButton_->setEnabled(true);
    enrollButton_->setEnabled(!image_.empty());
    checkButton_->setEnabled(!image_.empty());
    snapshotButton_->setEnabled(false);
    cameraButton_->setText(QStringLiteral("启动摄像头"));
}

void DemoWindow::pollCamera() {
    auto update = camera_->takeLatest();
    if (!update.error.isEmpty()) {
        stopCamera();
        metrics_->hide();
        showStatus(QStringLiteral("摄像头已停止"), update.error, denied);
        emit cameraFailed(update.error);
        return;
    }
    if (update.latest) {
        auto& result = *update.latest;
        // 预览与人脸框来自同一帧。Qt 主线程只消费最新结果，避免窗口积压刷新事件。
        loadPreview(std::move(result.image));
        displayLiveCheck(result.check, result.decision, result.latencyMs);
        const auto& name = result.decision.authorizedName;
        if (!lastLiveName_ || lastLiveName_->isEmpty() != name.isEmpty()) {
            emit liveEvent(name.isEmpty() ? QStringLiteral("decision=waiting")
                                          : QStringLiteral("decision=authorized"));
        }
        lastLiveName_ = name;
        snapshotButton_->setEnabled(true);
        renderPreview();
        camera_->recordPreview(result.capturedAt);
    }
    // 开锁只消费状态机的触发事件，保持绿色状态的后续帧不会反复写记录。
    for (const auto& face : update.unlocks) {
        // Qt 可以晚于识别线程拿到事件，切换模式前的旧帧不能进入当前策略。
        if (face.capturedAt < policyStartedAt_) {
            continue;
        }
        if (combinedVerification_->isChecked()) {
            const auto now = FaceGestureChallenge::Clock::now();
            if (const auto ticket = faceGestureChallenge_.start(face.capturedAt, now)) {
                pendingFaceName_ = face.name;
                combinedStatus_->setText(QStringLiteral("人脸已确认 · %1 · 请在 30 秒内完成手势").arg(face.name));
                const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(ticket->expiresAt - now);
                faceGestureTimer_->start(static_cast<int>(remaining.count()));
            }
            continue;
        }
        if (requestUnlock(QStringLiteral("实时人脸"), face.name)) {
            emit liveEvent(servo_ ? QStringLiteral("pwm_unlock_sent") : QStringLiteral("simulated_unlock"));
        }
    }
    if (const auto metrics = camera_->finishMeasurement()) {
        metrics_->setText(QStringLiteral("输入 %1 FPS · 处理 %2 FPS\n识别 P95 %3 ms · Qt 预览 P95 %4 ms")
            .arg(metricText(*metrics, "input_fps"), metricText(*metrics, "processing_fps"),
                 metricText(*metrics, "processing_p95_ms"), metricText(*metrics, "qt_preview_p95_ms")));
        emit measurementReady(*metrics);
    }
}

void DemoWindow::displayCheck(const CheckResult& result, double elapsedMs) {
    const bool authorized = result.decision == Decision::Authorized;
    faces_ = result.faces;
    faceColor_ = QColor(authorized ? passed : denied);
    const QString similarity = result.similarity
        ? QString::number(*result.similarity, 'f', 3) : QStringLiteral("—");
    QString details = QStringLiteral("人脸 %1 张 · 相似度 %2 · 阈值 %3 · %4 ms")
        .arg(static_cast<qulonglong>(faces_.size())).arg(similarity)
        .arg(FaceService::matchThreshold, 0, 'f', 2).arg(elapsedMs, 0, 'f', 1);
    if (authorized) {
        details.prepend(result.name + QStringLiteral(" · "));
    }
    const auto title = authorized && combinedVerification_->isChecked()
        ? QStringLiteral("图片人脸匹配 · 联合验证请使用实时摄像头") : statusText(result.decision);
    showStatus(title, details, authorized ? passed : denied);
}

void DemoWindow::displayLiveCheck(const CheckResult& result, const LiveDecision::State& decision,
                                  double elapsedMs) {
    // 主状态跟随连续帧判决，诊断行保留本帧分数，便于观察阈值附近的波动。
    const bool authorized = !decision.authorizedName.isEmpty();
    faces_ = result.faces;
    faceColor_ = QColor(authorized ? passed : neutral);
    QString context;
    if (authorized) {
        context = QStringLiteral("已确认 %1").arg(decision.authorizedName);
    } else if (decision.matchingFrames > 0) {
        context = QStringLiteral("正在确认 %1/%2").arg(decision.matchingFrames).arg(LiveDecision::requiredMatches);
    } else {
        context = statusText(result.decision);
    }
    const QString similarity = result.similarity
        ? QString::number(*result.similarity, 'f', 3) : QStringLiteral("—");
    const QString details = QStringLiteral("%1 · 人脸 %2 张 · 本帧相似度 %3 · 阈值 %4 · %5 ms")
        .arg(context).arg(static_cast<qulonglong>(faces_.size())).arg(similarity)
        .arg(FaceService::matchThreshold, 0, 'f', 2).arg(elapsedMs, 0, 'f', 1);
    showStatus(authorized ? QStringLiteral("实时授权通过") : QStringLiteral("等待授权"),
               details, authorized ? passed : neutral);
}

void DemoWindow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    renderPreview();
}

void DemoWindow::renderPreview() {
    if (previewImage_.isNull()) {
        preview_->setText(QStringLiteral("选择一张图片开始"));
        return;
    }
    QImage overlay = previewImage_.copy();
    {
        QPainter painter(&overlay);
        painter.setPen(QPen(faceColor_, qMax(2, overlay.width() / 140)));
        for (const auto& face : faces_) {
            painter.drawRect(QRect(face.x, face.y, face.width, face.height));
        }
    }
    preview_->setPixmap(QPixmap::fromImage(overlay).scaled(
        preview_->size() - QSize(18, 18), Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

void DemoWindow::showStatus(const QString& title, const QString& details, const char* color) {
    status_->setText(title);
    const QString style = QStringLiteral("font-weight: bold; font-size: 18px; color: %1;")
        .arg(QString::fromLatin1(color));
    if (status_->styleSheet() != style) {
        status_->setStyleSheet(style);
    }
    details_->setText(details);
}

void DemoWindow::logEvent(const QString& message) {
    events_->appendPlainText(QTime::currentTime().toString(QStringLiteral("HH:mm:ss  ")) + message);
}

void DemoWindow::cancelFaceGestureChallenge() {
    faceGestureChallenge_.cancel();
    faceGestureTimer_->stop();
    pendingFaceName_.clear();
    combinedStatus_->setText(QStringLiteral("等待已录入的人脸进入画面"));
}

void DemoWindow::recordSimulatedUnlock(const QString& method, const QString& subject,
                                       const QString& sessionSubject) {
    // 图片和指纹演练仍走模拟路径，启用 PWM 时也不会改变实体舵机状态。
    if (!servo_) {
        latchStatus_->setText(QStringLiteral("模拟锁舌 · 已解锁 · 3 秒后自动回锁"));
        relockTimer_->start(unlockHoldMs);
    }
    logEvent(QStringLiteral("模拟开锁 | %1").arg(sessionSubject.isEmpty() ? subject : sessionSubject));
    appendUnlockHistory(method, subject);
}

bool DemoWindow::requestUnlock(const QString& method, const QString& subject, const QString& sessionSubject) {
    if (!servo_) {
        recordSimulatedUnlock(method, subject, sessionSubject);
        return true;
    }
    if (servoFault_) {
        return false;
    }
    try {
        servo_->unlock();
    } catch (const std::exception& error) {
        servoFault_ = true;
        latchStatus_->setText(QStringLiteral("PWM 锁舌 · 开锁指令失败，等待重试回锁"));
        logEvent(QStringLiteral("PWM 开锁失败 · %1").arg(QString::fromUtf8(error.what())));
        relockTimer_->start(relockRetryMs);
        return false;
    }
    // 硬件会持续输出当前脉宽。Qt 的定时器只决定何时切回回锁脉宽。
    relockTimer_->start(unlockHoldMs);
    if (latchFeedback_) {
        latchFeedback_->unlockSent();
        pollLatchFeedback();
    }
    latchStatus_->setText(QStringLiteral("PWM 锁舌 · 开锁指令已发送 · 3 秒后回锁"));
    logEvent(QStringLiteral("PWM 开锁指令已发送 | %1").arg(sessionSubject.isEmpty() ? subject : sessionSubject));
    appendUnlockHistory(method + QStringLiteral("（PWM 指令）"), subject);
    return true;
}

void DemoWindow::relock() {
    if (!servo_) {
        latchStatus_->setText(QStringLiteral("模拟锁舌 · 已上锁"));
        return;
    }
    try {
        servo_->lock();
        servoFault_ = false;
        latchStatus_->setText(QStringLiteral("PWM 锁舌 · 回锁指令已发送"));
        logEvent(QStringLiteral("PWM 回锁指令已发送"));
        if (latchFeedback_) {
            latchFeedback_->relockSent();
            pollLatchFeedback();
        }
    } catch (const std::exception& error) {
        if (!servoFault_) {
            logEvent(QStringLiteral("PWM 回锁失败 · %1").arg(QString::fromUtf8(error.what())));
        }
        // 回锁写入失败时禁止再次开锁，直到回锁命令重新写入成功。
        servoFault_ = true;
        latchStatus_->setText(QStringLiteral("PWM 锁舌 · 回锁失败，正在重试"));
        relockTimer_->start(relockRetryMs);
    }
}

void DemoWindow::pollLatchFeedback() {
    using State = LatchFeedback::State;
    const auto state = latchFeedback_->poll();
    if (lastFeedbackState_ == state) {
        return;
    }
    lastFeedbackState_ = state;

    QString text;
    switch (state) {
    case State::Checking: text = QStringLiteral("正在确认限位触点"); break;
    case State::Released: text = QStringLiteral("触点释放 · 锁舌未在回锁位置"); break;
    case State::Waiting: text = QStringLiteral("等待锁舌到位"); break;
    case State::AtLimit: text = QStringLiteral("锁舌已到位"); break;
    case State::TimedOut: text = QStringLiteral("回锁超时 · 未确认锁舌到位"); break;
    case State::DidNotRelease: text = QStringLiteral("限位触点未释放 · 无法确认本轮回锁"); break;
    case State::ReadError: text = QStringLiteral("GPIO 读取失败 · 到位状态未知"); break;
    }
    latchFeedbackStatus_->setText(text);
    if (state == State::AtLimit || state == State::TimedOut ||
        state == State::DidNotRelease || state == State::ReadError) {
        logEvent(QStringLiteral("锁舌反馈 · %1").arg(text));
    }
}

void DemoWindow::appendUnlockHistory(const QString& method, const QString& subject) {
    try {
        unlockHistory_.append({QDateTime::currentDateTimeUtc(), method, subject});
    } catch (const std::exception& error) {
        logEvent(QStringLiteral("开锁历史保存失败 · %1").arg(QString::fromUtf8(error.what())));
    }
}

void DemoWindow::openUnlockHistory() {
    QDialog dialog(this);
    dialog.setObjectName("unlockHistoryDialog");
    dialog.setWindowTitle(QStringLiteral("模拟开锁历史"));
    dialog.resize(680, 390);
    auto* layout = new QVBoxLayout(&dialog);
    auto* filters = new QHBoxLayout;
    auto* methodLabel = new QLabel(QStringLiteral("授权方式"));
    auto* method = new QComboBox;
    method->setObjectName("unlockHistoryMethod");
    method->addItem(QStringLiteral("全部方式"));
    methodLabel->setBuddy(method);
    filters->addWidget(methodLabel);
    filters->addWidget(method);
    filters->addStretch();
    layout->addLayout(filters);

    auto* table = new QTableWidget(0, 3);
    table->setObjectName("unlockHistoryTable");
    table->setHorizontalHeaderLabels({QStringLiteral("时间"), QStringLiteral("授权方式"),
                                      QStringLiteral("姓名或凭据")});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->horizontalHeader()->setStretchLastSection(true);
    table->verticalHeader()->hide();
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    layout->addWidget(table);
    auto* summary = new QLabel;
    summary->setTextFormat(Qt::PlainText);
    summary->setWordWrap(true);
    layout->addWidget(summary);
    auto* close = new QDialogButtonBox;
    close->addButton(QStringLiteral("关闭"), QDialogButtonBox::RejectRole);
    connect(close, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(close);

    QVector<UnlockEntry> entries;
    try {
        entries = unlockHistory_.newestFirst();
    } catch (const std::exception& error) {
        summary->setText(QStringLiteral("读取失败 · %1").arg(QString::fromUtf8(error.what())));
        dialog.exec();
        return;
    }
    for (const auto& entry : entries) {
        if (method->findData(entry.method) == -1) {
            method->addItem(entry.method, entry.method);
        }
    }
    const auto showEntries = [&] {
        const auto filter = method->currentData().toString();
        table->setRowCount(0);
        for (const auto& entry : entries) {
            if (method->currentIndex() != 0 && filter != entry.method) {
                continue;
            }
            const auto row = table->rowCount();
            table->insertRow(row);
            table->setItem(row, 0, new QTableWidgetItem(entry.time.toLocalTime().toString("yyyy-MM-dd HH:mm:ss")));
            table->setItem(row, 1, new QTableWidgetItem(entry.method));
            table->setItem(row, 2, new QTableWidgetItem(entry.subject));
        }
        summary->setText(table->rowCount() == 0 ? QStringLiteral("暂无模拟开锁记录")
                                                : QStringLiteral("显示 %1 条").arg(table->rowCount()));
    };
    connect(method, qOverload<int>(&QComboBox::currentIndexChanged), &dialog, showEntries);
    showEntries();
    dialog.exec();
}

void DemoWindow::openGesture() {
    const bool combined = combinedVerification_->isChecked();
    const auto ticket = faceGestureChallenge_.active(FaceGestureChallenge::Clock::now());
    if (combined && !ticket) {
        combinedStatus_->setText(QStringLiteral("请先让已录入的人脸进入摄像头画面"));
        return;
    }
    GestureDialog dialog(this);
    connect(&dialog, &GestureDialog::verified, this, [this, &dialog, combined, ticket] {
        const auto sequence = QStringLiteral("左 → 上 → 顺时针");
        if (combined) {
            // Qt 的模态对话框仍会处理摄像头和计时事件。旧对话框不能消费新一轮资格。
            if (!faceGestureChallenge_.consume(ticket->id, FaceGestureChallenge::Clock::now())) {
                dialog.setVerificationFeedback(QStringLiteral("本轮联合验证已失效，请关闭窗口后重新开始"));
                return;
            }
            const auto name = pendingFaceName_;
            cancelFaceGestureChallenge();
            if (requestUnlock(QStringLiteral("人脸＋手势"), name, QStringLiteral("人脸＋手势 · %1").arg(name))) {
                combinedStatus_->setText(QStringLiteral("联合验证通过 · %1").arg(name));
                emit liveEvent(servo_ ? QStringLiteral("pwm_unlock_sent") : QStringLiteral("simulated_unlock"));
                dialog.accept();
            } else {
                dialog.setVerificationFeedback(QStringLiteral("联合验证通过 · PWM 开锁指令未成功"));
            }
            return;
        }
        const bool sent = requestUnlock(QStringLiteral("手势"), sequence, QStringLiteral("手势 · %1").arg(sequence));
        if (servo_) {
            dialog.setVerificationFeedback(sent ? QStringLiteral("授权通过 · PWM 开锁指令已发送")
                                                : QStringLiteral("授权通过 · PWM 开锁指令未成功"));
        }
    });
    dialog.exec();
    const auto current = faceGestureChallenge_.active(FaceGestureChallenge::Clock::now());
    if (combined && current && current->id == ticket->id) {
        cancelFaceGestureChallenge();
    }
}
