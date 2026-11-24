#pragma once

#include <QDateTime>
#include <QString>
#include <QVector>

struct UnlockEntry {
    QDateTime time;
    QString method;
    QString subject;
};

class UnlockHistory {
public:
    explicit UnlockHistory(const QString& faceDatabasePath);
    QVector<UnlockEntry> newestFirst() const;
    void append(const UnlockEntry& entry) const;

private:
    QString path_;
};
