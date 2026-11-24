#pragma once

#include <QDialog>
#include <QProcess>
#include <QTimer>

class QLabel;
class QPushButton;

class GestureDialog : public QDialog {
    Q_OBJECT

public:
    explicit GestureDialog(QWidget* parent);
    ~GestureDialog() override;
    void setVerificationFeedback(const QString& text);

signals:
    void verified();

private:
    void start(bool verify);
    void readEvents();
    void finish(int exitCode, QProcess::ExitStatus exitStatus);
    void setBusy(bool busy);

    QProcess helper_;
    QTimer timeout_;
    QByteArray pending_;
    QString problem_;
    bool verifying_ = false;
    bool unlocked_ = false;
    bool receivedResult_ = false;
    bool closing_ = false;
    QLabel* status_;
    QLabel* lastGesture_;
    QPushButton* read_;
    QPushButton* verify_;
};
