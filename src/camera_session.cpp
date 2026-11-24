#include "camera_session.h"

#include <opencv2/videoio.hpp>

#include <algorithm>
#include <cmath>
#include <exception>
#include <utility>

namespace {
using namespace std::chrono_literals;
constexpr auto warmup = 2s;

double elapsedMs(CameraSession::Clock::time_point start, CameraSession::Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

QJsonValue percentile95(std::vector<double>& samples) {
    if (samples.empty()) {
        return QJsonValue::Null;
    }
    std::sort(samples.begin(), samples.end());
    const auto rank = static_cast<std::size_t>(std::ceil(0.95 * samples.size()));
    return samples[rank - 1];
}
}

CameraSession::CameraSession(const QString& device, const QString& modelsDir,
                             const QString& databasePath, int measureSeconds)
    : measureSeconds_(measureSeconds) {
    try {
        captureThread_ = std::thread(&CameraSession::captureFrames, this, device);
        processingThread_ = std::thread(&CameraSession::processFrames, this, modelsDir, databasePath);
    } catch (...) {
        stopAndJoin();
        throw;
    }
}

CameraSession::~CameraSession() {
    stopAndJoin();
}

void CameraSession::captureFrames(const QString& device) {
    try {
        // 摄像头始终由采集线程持有，读帧和释放都在同一线程完成。
        cv::VideoCapture camera(device.toStdString(), cv::CAP_V4L2);
        if (!camera.isOpened()) {
            fail(QStringLiteral("无法打开摄像头：%1").arg(device));
            return;
        }
        camera.set(cv::CAP_PROP_FRAME_WIDTH, 640);
        camera.set(cv::CAP_PROP_FRAME_HEIGHT, 480);
        camera.set(cv::CAP_PROP_FPS, 30);
        camera.set(cv::CAP_PROP_BUFFERSIZE, 1);

        std::uint64_t frameNumber = 0;
        for (;;) {
            cv::Mat image;
            if (!camera.read(image) || image.empty()) {
                fail(QStringLiteral("摄像头读帧失败，请检查 USB 连接或设备占用"));
                return;
            }
            const auto capturedAt = Clock::now();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (stopping_) {
                    return;
                }
                if (!windowStart_) {
                    width_ = image.cols;
                    height_ = image.rows;
                    windowStart_ = capturedAt + warmup;
                    windowEnd_ = *windowStart_ + std::chrono::seconds(measureSeconds_);
                }
                if (inWindow(capturedAt)) {
                    ++captured_;
                    if (pending_ && inWindow(pending_->capturedAt)) {
                        ++overwritten_;
                    }
                }
                // 每次 read 写入新的 Mat。移动所有权后，下一次采集不会改动推理正在读的像素。
                pending_ = Frame{std::move(image), capturedAt, ++frameNumber};
            }
            ready_.notify_one();
        }
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
    }
}

void CameraSession::processFrames(const QString& modelsDir, const QString& databasePath) {
    try {
        // Net::setInput() 和 forward() 会改内部状态，所以推理线程有独立的模型实例。
        FaceService service(modelsDir, databasePath);
        for (;;) {
            Frame frame;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                ready_.wait(lock, [this] { return stopping_ || pending_.has_value(); });
                if (stopping_) {
                    return;
                }
                frame = std::move(*pending_);
                pending_.reset();
            }
            // 推理和删模板串行。这里不持有采集缓冲的锁，摄像头可以继续写入下一帧。
            std::lock_guard<std::mutex> checkLock(checkMutex_);
            auto check = service.check(frame.image);
            // 每个推理结果先进入状态机。Qt 即使跳过某次绘制，也不会改变三帧确认的结果。
            auto state = decision_.observe(check.decision == Decision::Authorized ? check.name : QString{},
                                           !check.faces.empty(), frame.number, frame.capturedAt);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (stopping_) {
                    return;
                }
                const auto done = Clock::now();
                const double latency = elapsedMs(frame.capturedAt, done);
                if (inWindow(done)) {
                    processingLatencies_.push_back(latency);
                    switch (check.decision) {
                    case Decision::Authorized: ++authorized_; break;
                    case Decision::Unknown: ++unknown_; break;
                    case Decision::NoFace: ++noFace_; break;
                    default: ++otherDenials_; break;
                    }
                }
                if (state.shouldUnlock) {
                    pendingUnlocks_.push_back({state.authorizedName, frame.capturedAt});
                }
                latest_ = Result{std::move(frame.image), frame.capturedAt, std::move(check),
                                 std::move(state), latency};
            }
        }
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
    }
}

bool CameraSession::removeTemplate(FaceService& service, const QString& name) {
    std::lock_guard<std::mutex> checkLock(checkMutex_);
    if (!service.removeTemplate(name)) {
        return false;
    }
    // 文件提交后撤销该模板的候选和授权，丢弃尚未展示的旧结果及待交付事件。
    decision_.forgetTemplate(name);
    std::lock_guard<std::mutex> lock(mutex_);
    if (latest_ && (latest_->decision.authorizedName == name || latest_->check.name == name)) {
        latest_.reset();
    }
    pendingUnlocks_.erase(std::remove_if(pendingUnlocks_.begin(), pendingUnlocks_.end(),
                                        [&name](const FaceAuthorization& event) { return event.name == name; }),
                          pendingUnlocks_.end());
    return true;
}

CameraSession::Update CameraSession::takeLatest() {
    std::lock_guard<std::mutex> lock(mutex_);
    Update update{std::move(latest_), std::exchange(pendingUnlocks_, {}), error_};
    latest_.reset();
    return update;
}

void CameraSession::recordPreview(Clock::time_point capturedAt) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = Clock::now();
    if (inWindow(now)) {
        previewLatencies_.push_back(elapsedMs(capturedAt, now));
    }
}

std::optional<QJsonObject> CameraSession::finishMeasurement() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (measurementReported_ || !windowStart_ || Clock::now() < windowEnd_ || stopping_) {
        return std::nullopt;
    }
    measurementReported_ = true;
    const auto seconds = static_cast<double>(measureSeconds_);
    const auto processed = static_cast<int>(processingLatencies_.size());
    // 分母是同一段预热后的墙钟时间，处理帧按结果就绪的时间计数。
    return QJsonObject{
        {"seconds", measureSeconds_}, {"width", width_}, {"height", height_},
        {"input_frames", captured_}, {"processed_frames", processed},
        {"overwritten_frames", overwritten_}, {"preview_frames", static_cast<int>(previewLatencies_.size())},
        {"input_fps", captured_ / seconds}, {"processing_fps", processed / seconds},
        {"processing_p95_ms", percentile95(processingLatencies_)},
        {"qt_preview_p95_ms", percentile95(previewLatencies_)},
        {"authorized_frames", authorized_}, {"unknown_frames", unknown_},
        {"no_face_frames", noFace_}, {"other_denial_frames", otherDenials_}
    };
}

void CameraSession::fail(const QString& error) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!stopping_) {
            error_ = error;
            stopping_ = true;
        }
    }
    ready_.notify_all();
}

void CameraSession::stopAndJoin() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    ready_.notify_all();
    if (processingThread_.joinable()) {
        processingThread_.join();
    }
    if (captureThread_.joinable()) {
        captureThread_.join();
    }
}

bool CameraSession::inWindow(Clock::time_point now) const {
    return windowStart_ && now >= *windowStart_ && now < windowEnd_;
}
