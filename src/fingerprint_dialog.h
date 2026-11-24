#pragma once

#include <QDialog>
#include <QJsonObject>
#include <QProcess>

class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

class FingerprintDialog : public QDialog {
    Q_OBJECT

public:
    explicit FingerprintDialog(const QString& port, QWidget* parent = nullptr);
    ~FingerprintDialog() override;
    QString port() const;
    void reject() override;

signals:
    void verified(int slot, bool simulation);

private:
    void startOperation(const QString& operation);
    void readMessages();
    void finishOperation(int exitCode, QProcess::ExitStatus exitStatus);
    void setBusy(bool busy);

    QProcess process_;
    QString operation_;
    QString error_;
    QJsonObject result_;
    bool malformed_ = false;
    bool simulation_ = false;
    QLineEdit* port_;
    QSpinBox* slot_;
    QLabel* mode_;
    QLabel* status_;
    QPushButton* probe_;
    QPushButton* enroll_;
    QPushButton* verify_;
};
