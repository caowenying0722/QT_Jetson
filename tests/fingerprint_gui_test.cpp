#include "demo_window.h"

#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void waitForText(const QLabel& label, const QString& expected) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!label.text().contains(expected) && elapsed.elapsed() < 4500) {
        QApplication::processEvents();
        QThread::msleep(15);
    }
    if (!label.text().contains(expected)) {
        throw std::runtime_error(QStringLiteral("Expected [%1], saw [%2]").arg(expected, label.text()).toStdString());
    }
}

void sendTouch(QProcess& simulator, const QByteArray& input, const QString& state) {
    simulator.readAllStandardOutput();
    simulator.write(input);
    QByteArray response;
    QElapsedTimer elapsed;
    elapsed.start();
    const auto expected = QStringLiteral("当前：%1").arg(state).toUtf8();
    while (!response.contains(expected) && elapsed.elapsed() < 1000) {
        simulator.waitForReadyRead(80);
        response += simulator.readAllStandardOutput();
    }
    require(response.contains(expected), "Simulator did not acknowledge the touch state.");
}
}

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    try {
        require(app.arguments().size() == 2, "Usage: fingerprint_gui_test <assets-dir>");
        const QDir assets(app.arguments().at(1));
        QProcess simulator;
        simulator.start("python3", {"-u", assets.absoluteFilePath("../tools/fingerprint_simulator.py")});
        require(simulator.waitForStarted(3000) && (simulator.canReadLine() || simulator.waitForReadyRead(3000)),
                "The PTY simulator could not start.");
        const auto port = simulator.readLine().trimmed();
        require(port.startsWith("PORT=/dev/pts/"), "The simulator did not return a PTY path.");

        QTemporaryDir temp;
        require(temp.isValid(), "Could not create isolated face database.");
        DemoWindow window(assets.filePath("models"), temp.filePath("faces.json"));
        window.setFingerprintPort(QString::fromUtf8(port.mid(5)));
        window.show();
        auto* open = window.findChild<QPushButton*>("fingerprintButton");
        auto* events = window.findChild<QPlainTextEdit*>("eventLog");
        require(open && events, "Fingerprint entry or session log is missing.");

        QString failure;
        bool completed = false;
        // 在真实的模态事件循环里按按钮，等待 Qt 子进程报告每一个阶段。
        QTimer::singleShot(0, &window, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog) {
                return;
            }
            try {
                auto* status = dialog->findChild<QLabel*>("fingerprintStatus");
                auto* probe = dialog->findChild<QPushButton*>("fingerprintProbe");
                auto* enroll = dialog->findChild<QPushButton*>("fingerprintEnroll");
                auto* verify = dialog->findChild<QPushButton*>("fingerprintVerify");
                require(status && probe && enroll && verify, "Missing fingerprint dialog controls.");
                probe->click();
                waitForText(*status, QStringLiteral("连接成功"));
                enroll->click();
                waitForText(*status, QStringLiteral("第一次"));
                sendTouch(simulator, "A\n", "A");
                waitForText(*status, QStringLiteral("松开"));
                sendTouch(simulator, "\n", QStringLiteral("已松开"));
                waitForText(*status, QStringLiteral("第二次"));
                sendTouch(simulator, "A\n", "A");
                waitForText(*status, QStringLiteral("录入成功"));
                require(events->toPlainText().isEmpty(), "Enrollment must not unlock.");

                verify->click();
                waitForText(*status, QStringLiteral("验证通过"));
                const auto unlock = QStringLiteral("模拟开锁 | 指纹（演练） · 槽位 1");
                require(events->toPlainText().count(unlock) == 1, "A match must record one simulated unlock.");
                sendTouch(simulator, "\n", QStringLiteral("已松开"));
                verify->click();
                waitForText(*status, QStringLiteral("请按压要验证"));
                sendTouch(simulator, "B\n", "B");
                waitForText(*status, QStringLiteral("拒绝 · 指纹未匹配"));
                require(events->toPlainText().count(unlock) == 1, "A rejected finger must not unlock.");
                const auto entries = UnlockHistory(temp.filePath("faces.json")).newestFirst();
                require(entries.size() == 1 && entries.front().method == QStringLiteral("指纹（演练）") &&
                        entries.front().subject == QStringLiteral("槽位 1"),
                        "Only the successful fingerprint match should persist, with its simulation label.");
                completed = true;
            } catch (const std::exception& error) {
                failure = QString::fromUtf8(error.what());
            }
            dialog->reject();
        });
        open->click();
        simulator.write("q\n");
        require(simulator.waitForFinished(1500), "Simulator did not exit.");
        if (!completed) {
            throw std::runtime_error(failure.isEmpty() ? "Fingerprint dialog did not complete" : failure.toStdString());
        }
        std::cout << "Qt fingerprint passed: PTY connection, enrollment, one simulated unlock, rejected finger\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
