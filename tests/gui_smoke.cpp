#include "demo_window.h"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>

#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

QByteArray readFile(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "Could not read temporary template database.");
    return file.readAll();
}

QStringList templateRows(const QListWidget& list) {
    QStringList names;
    for (int row = 0; row < list.count(); ++row) {
        names.append(list.item(row)->text());
    }
    return names;
}

QStringList exerciseTemplateDialog(DemoWindow& window, const QString& target,
                                   QMessageBox::StandardButton response = QMessageBox::NoButton) {
    auto* manage = window.findChild<QPushButton*>("manageTemplatesButton");
    require(manage && manage->isEnabled(), "Missing template management entry.");
    QStringList remaining;
    bool dialogSeen = false;
    bool confirmedAsExpected = false;
    // 两个 QTimer 分别进入管理框和确认框的事件循环，点击实际按钮。
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog) {
            return;
        }
        auto* list = dialog->findChild<QListWidget*>("templateList");
        auto* remove = dialog->findChild<QPushButton*>("removeTemplateButton");
        dialogSeen = dialog->objectName() == "templateDialog" && list && remove;
        if (dialogSeen) {
            if (response != QMessageBox::NoButton) {
                const auto items = list->findItems(target, Qt::MatchExactly);
                if (items.size() == 1) {
                    list->setCurrentItem(items.front());
                    if (remove->isEnabled()) {
                        QTimer::singleShot(0, dialog, [&] {
                            auto* modal = QApplication::activeModalWidget();
                            auto* box = qobject_cast<QMessageBox*>(modal);
                            if (box && box->objectName() == "removeTemplateConfirmation" && box->button(response)) {
                                confirmedAsExpected = box->text().contains(target) &&
                                    box->defaultButton() == box->button(QMessageBox::Cancel);
                                box->button(response)->click();
                            } else if (modal) {
                                modal->close();
                            }
                        });
                        remove->click();
                    }
                }
            }
            remaining = templateRows(*list);
        }
        dialog->reject();
    });
    manage->click();
    require(dialogSeen, "Template list dialog did not open.");
    require(response == QMessageBox::NoButton || confirmedAsExpected,
            "Delete confirmation did not show the target and default to Cancel.");
    return remaining;
}

