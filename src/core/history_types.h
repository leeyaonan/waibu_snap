#pragma once
#include <QJsonObject>
#include <QSize>
#include <QStringList>
#include <QVector>
namespace waibusnap
{
enum class HistorySource
{
    Overlay,
    Scroll,
    External
};
enum class HistorySort
{
    OldestFirst,
    NewestFirst
};
struct CleanupPolicy
{
    bool enabled = true;
    bool ageEnabled = true;
    int maxAgeDays = 30;
    bool countEnabled = true;
    int maxCount = 100;
    bool bytesEnabled = true;
    qint64 maxBytes = 100 * 1024 * 1024;
};
struct HistoryGroup
{
    QString id;
    QString name;
    QString absolutePath;
    bool isDefault = false;
    qint64 createdAtMs = 0;
    CleanupPolicy cleanup;
};
struct HistoryItem
{
    QString groupId;
    QString fileName;
    QString absolutePath;
    HistorySource source = HistorySource::External;
    bool managed = false;
    bool starred = false;
    qint64 bytes = 0;
    qint64 createdAtMs = 0;
    QSize pixelSize;
};
struct GroupUsage
{
    qsizetype managedCount = 0;
    qsizetype externalCount = 0;
    qsizetype starredCount = 0;
    qint64 managedBytes = 0;
    qint64 externalBytes = 0;
    qsizetype protectedCount = 0;
    qint64 protectedBytes = 0;
};
// 调用者使用返回的规范名；原始输入中的控制字符即使位于首尾也拒绝。
QString normalizedHistoryGroupName(const QString& name);
QString historyGroupNameError(const QString& name, const QStringList& existingNames = {});
QString cleanupPolicyError(const CleanupPolicy& policy);
QJsonObject cleanupPolicyToJson(const CleanupPolicy& policy);
bool cleanupPolicyFromJson(const QJsonObject& json, CleanupPolicy& policy);
QString historyItemId(const HistoryItem& item);
QVector<HistoryItem> sortedHistoryItems(QVector<HistoryItem> items, HistorySort order);
GroupUsage historyUsage(const QVector<HistoryItem>& items);
}
