#include "core/history_cleanup.h"
#include <QSet>
#include <limits>
namespace waibusnap
{
HistoryCleanupResult selectCleanupCandidates(const CleanupPolicy& policy,
                                             const QVector<HistoryItem>& items, qint64 nowMs,
                                             const QString& selectedId)
{
    HistoryCleanupResult result;
    result.error = cleanupPolicyError(policy);
    if (!result.error.isEmpty())
        return result;
    auto ordered = sortedHistoryItems(items, HistorySort::OldestFirst);
    QString latest;
    QString groupId;
    qint64 bytes = 0;
    qsizetype count = 0;
    qsizetype expired = 0;
    QSet<QString> ids;
    const auto isExpired = [&](const HistoryItem& item)
    {
        // 用差值避免减去保留时长时的时间戳下溢，等于上限不清理。
        return policy.ageEnabled && nowMs > item.createdAtMs &&
               nowMs - item.createdAtMs > qint64(policy.maxAgeDays) * 86400000;
    };
    for (const auto& item : ordered)
    {
        if (!item.managed)
            continue;
        if (item.bytes < 0 || item.createdAtMs < 0 || nowMs < 0 ||
            (item.source != HistorySource::Overlay && item.source != HistorySource::Scroll) ||
            item.bytes > std::numeric_limits<qint64>::max() - bytes ||
            ids.contains(historyItemId(item)) || (count && groupId != item.groupId))
        {
            result.error = QStringLiteral("清理记录无效、重复、跨组或占用溢出，已停止选择候选。");
            return result;
        }
        groupId = item.groupId;
        ids.insert(historyItemId(item));
        latest = historyItemId(item);
        bytes += item.bytes;
        ++count;
        if (isExpired(item))
            ++expired;
    }
    for (const auto& item : ordered)
    {
        if (!item.managed)
            continue;
        const bool protectedItem = item.starred || historyItemId(item) == latest ||
                                   historyItemId(item) == selectedId ||
                                   (policy.bytesEnabled && item.bytes > policy.maxBytes);
        if (protectedItem)
        {
            ++result.protectedCount;
            result.protectedBytes += item.bytes;
            continue;
        }
        const bool quotaExceeded = (policy.countEnabled && count > policy.maxCount) ||
                                   (policy.bytesEnabled && bytes > policy.maxBytes);
        // 时长规则只使过期项成为可清理项；受保护的旧图不连带删除尚未过期的新图。
        if (policy.enabled && (quotaExceeded || isExpired(item)))
        {
            result.candidates.append(item);
            result.releasedBytes += item.bytes;
            bytes -= item.bytes;
            --count;
            if (isExpired(item))
                --expired;
        }
    }
    result.protectedOverLimit =
        policy.enabled &&
        ((policy.ageEnabled && expired > 0) || (policy.countEnabled && count > policy.maxCount) ||
         (policy.bytesEnabled && bytes > policy.maxBytes));
    return result;
}
}