void verifyHistoryDialog(DemoWindow& reopened) {
    auto* open = reopened.findChild<QPushButton*>("unlockHistoryButton");
    require(open, "History button is missing.");
    bool checked = false;
    QTimer::singleShot(0, &reopened, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog) {
            return;
        }
        auto* table = dialog->findChild<QTableWidget*>("unlockHistoryTable");
        auto* method = dialog->findChild<QComboBox*>("unlockHistoryMethod");
        // 新窗口应显示磁盘里的两次成功记录，并支持切换筛选方式。
        if (table && method && table->rowCount() == 2 &&
            table->item(0, 1)->text() == QStringLiteral("手势")) {
            const auto face = QStringLiteral("图片人脸");
            const auto index = method->findData(face);
            if (index > 0) {
                method->setCurrentIndex(index);
                checked = table->rowCount() == 1 && table->item(0, 1)->text() == face &&
                    table->item(0, 2)->text() == QStringLiteral("样例 A");
            }
        }
        dialog->reject();
    });
    open->click();
    require(checked, "History was not restored in order or the method filter failed.");
}
}

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    try {
        require(app.arguments().size() == 2, "Usage: gui_smoke <assets-dir>");
        const QDir assets(app.arguments().at(1));
        QTemporaryDir temp;
        require(temp.isValid(), "Could not create isolated test storage.");
        const auto database = temp.filePath("faces.json");
        DemoWindow window(assets.filePath("models"), database);
        window.show();
        app.processEvents();

        auto* name = window.findChild<QLineEdit*>("nameInput");
        auto* enroll = window.findChild<QPushButton*>("enrollButton");
        auto* check = window.findChild<QPushButton*>("checkButton");
        auto* status = window.findChild<QLabel*>("decisionStatus");
        auto* details = window.findChild<QLabel*>("decisionDetails");
        auto* events = window.findChild<QPlainTextEdit*>("eventLog");
        require(name && enroll && check && status && details && events, "Missing window controls.");
        require(!enroll->isEnabled() && !check->isEnabled(), "Actions enabled without an image.");

        // 走实际按钮的 clicked 信号和槽，测试界面展示及授权分支的模拟开锁事件。
        window.openImage(assets.filePath("samples/enroll.jpg"));
        name->setText(QStringLiteral("样例 A"));
        enroll->click();
        require(status->text() == QStringLiteral("录入成功"), "GUI enrollment failed.");

        window.openImage(assets.filePath("samples/known.jpg"));
        check->click();
        require(status->text() == QStringLiteral("授权通过 · 模拟开锁"), "Known user rejected.");
        require(events->toPlainText().count(QStringLiteral("模拟开锁 |")) == 1, "Missing unlock event.");
        std::cout << status->text().toStdString() << " / " << details->text().toStdString() << '\n';

        window.openImage(assets.filePath("samples/unknown.jpg"));
        check->click();
        require(status->text() == QStringLiteral("拒绝 · 未知用户"), "Unknown user was not rejected.");
        require(events->toPlainText().count(QStringLiteral("模拟开锁 |")) == 1,
                "Rejected user triggered another unlock event.");
        std::cout << status->text().toStdString() << " / " << details->text().toStdString() << '\n';

        const auto screenshot = qEnvironmentVariable("SMARTLOCK_SCREENSHOT");
        if (!screenshot.isEmpty()) {
            app.processEvents();
            require(window.grab().save(screenshot), "Could not save requested screenshot.");
        }

        QImage blank(640, 480, QImage::Format_RGB888);
        blank.fill(Qt::white);
        const auto blankPath = temp.filePath("blank.png");
        require(blank.save(blankPath), "Could not create the no-face test image.");
        window.openImage(blankPath);
        check->click();
        require(status->text() == QStringLiteral("拒绝 · 未检测到人脸"), "Blank image was not rejected.");
        require(events->toPlainText().count(QStringLiteral("模拟开锁 |")) == 1,
                "Blank image triggered another unlock event.");
        std::cout << status->text().toStdString() << '\n';

        UnlockHistory history(database);
        const auto entries = history.newestFirst();
        require(entries.size() == 1 && entries.front().method == QStringLiteral("图片人脸"),
                "History must contain only the successful face check.");
        // 加一条手势样例用于验证筛选。手势入口写盘由 gesture_gui 单独覆盖。
        history.append({QDateTime::currentDateTimeUtc(), QStringLiteral("手势"),
                        QStringLiteral("左 → 上 → 顺时针")});
        {
            DemoWindow reopened(assets.filePath("models"), database);
            reopened.show();
            verifyHistoryDialog(reopened);
        }

        const auto first = QStringLiteral("样例 A");
        const auto retained = QStringLiteral("样例 B");
        window.openImage(assets.filePath("samples/unknown.jpg"));
        name->setText(retained);
        enroll->click();
        require(status->text() == QStringLiteral("录入成功"), "Second template could not be enrolled.");
        require(exerciseTemplateDialog(window, {}) == QStringList{first, retained}, "List must show both templates.");

        const auto beforeCancel = readFile(database);
        require(exerciseTemplateDialog(window, first, QMessageBox::Cancel) == QStringList{first, retained},
                "Cancel must keep both templates.");
        require(readFile(database) == beforeCancel, "Cancel unexpectedly modified the database.");
        require(!events->toPlainText().contains(QStringLiteral("已删除模板")), "Cancel logged a deletion.");
        require(exerciseTemplateDialog(window, first, QMessageBox::Yes) == QStringList{retained},
                "Confirmed deletion must refresh the list with only the retained template.");
        require(events->toPlainText().contains(QStringLiteral("已删除模板 | %1").arg(first)),
                "Confirmed deletion was not logged.");

        window.openImage(assets.filePath("samples/known.jpg"));
        check->click();
        require(status->text() == QStringLiteral("拒绝 · 未知用户"), "Deleted face was still accepted.");
        window.openImage(assets.filePath("samples/unknown.jpg"));
        check->click();
        require(status->text() == QStringLiteral("授权通过 · 模拟开锁"), "Deleting one face affected the other.");
        require(exerciseTemplateDialog(window, retained, QMessageBox::Yes).isEmpty(),
                "Deleting the last template must leave an empty list.");
        FaceService reopened(assets.filePath("models"), database);
        require(reopened.templateNames().isEmpty(), "The empty list must survive reopening the database.");
        check->click();
        require(status->text() == QStringLiteral("拒绝 · 请先录入用户"), "An empty database must deny verification.");
        std::cout << "Template management passed: list, cancel unchanged, selective deletion, rejection, empty database\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
