#pragma once
#include "core/history_cleanup.h"
#include <QImage>
#include <functional>
namespace waibusnap
{
struct HistoryStatus
{
    bool success = false;
    QString error;
};
struct HistoryGroupSnapshot
{
    HistoryGroup group;
    QVector<HistoryItem> items;
    GroupUsage usage;
    bool cleanupPaused = false;
    QString error;
};
struct HistoryScanResult
{
    QVector<HistoryGroupSnapshot> groups;
    QString error;
};
struct HistoryGroupResult : HistoryStatus
{
    HistoryGroup group;
};
struct HistoryWriteResult : HistoryStatus
{
    HistoryItem item;
};
struct HistoryItemsResult : HistoryStatus
{
    QVector<HistoryItem> items;
    GroupUsage usage;
};
// 回调失败必须保留原目录；Store 不提供永久删除分组的降级路径。
using HistoryTrash = std::function<HistoryStatus(const QString&)>;
class HistoryStore
{
  public:
    explicit HistoryStore(QString rootDirectory = {}, CleanupPolicy defaultCleanup = {},
                          HistoryTrash trash = {});
    static QString defaultRootDirectory();
    QString rootDirectory() const { return rootDirectory_; }
    // 构造不碰磁盘。首次显式初始化才创建根目录和默认分组。
    HistoryScanResult initialize();
    HistoryScanResult scan() const;
    HistoryGroupResult createGroup(const QString& name);
    HistoryGroupResult renameGroup(const QString& groupId, const QString& name);
    HistoryStatus deleteGroup(const QString& groupId);
    HistoryStatus saveCleanupPolicy(const QString& groupId, const CleanupPolicy& policy);
    HistoryWriteResult writeManagedPng(const QString& groupId, const QImage& image,
                                       HistorySource source, const QString& existingFileName = {});
    HistoryStatus setStarred(const QString& groupId, const QString& fileName, bool starred);
    HistoryItemsResult itemsForGroup(const QString& groupId,
                                     HistorySort order = HistorySort::OldestFirst,
                                     const QString& selectedId = {}) const;
    HistoryCleanupResult cleanupCandidates(const QString& groupId, qint64 nowMs,
                                           const QString& selectedId = {}) const;

  private:
    HistoryScanResult scanUnlocked() const;
    HistoryGroupResult createGroupUnlocked(const QString& name, bool isDefault);
    QString rootDirectory_;
    CleanupPolicy defaultCleanup_;
    HistoryTrash trash_;
};
}
