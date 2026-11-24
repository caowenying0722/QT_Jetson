#pragma once

#include "face_gesture_challenge.h"
#include "face_service.h"
#include "latch_feedback.h"
#include "live_decision.h"
#include "unlock_history.h"

#include <QColor>
#include <QImage>
#include <QJsonObject>
#include <QWidget>
#include <memory>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QResizeEvent;
class QTimer;
class CameraSession;
class PwmServo;

class DemoWindow : public QWidget {
    Q_OBJECT

public:
    DemoWindow(const QString& modelsDir, const QString& databasePath);
    ~DemoWindow() override;
    void openImage(const QString& path);
    void startCamera(const QString& device, int measureSeconds = 20);
    void setFingerprintPort(const QString& port);
    void enableServoPwm(const QString& chipPath, unsigned channel);
    void enableLatchFeedback(LatchFeedback::Reader reader);

signals:
    void measurementReady(const QJsonObject& metrics);
    void cameraFailed(const QString& error);
    void liveEvent(const QString& event);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void enroll();
    void check();
    void manageTemplates();
    bool removeManagedTemplate(const QString& name);
    void openFingerprint();
    void openGesture();
    void openUnlockHistory();
    void stopCamera();
    void pollCamera();
    void loadPreview(cv::Mat image);
    void displayCheck(const CheckResult& result, double elapsedMs);
    void displayLiveCheck(const CheckResult& result, const LiveDecision::State& decision, double elapsedMs);
    void renderPreview();
    void showStatus(const QString& title, const QString& details, const char* color);
    void logEvent(const QString& message);
    void cancelFaceGestureChallenge();
    bool requestUnlock(const QString& method, const QString& subject, const QString& sessionSubject = {});
    void relock();
    void pollLatchFeedback();
    void appendUnlockHistory(const QString& method, const QString& subject);
    void recordSimulatedUnlock(const QString& method, const QString& subject,
                               const QString& sessionSubject = {});

    const QString modelsDir_;
    const QString databasePath_;
    QString fingerprintPort_;
    FaceService service_;
    UnlockHistory unlockHistory_;
    FaceGestureChallenge faceGestureChallenge_;
    FaceGestureChallenge::Clock::time_point policyStartedAt_;
    QString pendingFaceName_;
    std::unique_ptr<CameraSession> camera_;
    std::unique_ptr<PwmServo> servo_;
    std::optional<LatchFeedback> latchFeedback_;
    std::optional<LatchFeedback::State> lastFeedbackState_;
    bool servoFault_ = false;
    std::optional<QString> lastLiveName_;
    cv::Mat image_;
    QImage previewImage_;
    std::vector<cv::Rect> faces_;
    QColor faceColor_;
    QLabel* preview_;
    QLabel* fileName_;
    QLabel* status_;
    QLabel* details_;
    QLineEdit* name_;
    QLineEdit* cameraPath_;
    QPushButton* chooseImageButton_;
    QPushButton* cameraButton_;
    QPushButton* snapshotButton_;
    QPushButton* enrollButton_;
    QPushButton* checkButton_;
    QPlainTextEdit* events_;
    QLabel* metrics_;
    QCheckBox* combinedVerification_;
    QLabel* combinedStatus_;
    QLabel* latchStatus_;
    QLabel* latchFeedbackStatus_;
    QTimer* cameraTimer_;
    QTimer* faceGestureTimer_;
    QTimer* relockTimer_;
    QTimer* latchFeedbackTimer_;
};
