#pragma once

#include <QString>

class PwmServo;
class QLabel;
class QWidget;

void showDeviceChecks(QWidget* parent, const QString& camera, const PwmServo* servo, const QLabel& latchFeedback);
