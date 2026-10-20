#include "app/history_manager.h"
#include <QDebug>
#include <QUuid>
namespace waibusnap
{
HistoryManager::HistoryManager(AppSettings settings, QString rootOverride, QObject* parent)
    : QObject(parent), settings_(std::move(settings)), rootOverride_(std::move(rootOverride))
{
}
HistoryStatus HistoryManager::status(QString error)
{
    if (!error.isEmpty())
        qWarning().noquote() << "截图历史：" << error;
    if (error_ != error)
    {
        error_ = error;
        emit errorChanged(error_);
    }
    return {error.isEmpty(), error};
}
QString HistoryManager::rootDirectory() const
{
    return store_ ? store_->rootDirectory() : QString();
}
std::optional<HistoryGroup> HistoryManager::currentGroup() const
{
    for (const auto& group : groups_)
        if (group.id == currentGroupId())
            return group;
    return {};
}
HistoryStatus HistoryManager::persistCurrentGroup(const QString& groupId)
{
    auto preferences = settings_.loadHistoryPreferences();
    preferences.currentGroupId = groupId;
    return status(settings_.saveHistoryPreferences(preferences));
}
HistoryStatus HistoryManager::initialize()
{
    if (ready_)
        return {true, {}};
    QString notice;
    preferences_ = settings_.loadHistoryPreferences(&notice);
    store_ = std::make_unique<HistoryStore>(rootOverride_.isEmpty() ? preferences_.rootDirectory
                                                                    : rootOverride_,
                                            preferences_.defaultCleanup);
    const auto scanned = store_->initialize();
    groups_.clear();
    for (const auto& snapshot : scanned.groups)
        groups_.append(snapshot.group);
    if (!scanned.error.isEmpty())
        return status(scanned.error);
    QString defaultId;
    for (const auto& group : groups_)
        if (group.isDefault)
            defaultId = group.id;
    if (defaultId.isEmpty())
        return status(QStringLiteral("历史默认分组缺失，截图历史暂不可用。"));
    ready_ = true;
    if (!currentGroup())
    {
        if (!preferences_.currentGroupId.isEmpty())
            qWarning("历史当前分组已丢失，已回退默认分组。");
        preferences_.currentGroupId = defaultId;
        const auto persisted = persistCurrentGroup(defaultId);
        if (!persisted.success)
            return persisted;
    }
    return status(notice);
}
HistoryStatus HistoryManager::setCurrentGroup(const QString& groupId)
{
    if (!ready_)
        return status(QStringLiteral("历史库未就绪，无法切换当前分组。"));
    const auto scanned = store_->scan();
    if (!scanned.error.isEmpty())
        return status(scanned.error);
    groups_.clear();
    bool found = false;
    for (const auto& snapshot : scanned.groups)
    {
        groups_.append(snapshot.group);
        found |= snapshot.group.id == groupId;
    }
    if (!found)
        return status(QStringLiteral("目标历史分组不存在，当前分组未改变。"));
    const auto persisted = persistCurrentGroup(groupId);
    if (!persisted.success)
        return persisted;
    preferences_.currentGroupId = groupId;
    emit currentGroupChanged(groupId);
    return {true, {}};
}
HistorySession HistoryManager::beginSession() const
{
    return {QUuid::createUuid().toString(QUuid::WithoutBraces), currentGroupId()};
}
HistoryWriteResult HistoryManager::record(const HistorySession& session, const QImage& image,
                                          HistorySource source)
{
    if (!ready_ || session.id.isEmpty() || session.groupId.isEmpty())
        return {status(QStringLiteral("历史库未就绪或截图会话无效，本次输出未写入历史。")), {}};
    const auto existing = sessions_.constFind(session.id);
    if (existing != sessions_.cend() &&
        (existing->groupId != session.groupId || existing->source != source))
        return {status(QStringLiteral("截图历史会话的分组或来源不一致，已拒绝写入。")), {}};
    auto result =
        store_->writeManagedPng(session.groupId, image, source,
                                existing == sessions_.cend() ? QString() : existing->fileName);
    // 图片已发布但索引失败时也记住目标，重试不能产生第二张孤立 PNG。
    if (!result.item.fileName.isEmpty())
        sessions_.insert(session.id, result.item);
    status(result.error);
    return result;
}
}
