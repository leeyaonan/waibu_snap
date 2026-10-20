#include "app/history_manager.h"
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
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
    const auto error = settings_.saveHistoryCurrentGroupId(groupId);
    return {error.isEmpty(), error};
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
    snapshot_ = {scanned.groups, {}, scanned.error};
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
HistoryStatus HistoryManager::savePreferences(HistoryPreferences preferences)
{
    const auto validation = cleanupPolicyError(preferences.defaultCleanup);
    if (!validation.isEmpty())
        return {false, validation};
    if (preferences.rootDirectory.isEmpty())
        preferences.rootDirectory = HistoryStore::defaultRootDirectory();
    if (!QDir::isAbsolutePath(preferences.rootDirectory))
        return {false, QStringLiteral("历史根目录必须为绝对路径。")};
    preferences.rootDirectory = QDir::cleanPath(preferences.rootDirectory);
    const bool switching = !ready_ || preferences.rootDirectory != rootDirectory();
    std::unique_ptr<HistoryStore> candidate;
    HistoryScanResult scanned;
    if (switching)
    {
        candidate =
            std::make_unique<HistoryStore>(preferences.rootDirectory, preferences.defaultCleanup);
        scanned = candidate->initialize();
        if (!scanned.error.isEmpty())
            return {false, QStringLiteral("历史根目录切换失败，旧目录保留：%1").arg(scanned.error)};
        preferences.currentGroupId.clear();
        for (const auto& group : scanned.groups)
            if (group.group.isDefault)
                preferences.currentGroupId = group.group.id;
        if (preferences.currentGroupId.isEmpty())
            return {false, QStringLiteral("新历史库缺少默认分组，旧目录保留。")};
    }
    else
        preferences.currentGroupId = currentGroupId();
    auto stored = preferences;
    // 受控根覆盖不因保存模板而写回；只有用户明确选择不同根时才持久切换。
    if (!switching && !rootOverride_.isEmpty())
        stored.rootDirectory = settings_.loadHistoryPreferences().rootDirectory;
    const auto error = settings_.saveHistoryPreferences(stored);
    if (!error.isEmpty())
        return {false, error};
    preferences_ = stored;
    if (switching)
    {
        store_ = std::move(candidate);
        rootOverride_.clear();
        ready_ = true;
        snapshot_ = {scanned.groups, preferences.currentGroupId, {}};
        groups_.clear();
        for (const auto& group : scanned.groups)
            groups_.append(group.group);
        // 保留 sessions_：切回旧根后同一会话仍更新原记录，不重新生成副本。
        status();
        emit groupsChanged();
        emit currentGroupChanged(currentGroupId());
    }
    return {true, {}};
}
GroupUsage HistoryManager::totalUsage(const QString& selectedId)
{
    GroupUsage result;
    for (const auto& group : refresh().groups)
    {
        result.managedCount += group.usage.managedCount;
        result.managedBytes += group.usage.managedBytes;
        result.externalCount += group.usage.externalCount;
        result.externalBytes += group.usage.externalBytes;
        auto policy = group.group.cleanup;
        policy.enabled = false;
        const auto protectedItems = selectCleanupCandidates(
            policy, group.items, QDateTime::currentMSecsSinceEpoch(), selectedId);
        result.protectedCount += protectedItems.protectedCount;
        result.protectedBytes += protectedItems.protectedBytes;
    }
    return result;
}
HistoryStatus HistoryManager::setCurrentGroup(const QString& groupId)
{
    if (!ready_)
        return {false, QStringLiteral("历史库未就绪，无法切换当前分组。")};
    const auto scanned = store_->scan();
    if (!scanned.error.isEmpty())
        return {false, scanned.error};
    bool found = false;
    for (const auto& snapshot : scanned.groups)
        found |= snapshot.group.id == groupId;
    if (!found)
        return {false, QStringLiteral("目标历史分组不存在，当前分组未改变。")};
    const auto persisted = persistCurrentGroup(groupId);
    if (!persisted.success)
        return persisted;
    preferences_.currentGroupId = groupId;
    snapshot_ = {scanned.groups, {}, scanned.error};
    groups_.clear();
    for (const auto& group : scanned.groups)
        groups_.append(group.group);
    emit currentGroupChanged(groupId);
    return {true, {}};
}
CleanupPolicy HistoryManager::defaultCleanupPolicy() const
{
    return settings_.loadHistoryPreferences().defaultCleanup;
}
HistoryGroupResult HistoryManager::createGroup(const QString& name)
{
    if (!ready_)
        return {{false, QStringLiteral("历史库未就绪，无法新建分组。")}, {}};
    // 每次读取当前模板，不重建入库 Store 或会话去重表。
    HistoryStore creator(rootDirectory(), defaultCleanupPolicy());
    const auto result = creator.createGroup(name);
    if (result.success)
    {
        refresh();
        emit groupsChanged();
    }
    return result;
}
HistoryGroupResult HistoryManager::renameGroup(const QString& groupId, const QString& name)
{
    if (!ready_)
        return {{false, QStringLiteral("历史库未就绪，无法改名。")}, {}};
    const auto result = store_->renameGroup(groupId, name);
    if (result.success)
    {
        refresh();
        emit groupsChanged();
    }
    return result;
}
HistoryStatus HistoryManager::saveCleanupPolicy(const QString& groupId, const CleanupPolicy& policy,
                                                bool makeCurrent)
{
    if (!ready_)
        return {false, QStringLiteral("历史库未就绪，无法保存分组设置。")};
    const auto error = cleanupPolicyError(policy);
    if (!error.isEmpty())
        return {false, error};
    const QString previous = currentGroupId();
    const bool switching = makeCurrent && previous != groupId;
    // 先验证偏好可写；策略失败时恢复旧 UUID，成功前不发布内存状态或信号。
    if (switching)
    {
        const auto persisted = persistCurrentGroup(groupId);
        if (!persisted.success)
            return persisted;
    }
    auto result = store_->saveCleanupPolicy(groupId, policy);
    if (!result.success && switching)
    {
        const auto restored = persistCurrentGroup(previous);
        if (!restored.success)
            result.error += QStringLiteral("；恢复当前分组偏好失败：") + restored.error;
    }
    if (result.success)
    {
        if (switching)
            preferences_.currentGroupId = groupId;
        refresh();
        emit groupsChanged();
        if (switching)
            emit currentGroupChanged(groupId);
    }
    return result;
}
HistoryStatus HistoryManager::deleteGroup(const QString& groupId, const HistoryTrash& trash)
{
    if (!ready_)
        return {false, QStringLiteral("历史库未就绪，无法删除分组。")};
    if (!trash)
        return {false, QStringLiteral("回收站服务不可用，分组已保留。")};
    const auto scanned = store_->scan();
    if (!scanned.error.isEmpty())
        return {false, scanned.error};
    QString defaultId;
    for (const auto& group : scanned.groups)
        if (group.group.isDefault)
            defaultId = group.group.id;
    if (defaultId.isEmpty())
        return {false, QStringLiteral("默认分组缺失，已拒绝删除。")};
    const bool switching = currentGroupId() == groupId;
    bool persisted = false;
    const HistoryTrash move = [&](const QString& path)
    {
        // 根锁内已验证默认组和路径；偏好落盘失败时不调用回收站。
        if (switching)
        {
            const auto saved = persistCurrentGroup(defaultId);
            if (!saved.success)
                return saved;
            persisted = true;
        }
        return trash(path);
    };
    const auto result = store_->deleteGroup(groupId, move);
    if (!result.success)
    {
        if (persisted)
        {
            const auto restored = persistCurrentGroup(groupId);
            if (!restored.success)
                return {false,
                        result.error + QStringLiteral("；恢复当前分组偏好失败：") + restored.error};
        }
        return result;
    }
    if (switching)
        preferences_.currentGroupId = defaultId;
    refresh();
    emit groupsChanged();
    if (switching)
        emit currentGroupChanged(defaultId);
    return result;
}
HistorySession HistoryManager::beginSession() const
{
    return {QUuid::createUuid().toString(QUuid::WithoutBraces), currentGroupId()};
}
HistorySnapshot HistoryManager::snapshot() const
{
    auto result = snapshot_;
    result.currentGroupId = currentGroupId();
    return result;
}
HistorySnapshot HistoryManager::refresh()
{
    if (!store_)
        snapshot_ = {{}, {}, QStringLiteral("历史库未初始化，请稍后重试。")};
    else
    {
        const auto scanned = store_->scan();
        snapshot_ = {scanned.groups, {}, scanned.error};
    }
    groups_.clear();
    for (const auto& group : snapshot_.groups)
        if (!group.group.id.isEmpty())
            groups_.append(group.group);
    return snapshot();
}
std::optional<HistoryItem> HistoryManager::findItem(const QString& itemId) const
{
    for (const auto& group : snapshot_.groups)
        for (const auto& item : group.items)
            if (historyItemId(item) == itemId)
                return item;
    return {};
}
HistoryStatus HistoryManager::setStarred(const QString& itemId, bool starred)
{
    const auto item = findItem(itemId);
    if (!item || !store_)
        return {false, QStringLiteral("历史记录已不存在，请刷新历史。")};
    const auto result = store_->setStarred(item->groupId, item->fileName, starred);
    if (result.success)
        refresh();
    return result;
}
HistoryStatus HistoryManager::deleteItem(const QString& itemId, const HistoryTrash& trash)
{
    const auto item = findItem(itemId);
    if (!item || !store_)
        return {false, QStringLiteral("历史记录已不存在，请刷新历史。")};
    const auto result = store_->deleteManagedItem(*item, trash);
    if (result.success)
        refresh();
    return result;
}
HistoryStatus HistoryManager::revealItem(const QString& itemId, const HistoryTrash& reveal) const
{
    const auto item = findItem(itemId);
    if (!item || !store_)
        return {false, QStringLiteral("历史记录已不存在，请刷新历史。")};
    const auto checked = store_->validateItem(*item);
    if (!checked.success)
        return checked;
    return reveal ? reveal(item->absolutePath)
                  : HistoryStatus{false, QStringLiteral("文件管理器服务不可用，请稍后重试。")};
}
HistoryImageResult HistoryManager::loadImage(const QString& itemId, QSize bounds) const
{
    // 路径只能来自本次扫描的记录；视图不传入任意文件路径。
    for (const auto& group : snapshot_.groups)
        for (const auto& item : group.items)
            if (historyItemId(item) == itemId)
            {
                const auto checked = store_->validateItem(item);
                if (!checked.success)
                    return {checked, {}};
                if (QFileInfo(item.fileName)
                            .suffix()
                            .compare(QLatin1String("webp"), Qt::CaseInsensitive) == 0 &&
                    !QImageReader::supportedImageFormats().contains("webp"))
                    return {
                        {false, QStringLiteral("缺少 Qt WebP 图片插件（qtimageformats），请安装 "
                                               "Qt 6.11.2 的 Qt Image Formats 后重试。")},
                        {}};
                QImageReader reader(item.absolutePath);
                reader.setAutoTransform(true);
                if (bounds.isValid() && reader.size().isValid())
                    reader.setScaledSize(reader.size().scaled(bounds, Qt::KeepAspectRatio));
                auto image = reader.read();
                if (image.isNull())
                    return {{false, QStringLiteral("图片已丢失、无法读取或格式损坏，请刷新历史。%1")
                                        .arg(reader.errorString())},
                            {}};
                image.setDevicePixelRatio(1);
                return {{true, {}}, image};
            }
    return {{false, QStringLiteral("历史记录已不存在，请刷新历史。")}, {}};
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
