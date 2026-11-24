#include "face_service.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <opencv2/imgcodecs.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace {
constexpr int embeddingSize = 128;
constexpr int databaseVersion = 1;
constexpr char embeddingModel[] = "openface-nn4.small2.v1-rgb-crop";

[[noreturn]] void fail(const QString& message) {
    throw std::runtime_error(message.toStdString());
}
}

cv::Mat loadImage(const QString& path) {
    // Qt 负责打开文件路径，OpenCV 只解码字节。这样文件名可以包含中文。
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        fail(QStringLiteral("无法读取图片：%1").arg(path));
    }
    const QByteArray bytes = file.readAll();
    if (bytes.isEmpty()) {
        fail(QStringLiteral("图片文件为空：%1").arg(path));
    }
    const auto* begin = reinterpret_cast<const unsigned char*>(bytes.constData());
    cv::Mat image = cv::imdecode(std::vector<unsigned char>(begin, begin + bytes.size()),
                                cv::IMREAD_COLOR);
    if (image.empty()) {
        fail(QStringLiteral("无法解码图片：%1").arg(path));
    }
    return image;
}

const char* decisionCode(Decision decision) {
    switch (decision) {
    case Decision::Authorized: return "authorized";
    case Decision::Unknown: return "unknown";
    case Decision::NoEnrollment: return "no_enrollment";
    case Decision::NoFace: return "no_face";
    case Decision::MultipleFaces: return "multiple_faces";
    }
    return "invalid_decision";
}

FaceService::FaceService(const QString& modelsDir, const QString& databasePath)
    : databasePath_(QFileInfo(databasePath).absoluteFilePath()) {
    const QDir dir(modelsDir);
    const QString config = dir.filePath(QStringLiteral("deploy.prototxt"));
    const QString weights = dir.filePath(QStringLiteral("res10_300x300_ssd_iter_140000.caffemodel"));
    const QString openface = dir.filePath(QStringLiteral("nn4.small2.v1.t7"));
    for (const auto& path : {config, weights, openface}) {
        if (!QFileInfo(path).isReadable()) {
            fail(QStringLiteral("缺少可读的模型文件：%1").arg(path));
        }
    }

    detector_ = cv::dnn::readNetFromCaffe(config.toStdString(), weights.toStdString());
    recognizer_ = cv::dnn::readNetFromTorch(openface.toStdString());
    for (auto* net : {&detector_, &recognizer_}) {
        if (net->empty()) {
            fail(QStringLiteral("OpenCV 未能加载模型"));
        }
        net->setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
        net->setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
    }
}

std::vector<cv::Rect> FaceService::detect(const cv::Mat& image) {
    // SSD 模型接收 300×300 的 BGR 张量。输出的四个坐标是相对原图宽高的比例。
    detector_.setInput(cv::dnn::blobFromImage(image, 1.0, {300, 300}, {104, 177, 123}));
    const cv::Mat detections = detector_.forward();
    if (detections.dims != 4 || detections.size[3] != 7) {
        fail(QStringLiteral("人脸检测模型返回了意外的输出形状"));
    }
    const cv::Mat rows = detections.reshape(1, static_cast<int>(detections.total() / 7));
    std::vector<cv::Rect> faces;
    for (int i = 0; i < rows.rows; ++i) {
        const float* row = rows.ptr<float>(i);
        if (!(row[2] >= 0.60f) || !std::all_of(row + 3, row + 7, [](float value) {
                return std::isfinite(value);
            })) {
            continue;
        }
        const int left = cvRound(std::clamp(row[3], 0.0f, 1.0f) * image.cols);
        const int top = cvRound(std::clamp(row[4], 0.0f, 1.0f) * image.rows);
        const int right = cvRound(std::clamp(row[5], 0.0f, 1.0f) * image.cols);
        const int bottom = cvRound(std::clamp(row[6], 0.0f, 1.0f) * image.rows);
        if (right > left && bottom > top) {
            faces.emplace_back(left, top, right - left, bottom - top);
        }
    }
    return faces;
}

cv::Mat FaceService::embed(const cv::Mat& face) {
    // OpenFace 接收 RGB、人脸裁剪缩放至 96×96，像素缩放到 [0, 1]。
    // 录入和验证共用这个函数，保证两次计算的特征处在同一个空间。
    recognizer_.setInput(cv::dnn::blobFromImage(face, 1.0 / 255.0, {96, 96}, {}, true));
    const cv::Mat output = recognizer_.forward();
    const double length = cv::norm(output);
    if (output.type() != CV_32F || output.total() != embeddingSize ||
        !std::isfinite(length) || length < 1e-6) {
        fail(QStringLiteral("人脸识别模型没有输出有效的 128 维特征"));
    }
    // 单位向量的点积等于余弦相似度。这里生成独立内存，避免下次推理覆盖模板。
    return output.reshape(1, 1) / length;
}

