#pragma once

#include <QMap>
#include <QString>
#include <QStringList>
#include <opencv2/core.hpp>
#include <opencv2/dnn.hpp>

#include <optional>
#include <vector>

enum class Decision { Authorized, Unknown, NoEnrollment, NoFace, MultipleFaces };

class QLockFile;

struct CheckResult {
    Decision decision;
    QString name;
    std::optional<double> similarity;
    std::vector<cv::Rect> faces;
};

cv::Mat loadImage(const QString& path);
const char* decisionCode(Decision decision);

class FaceService {
public:
    static constexpr double matchThreshold = 0.65;

    FaceService(const QString& modelsDir, const QString& databasePath);
    cv::Rect enroll(const QString& name, const cv::Mat& image);
    CheckResult check(const cv::Mat& image);
    QStringList templateNames() const;
    bool removeTemplate(const QString& name);

private:
    using Templates = QMap<QString, cv::Mat>;

    std::vector<cv::Rect> detect(const cv::Mat& image);
    cv::Mat embed(const cv::Mat& face);
    Templates loadTemplates() const;
    void saveTemplates(const Templates& templates) const;
    void lockTemplates(QLockFile& lock) const;

    QString databasePath_;
    cv::dnn::Net detector_;
    cv::dnn::Net recognizer_;
};
