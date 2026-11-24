#pragma once

#include "face_service.h"
#include "live_decision.h"

#include <QJsonObject>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

class CameraSession {
public:
    using Clock = std::chrono::steady_clock;

    struct Result {
        cv::Mat image;
        Clock::time_point capturedAt;
        CheckResult check;
        LiveDecision::State decision;
        double latencyMs;
    };

    struct FaceAuthorization {
        QString name;
        Clock::time_point capturedAt;
    };

    struct Update {
        std::optional<Result> latest;
        std::vector<FaceAuthorization> unlocks;
        QString error;
    };

    CameraSession(const QString& device, const QString& modelsDir, const QString& databasePath,
                  int measureSeconds);
    ~CameraSession();
    CameraSession(const CameraSession&) = delete;
    CameraSession& operator=(const CameraSession&) = delete;

    Update takeLatest();
    bool removeTemplate(FaceService& service, const QString& name);
    void recordPreview(Clock::time_point capturedAt);
    std::optional<QJsonObject> finishMeasurement();

private:
    struct Frame {
        cv::Mat image;
        Clock::time_point capturedAt;
        std::uint64_t number;
    };

    void captureFrames(const QString& device);
    void processFrames(const QString& modelsDir, const QString& databasePath);
    void fail(const QString& error);
    void stopAndJoin();
    bool inWindow(Clock::time_point now) const;

    const int measureSeconds_;
    std::mutex checkMutex_;
    LiveDecision decision_;
    std::mutex mutex_;
    std::condition_variable ready_;
    bool stopping_ = false;
    QString error_;
    // 两级各留一个最新项，避免采集或 Qt 画框暂时比下一阶段快时积攒历史帧。
    std::optional<Frame> pending_;
    std::optional<Result> latest_;
    // 最新预览可以覆盖旧画面，已触发的开锁事件单独交给 Qt。
    std::vector<FaceAuthorization> pendingUnlocks_;
    std::optional<Clock::time_point> windowStart_;
    Clock::time_point windowEnd_;
    bool measurementReported_ = false;
    int width_ = 0;
    int height_ = 0;
    int captured_ = 0;
    int overwritten_ = 0;
    int authorized_ = 0;
    int unknown_ = 0;
    int noFace_ = 0;
    int otherDenials_ = 0;
    std::vector<double> processingLatencies_;
    std::vector<double> previewLatencies_;
    std::thread captureThread_;
    std::thread processingThread_;
};
