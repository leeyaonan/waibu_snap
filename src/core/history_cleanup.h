#pragma once
#include "core/history_types.h"
namespace waibusnap
{
struct HistoryCleanupResult
{
    QVector<HistoryItem> candidates;
    qint64 releasedBytes = 0;
    qsizetype protectedCount = 0;
    qint64 protectedBytes = 0;
    bool protectedOverLimit = false;
    QString error;
};
// 输入为同一组的扫描快照；选中项使用 historyItemId。最新受管项由算法自行确定。
HistoryCleanupResult selectCleanupCandidates(const CleanupPolicy& policy,
                                             const QVector<HistoryItem>& items, qint64 nowMs,
                                             const QString& selectedId = {});
}
