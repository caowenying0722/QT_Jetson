#include "unlock_history.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>

#include <stdexcept>

namespace {
constexpr int maxEntries = 500;

std::runtime_error storageError(const QString& message) {
    return std::runtime_error(message.toUtf8().constData());
}
}

UnlockHistory::UnlockHistory(const QString& faceDatabasePath)
    : path_(faceDatabasePath + QStringLiteral(".unlock-history.json")) {}

QVector<UnlockEntry> UnlockHistory::newestFirst() const {
    QFile file(path_);
    if (!file.exists()) {
        return {};
    }
    if (!file.open(QIODevice::ReadOnly)) {
        throw storageError(QStringLiteral("无法打开历史文件 · %1").arg(file.errorString()));
    }
    const auto document = QJsonDocument::fromJson(file.readAll());
    if (file.error() != QFileDevice::NoError) {
        throw storageError(QStringLiteral("无法读取历史文件 · %1").arg(file.errorString()));
    }
    if (!document.isArray()) {
        throw storageError(QStringLiteral("历史文件格式无效"));
    }

    QVector<UnlockEntry> entries;
    const auto rows = document.array();
    entries.reserve(rows.size());
    for (const auto& value : rows) {
        const auto row = value.toObject();
        UnlockEntry entry{QDateTime::fromString(row.value("time").toString(), Qt::ISODateWithMs),
                          row.value("method").toString(), row.value("subject").toString()};
        // 解析失败时保留原文件，避免把损坏的历史当作空历史覆盖。
        if (!entry.time.isValid() || entry.method.isEmpty() || entry.subject.isEmpty()) {
            throw storageError(QStringLiteral("历史文件包含无效记录"));
        }
        entries.append(entry);
    }
    return entries;
}

void UnlockHistory::append(const UnlockEntry& entry) const {
    if (!QDir().mkpath(QFileInfo(path_).absolutePath())) {
        throw storageError(QStringLiteral("无法创建历史目录"));
    }
    // 写锁保护整个读改写过程，两个 Qt 窗口同时开锁时不会互相覆盖。
    QLockFile lock(path_ + QStringLiteral(".lock"));
    if (!lock.tryLock(150)) {
        throw storageError(QStringLiteral("无法锁定历史文件"));
    }
    auto entries = newestFirst();
    entries.prepend(entry);
    if (entries.size() > maxEntries) {
        entries.resize(maxEntries);
    }

    QJsonArray rows;
    for (const auto& item : entries) {
        rows.append(QJsonObject{{"time", item.time.toUTC().toString(Qt::ISODateWithMs)},
                                {"method", item.method}, {"subject", item.subject}});
    }
    // 先写临时文件，提交时原子替换，读者始终看到一份完整的 JSON。
    QSaveFile file(path_);
    const auto data = QJsonDocument(rows).toJson(QJsonDocument::Compact);
    if (!file.open(QIODevice::WriteOnly) ||
        !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
        file.write(data) != data.size() || !file.commit()) {
        throw storageError(QStringLiteral("无法保存历史文件 · %1").arg(file.errorString()));
    }
}
