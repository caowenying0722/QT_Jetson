#include "face_service.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>

#include <exception>
#include <iostream>

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("smartlock"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Jetson 人脸门禁图片实验"));
    parser.addHelpOption();
    const QCommandLineOption models(QStringList{"m", "models"}, "模型目录", "dir", "assets/models");
    const QCommandLineOption database(QStringList{"d", "db"}, "模板库文件", "file", "data/faces.json");
    parser.addOptions({models, database});
    parser.addPositionalArgument("command", "model-check | enroll <name> <image> | check <image>");
    parser.process(app);
    const auto args = parser.positionalArguments();
    const QString command = args.value(0);
    const bool loadOnly = command == "model-check" && args.size() == 1;
    const bool enrolling = command == "enroll" && args.size() == 3;
    const bool checking = command == "check" && args.size() == 2;
    if (!loadOnly && !enrolling && !checking) {
        parser.showHelp(1);
    }

    try {
        FaceService service(parser.value(models), parser.value(database));
        if (loadOnly) {
            std::cout << "Caffe detector and OpenFace recognizer loaded on CPU\n";
            return 0;
        }
        const cv::Mat image = loadImage(args.last());
        if (enrolling) {
            service.enroll(args.at(1), image);
            std::cout << "enrolled: " << args.at(1).trimmed().toStdString() << '\n';
            return 0;
        }

        // CLI 和 GUI 调用同一个 FaceService。这里只负责把结果变成可核验的 JSON。
        QElapsedTimer timer;
        timer.start();
        const auto result = service.check(image);
        const double elapsedMs = timer.nsecsElapsed() / 1e6;
        const bool authorized = result.decision == Decision::Authorized;
        QJsonObject output{
            {"decision", QString::fromLatin1(decisionCode(result.decision))},
            {"face_count", static_cast<int>(result.faces.size())},
            {"threshold", FaceService::matchThreshold},
            {"elapsed_ms", elapsedMs},
            {"simulated_unlock", authorized}
        };
        if (result.similarity) {
            output.insert("similarity", *result.similarity);
        }
        if (authorized) {
            output.insert("name", result.name);
        }
        std::cout << QJsonDocument(output).toJson(QJsonDocument::Compact).constData() << '\n';
        // 0 是授权，3 是业务拒绝，1 是文件、模型或命令出错。
        return authorized ? 0 : 3;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
