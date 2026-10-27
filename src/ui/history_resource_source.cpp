#include "ui/history_resource_source.h"
#include "ui/group_settings_dialog.h"
#include <QMenu>
#include <QPointer>
namespace waibusnap
{
namespace
{
ResourceUsage usage(const GroupUsage& value)
{
    return {value.managedCount,  value.externalCount,  value.starredCount,  value.managedBytes,
            value.externalBytes, value.protectedCount, value.protectedBytes};
}
}
HistoryResourceSource::HistoryResourceSource(HistoryManager& manager, HistoryWindowActions actions,
                                             QObject* parent)
    : ResourceSource(parent), manager_(manager), actions_(std::move(actions))
{
    connect(&manager_, &HistoryManager::itemsChanged, this, &ResourceSource::changed);
    connect(&manager_, &HistoryManager::groupsChanged, this, &ResourceSource::changed);
    connect(&manager_, &HistoryManager::currentGroupChanged, this, &ResourceSource::changed);
}
QString HistoryResourceSource::id() const { return QStringLiteral("history"); }
QString HistoryResourceSource::displayName() const { return QStringLiteral("历史"); }
bool HistoryResourceSource::visible() const
{
    // 本批只在历史已初始化的入口创建来源；关闭历史时控制器仍关闭整个窗口。
    return manager_.ready();
}
ResourceCapabilities HistoryResourceSource::capabilities() const
{
    return {true, true, true, true, bool(actions_.trash), bool(actions_.reveal)};
}
ResourceSnapshot HistoryResourceSource::snapshot() const
{
    ResourceSnapshot result;
    const auto history = manager_.snapshot();
    result.error = history.error;
    result.notice = manager_.lastCleanup().message();
    for (const auto& group : history.groups)
    {
        result.groups.append({group.group.id, group.group.name,
                              group.group.id == history.currentGroupId, group.group.isDefault,
                              group.group.absolutePath});
        result.groupUsage.insert(group.group.id, usage(group.usage));
        result.usage.managedCount += group.usage.managedCount;
        result.usage.externalCount += group.usage.externalCount;
        result.usage.starredCount += group.usage.starredCount;
        result.usage.managedBytes += group.usage.managedBytes;
        result.usage.externalBytes += group.usage.externalBytes;
        result.usage.protectedCount += group.usage.protectedCount;
        result.usage.protectedBytes += group.usage.protectedBytes;
        for (const auto& item : group.items)
        {
            ResourceItem mapped{historyItemId(item),
                                id(),
                                item.groupId,
                                item.fileName,
                                item.absolutePath,
                                item.bytes,
                                item.originalBytes,
                                item.hasOriginal,
                                item.starred,
                                item.managed,
                                item.createdAtMs,
                                item.pixelSize,
                                {}};
            if (item.source == HistorySource::Scroll)
                mapped.badges.append(QStringLiteral("长图"));
            if (item.source == HistorySource::External)
                mapped.badges.append(QStringLiteral("外部图片"));
            if (item.hasOriginal)
                mapped.badges.append(QStringLiteral("水印/原图"));
            result.items.append(std::move(mapped));
        }
    }
    return result;
}
void HistoryResourceSource::refresh() { manager_.refresh(); }
ResourceImageResult HistoryResourceSource::loadImage(const QString& id, QSize bounds,
                                                     bool original) const
{
    const auto result = manager_.loadImage(
        id, bounds, original ? HistoryImageVariant::Original : HistoryImageVariant::Watermarked);
    return {{result.success, result.error}, result.image};
}
ResourceStatus HistoryResourceSource::setStarred(const QString& id, bool starred)
{
    const auto result = manager_.setStarred(id, starred);
    return {result.success, result.error};
}
ResourceStatus HistoryResourceSource::deleteItem(const QString& id)
{
    const auto result = manager_.deleteItem(id, actions_.trash);
    return {result.success, result.error};
}
ResourceStatus HistoryResourceSource::revealItem(const QString& id) const
{
    const auto result = manager_.revealItem(id, actions_.reveal);
    return {result.success, result.error};
}
void HistoryResourceSource::setProtectedItem(const QString& id) { manager_.setProtectedItem(id); }
bool HistoryResourceSource::canCopy() const { return bool(actions_.copyImage); }
bool HistoryResourceSource::canPin() const { return bool(actions_.pinImage); }
ResourceStatus HistoryResourceSource::outputImage(const QString& id, bool original, bool pin) const
{
    const auto loaded = loadImage(id, {}, original);
    if (!loaded.success)
        return {false, loaded.error};
    const auto& callback = pin ? actions_.pinImage : actions_.copyImage;
    const auto result = callback ? callback(loaded.image)
                                 : ImageOutputResult{false, QStringLiteral("图片输出服务不可用。")};
    return {result.success, result.explanation};
}
void HistoryResourceSource::editGroupName(const QString& groupId, QWidget* parent)
{
    if (!groupActionsEnabled_)
        return;
    const QPointer<HistoryResourceSource> guard(this);
    emit groupDialogOpenChanged(true);
    if (groupId.isEmpty())
    {
        const auto result = createHistoryGroupDialog(manager_, parent);
        if (guard && result && result->success)
            emit groupViewRequested(result->group.id);
    }
    else
        renameHistoryGroupDialog(manager_, groupId, parent);
    if (guard)
        emit groupDialogOpenChanged(false);
}
void HistoryResourceSource::openGroupSettings(const QString& groupId, QWidget* parent)
{
    if (!groupActionsEnabled_)
        return;
    QPointer<GroupSettingsDialog> dialog = new GroupSettingsDialog(
        manager_, groupId, actions_.trash, manager_.protectedItemId(), parent);
    const QPointer<HistoryResourceSource> guard(this);
    emit groupDialogOpenChanged(true);
    dialog->exec();
    delete dialog.data();
    if (guard)
        emit groupDialogOpenChanged(false);
}
void HistoryResourceSource::deleteGroup(const QString& groupId, QWidget* parent)
{
    if (!groupActionsEnabled_)
        return;
    const QPointer<HistoryResourceSource> guard(this);
    emit groupDialogOpenChanged(true);
    const auto result = confirmHistoryGroupDeletion(manager_, groupId, actions_.trash, parent);
    if (guard)
    {
        emit groupDialogOpenChanged(false);
        if (result)
            emit actionFinished({result->success, result->error});
    }
}
void HistoryResourceSource::showGroupMenu(const QString& id, const QPoint& position,
                                          QWidget* parent)
{
    if (!groupActionsEnabled_)
        return;
    for (const auto& group : manager_.groups())
        if (group.id == id)
        {
            QPointer<QMenu> menu = new QMenu(parent);
            menu->setObjectName(QStringLiteral("historyGroupMenu"));
            auto* current =
                menu->addAction(QStringLiteral("设为当前分组"), this,
                                [this, id]
                                {
                                    if (groupActionsEnabled_)
                                    {
                                        const auto result = manager_.setCurrentGroup(id);
                                        emit actionFinished({result.success, result.error});
                                    }
                                });
            current->setObjectName(QStringLiteral("historyGroupSetCurrent"));
            current->setCheckable(true);
            current->setChecked(manager_.currentGroupId() == id);
            menu->addAction(QStringLiteral("分组设置…"), this,
                            [this, id, parent] { openGroupSettings(id, parent); })
                ->setObjectName(QStringLiteral("historyGroupSettings"));
            menu->addAction(QStringLiteral("改名…"), this,
                            [this, id, parent] { editGroupName(id, parent); })
                ->setObjectName(QStringLiteral("historyGroupRename"));
            menu->addAction(QStringLiteral("更改目录…"), this,
                            [this, id, parent]
                            {
                                if (!groupActionsEnabled_)
                                    return;
                                const QPointer<HistoryResourceSource> guard(this);
                                emit groupDialogOpenChanged(true);
                                changeHistoryGroupDirectoryDialog(manager_, id, parent);
                                if (guard)
                                    emit groupDialogOpenChanged(false);
                            })
                ->setObjectName(QStringLiteral("historyGroupChangeDirectory"));
            auto* remove = menu->addAction(QStringLiteral("删除…"), this,
                                           [this, id, parent] { deleteGroup(id, parent); });
            remove->setObjectName(QStringLiteral("historyGroupDelete"));
            remove->setEnabled(!group.isDefault && bool(actions_.trash));
            menu->exec(position);
            delete menu.data();
            return;
        }
}
}
