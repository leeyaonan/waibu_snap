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
                          HistoryTrash trash = {}, std::function<QString()> excludedDirectory = {});
    HistoryStore(QString rootDirectory, CleanupPolicy defaultCleanup, HistoryTrash trash,
                 std::function<QStringList()> excludedDirectories);
    static QString defaultRootDirectory();
    QString rootDirectory() const { return rootDirectory_; }
    // 仅供显式保存设置时做目录冲突校验；只读登记与顶层 marker，不扫描图片、不初始化历史。
    QStringList registeredDirectories(QString* error = nullptr) const;
    // 构造不碰磁盘。首次显式初始化才创建根目录和默认分组。
    HistoryScanResult initialize();
    HistoryScanResult scan() const;
    // directory 为空时沿用 <root>/<name>；否则使用用户选择的独立绝对目录。
    HistoryGroupResult createGroup(const QString& name, const QString& directory = {});
    HistoryGroupResult renameGroup(const QString& groupId, const QString& name);
    // 将整组图片与元数据移动到独立目录；目标必须为空或不存在。
    HistoryGroupResult changeGroupDirectory(const QString& groupId, const QString& directory);
    HistoryStatus deleteGroup(const QString& groupId, const HistoryTrash& trash = {});
    HistoryStatus saveCleanupPolicy(const QString& groupId, const CleanupPolicy& policy);
    HistoryWriteResult writeManagedPng(const QString& groupId, const QImage& image,
                                       HistorySource source, const QString& existingFileName = {},
                                       const QImage& original = {});
    HistoryStatus setStarred(const QString& groupId, const QString& fileName, bool starred);
    HistoryStatus deleteManagedItem(const HistoryItem& item, const HistoryTrash& trash);
    // 自动清理永久删除：锁内重扫并重新核对策略 / 保护；失败保留恢复信息。
    HistoryStatus permanentlyDeleteManagedItem(const HistoryItem& item, qint64 nowMs,
                                               const QString& selectedId = {});
    HistoryStatus validateItem(const HistoryItem& item) const;
    HistoryItemsResult itemsForGroup(const QString& groupId,
                                     HistorySort order = HistorySort::OldestFirst,
                                     const QString& selectedId = {}) const;
    HistoryCleanupResult cleanupCandidates(const QString& groupId, qint64 nowMs,
                                           const QString& selectedId = {}) const;

  private:
    friend struct HistoryCleanupExecutionTestAccess;
    HistoryTrash permanentRemove_;
    HistoryStatus removeManagedItem(const HistoryItem& item, const HistoryTrash& remove,
                                    bool automatic, qint64 nowMs = 0,
                                    const QString& selectedId = {});
    HistoryScanResult scanUnlocked() const;
    HistoryGroupResult createGroupUnlocked(const QString& name, bool isDefault,
                                           const QString& directory,
                                           const QStringList& existingNames);
    std::function<QStringList()> excludedDirectories_;
    QString rootDirectory_;
    CleanupPolicy defaultCleanup_;
    HistoryTrash trash_;
};
}