cv::Rect FaceService::enroll(const QString& name, const cv::Mat& image) {
    const QString label = name.trimmed();
    if (label.isEmpty()) {
        fail(QStringLiteral("录入前请输入名字或代号"));
    }
    const auto faces = detect(image);
    if (faces.size() != 1) {
        fail(QStringLiteral("录入需要恰好一张人脸，当前检测到 %1 张")
                 .arg(static_cast<qulonglong>(faces.size())));
    }
    const cv::Mat feature = embed(image(faces.front()));
    const QDir parent = QFileInfo(databasePath_).absoluteDir();
    if (!parent.mkpath(QStringLiteral("."))) {
        fail(QStringLiteral("无法创建模板库目录：%1").arg(parent.path()));
    }
    QLockFile lock(databasePath_ + QStringLiteral(".lock"));
    lockTemplates(lock);
    Templates templates = loadTemplates();
    if (templates.contains(label)) {
        fail(QStringLiteral("此名字或代号已经录入：%1").arg(label));
    }
    templates.insert(label, feature);
    saveTemplates(templates);
    return faces.front();
}

CheckResult FaceService::check(const cv::Mat& image) {
    CheckResult result{Decision::Unknown, {}, std::nullopt, detect(image)};
    if (result.faces.empty()) {
        result.decision = Decision::NoFace;
        return result;
    }
    if (result.faces.size() != 1) {
        result.decision = Decision::MultipleFaces;
        return result;
    }

    const Templates templates = loadTemplates();
    if (templates.isEmpty()) {
        result.decision = Decision::NoEnrollment;
        return result;
    }
    const cv::Mat query = embed(image(result.faces.front()));
    double bestScore = -1.0;
    QString bestName;
    for (auto it = templates.cbegin(); it != templates.cend(); ++it) {
        const double score = std::clamp(query.dot(it.value()), -1.0, 1.0);
        if (bestName.isEmpty() || score > bestScore) {
            bestScore = score;
            bestName = it.key();
        }
    }
    result.similarity = bestScore;
    // 最近的模板仍可能属于另一个人。只有达到阈值才返回授权和录入名称。
    if (bestScore >= matchThreshold) {
        result.decision = Decision::Authorized;
        result.name = bestName;
    }
    return result;
}

QStringList FaceService::templateNames() const {
    // 管理界面只拿到名称。向量仍由服务层负责读取和校验。
    return loadTemplates().keys();
}

bool FaceService::removeTemplate(const QString& name) {
    if (!QFileInfo(databasePath_).absoluteDir().exists()) {
        return false;
    }
    QLockFile lock(databasePath_ + QStringLiteral(".lock"));
    lockTemplates(lock);
    Templates templates = loadTemplates();
    if (!templates.remove(name)) {
        return false;
    }
    // 最后一条删除后也提交一个合法的空库，供下次验证和录入使用。
    saveTemplates(templates);
    return true;
}

void FaceService::lockTemplates(QLockFile& lock) const {
    // 录入和删除用同一把文件锁覆盖整个读取、修改、保存过程。
    if (!lock.tryLock(1000)) {
        fail(QStringLiteral("模板库正在写入或无法加锁：%1").arg(databasePath_));
    }
}

FaceService::Templates FaceService::loadTemplates() const {
    QFile file(databasePath_);
    if (!file.exists()) {
        return {};
    }
    if (!file.open(QIODevice::ReadOnly)) {
        fail(QStringLiteral("无法读取人脸模板库：%1").arg(file.errorString()));
    }
    const auto root = QJsonDocument::fromJson(file.readAll()).object();
    if (root.value("version").toInt() != databaseVersion ||
        root.value("model").toString() != QString::fromLatin1(embeddingModel) ||
        !root.value("templates").isObject()) {
        fail(QStringLiteral("模板库格式或模型版本不匹配：%1").arg(databasePath_));
    }
    Templates templates;
    const auto entries = root.value("templates").toObject();
    for (auto it = entries.constBegin(); it != entries.constEnd(); ++it) {
        const auto values = it.value().toArray();
        if (it.key().trimmed().isEmpty() || values.size() != embeddingSize) {
            fail(QStringLiteral("模板库里有无效记录：%1").arg(it.key()));
        }
        cv::Mat vector(1, embeddingSize, CV_32F);
        for (int i = 0; i < embeddingSize; ++i) {
            const auto value = values.at(i);
            if (!value.isDouble() || std::abs(value.toDouble()) > 1.0001) {
                fail(QStringLiteral("模板库里有无效特征：%1").arg(it.key()));
            }
            vector.at<float>(0, i) = static_cast<float>(value.toDouble());
        }
        if (std::abs(cv::norm(vector) - 1.0) > 1e-3) {
            fail(QStringLiteral("模板库里有未归一化的特征：%1").arg(it.key()));
        }
        templates.insert(it.key(), std::move(vector));
    }
    return templates;
}

void FaceService::saveTemplates(const Templates& templates) const {
    QJsonObject entries;
    for (auto it = templates.cbegin(); it != templates.cend(); ++it) {
        QJsonArray values;
        for (int i = 0; i < embeddingSize; ++i) {
            values.append(it.value().at<float>(0, i));
        }
        entries.insert(it.key(), values);
    }

    QSaveFile file(databasePath_);
    const auto data = QJsonDocument(QJsonObject{{"version", databaseVersion},
                                               {"model", QString::fromLatin1(embeddingModel)},
                                               {"templates", entries}}).toJson();
    // 写到临时文件并原子替换。读取端只会看到旧的完整模板库或新的完整模板库。
    if (!file.open(QIODevice::WriteOnly) ||
        !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
        file.write(data) != data.size() || !file.commit()) {
        fail(QStringLiteral("无法保存模板库：%1").arg(databasePath_));
    }
}
