#pragma once
#include "app/app_settings.h"
#include "app/history_store.h"
#include <QHash>
#include <QObject>
#include <memory>
#include <optional>
namespace waibusnap
{
struct HistorySession
{
    QString id;
    QString groupId;
};
struct HistorySnapshot
{
    QVector<HistoryGroupSnapshot> groups;
    QString currentGroupId;
    QString error;
};
struct HistoryImageResult : HistoryStatus
{
    QImage image;
};
struct HistoryCleanupReport
{
    qsizetype deletedCount = 0;
    qint64 releasedBytes = 0;
    qsizetype protectedCount = 0;
    qint64 protectedBytes = 0;
    bool protectedOverLimit = false;
    QString error;
    QString message() const;
};
class HistoryManager final : public QObject
{
    Q_OBJECT
  public:
    // 根目录覆盖仅用于受控运行，不写回用户的根目录偏好。
    explicit HistoryManager(AppSettings settings, QString rootOverride = {},
                            QObject* parent = nullptr);
    HistoryStatus initialize();
    HistoryStatus savePreferences(HistoryPreferences preferences);
    GroupUsage totalUsage(const QString& selectedId = {});
    HistoryCleanupReport runCleanup(qint64 nowMs = -1);
    HistoryCleanupReport lastCleanup() const { return cleanup_; }
    void setProtectedItem(const QString& itemId);
    QString protectedItemId() const { return protectedItemId_; }
    bool ready() const { return ready_; }
    QString error() const { return error_; }
    QString rootDirectory() const;
    QString currentGroupId() const { return preferences_.currentGroupId; }
    std::optional<HistoryGroup> currentGroup() const;
    QVector<HistoryGroup> groups() const { return groups_; }
    HistoryStatus setCurrentGroup(const QString& groupId);
    HistoryGroupResult createGroup(const QString& name);
    HistoryGroupResult renameGroup(const QString& groupId, const QString& name);
    HistoryStatus deleteGroup(const QString& groupId, const HistoryTrash& trash);
    HistoryStatus saveCleanupPolicy(const QString& groupId, const CleanupPolicy& policy,
                                    bool makeCurrent = false);
    CleanupPolicy defaultCleanupPolicy() const;
    // 浏览刷新不修改入库错误、当前分组或会话去重表。
    HistorySnapshot refresh();
    HistorySnapshot snapshot() const;
    HistoryImageResult loadImage(const QString& itemId, QSize bounds = {}) const;
    std::optional<HistoryItem> findItem(const QString& itemId) const;
    HistoryStatus setStarred(const QString& itemId, bool starred);
    HistoryStatus deleteItem(const QString& itemId, const HistoryTrash& trash);
    HistoryStatus revealItem(const QString& itemId, const HistoryTrash& reveal) const;
    HistorySession beginSession() const;
    HistoryWriteResult record(const HistorySession& session, const QImage& image,
                              HistorySource source);
  signals:
    void errorChanged(QString error);
    void currentGroupChanged(QString groupId);
    void groupsChanged();
    void cleanupFinished();

  private:
    friend struct HistoryCleanupExecutionTestAccess;
    HistoryStatus status(QString error = {});
    HistoryStatus persistCurrentGroup(const QString& groupId);
    AppSettings settings_;
    QString rootOverride_;
    HistoryPreferences preferences_;
    std::unique_ptr<HistoryStore> store_;
    QVector<HistoryGroup> groups_;
    QHash<QString, HistoryItem> sessions_;
    HistorySnapshot snapshot_;
    HistoryCleanupReport cleanup_;
    QString protectedItemId_;
    QString error_;
    bool ready_ = false;
};
}
