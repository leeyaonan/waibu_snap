#include "app/history_store.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageWriter>
#include <QJsonDocument>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QTimeZone>
#include <QUuid>
#include <cmath>
namespace waibusnap
{
namespace
{
QString newId() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
QString metadataPath(const QString& groupPath, const QString& file)
{
    return QDir(QDir(groupPath).filePath(QStringLiteral(".waibusnap"))).filePath(file);
}
QString groupFile(const QString& path) { return metadataPath(path, QStringLiteral("group.json")); }
QString itemsFile(const QString& path) { return metadataPath(path, QStringLiteral("items.json")); }
bool validUuid(const QString& id)
{
    return !QUuid(id).isNull() && QUuid(id).toString(QUuid::WithoutBraces) == id;
}
bool validTimestamp(const QJsonValue& value)
{
    const double number = value.toDouble(-1);
    return value.isDouble() && number >= 0 && number <= 9007199254740991.0 &&
           std::floor(number) == number;
}
bool imageFileName(const QString& name)
{
    if (name.isEmpty() || name.startsWith(QLatin1Char('.')) || name.contains(QLatin1Char('/')) ||
        name.contains(QLatin1Char('\\')) || name.contains(QLatin1Char(':')))
        return false;
    const QString suffix = QFileInfo(name).suffix().toLower();
    return suffix == QStringLiteral("png") || suffix == QStringLiteral("jpg") ||
           suffix == QStringLiteral("jpeg") || suffix == QStringLiteral("webp");
}
QString sourceText(HistorySource source)
{
    switch (source)
    {
    case HistorySource::Overlay:
        return QStringLiteral("overlay");
    case HistorySource::Scroll:
        return QStringLiteral("scroll");
    case HistorySource::External:
        return QStringLiteral("external");
    }
    return {};
}
bool managedFileName(const QString& name)
{
    static const QRegularExpression pattern(
        QStringLiteral(R"(^WaibuSnap_[0-9]{8}_[0-9]{6}_[0-9]{3}_[0-9a-f]{6}\.png$)"));
    return pattern.match(name).hasMatch();
}
QString readJson(const QString& path, QJsonObject& object)
{
    QFile file(path);
    if (QFileInfo(path).isSymLink() || !file.open(QIODevice::ReadOnly))
        return QStringLiteral("无法读取历史元数据：%1；%2").arg(path, file.errorString());
    const QByteArray bytes = file.readAll();
    if (file.error() != QFileDevice::NoError)
        return QStringLiteral("读取历史元数据失败：%1；%2").arg(path, file.errorString());
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(bytes, &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject() ||
        document.object().value(QStringLiteral("schemaVersion")) != QJsonValue(1))
        return QStringLiteral("历史元数据损坏或版本不支持：%1；该组清理已暂停。").arg(path);
    object = document.object();
    return {};
}
QString writeJson(const QString& path, const QJsonObject& object)
{
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (QFileInfo(path).isSymLink() || !file.open(QIODevice::WriteOnly))
        return QStringLiteral("无法写入历史元数据：%1；%2").arg(path, file.errorString());
    const QByteArray bytes = QJsonDocument(object).toJson();
    if (file.write(bytes) != bytes.size() || !file.commit())
        return QStringLiteral("历史元数据原子提交失败：%1；%2").arg(path, file.errorString());
    return {};
}
QJsonObject groupJson(const HistoryGroup& group)
{
    return {{QStringLiteral("schemaVersion"), 1},
            {QStringLiteral("id"), group.id},
            {QStringLiteral("default"), group.isDefault},
            {QStringLiteral("createdAt"), group.createdAtMs},
            {QStringLiteral("cleanup"), cleanupPolicyToJson(group.cleanup)}};
}
QString readGroup(const QFileInfo& directory, HistoryGroup& group)
{
    group.name = directory.fileName();
    group.absolutePath = directory.absoluteFilePath();
    QJsonObject json;
    const QString error = readJson(groupFile(group.absolutePath), json);
    if (!error.isEmpty())
        return error;
    if (!validUuid(json.value(QStringLiteral("id")).toString()) ||
        !json.value(QStringLiteral("default")).isBool() ||
        !validTimestamp(json.value(QStringLiteral("createdAt"))) ||
        !json.value(QStringLiteral("cleanup")).isObject() ||
        !cleanupPolicyFromJson(json.value(QStringLiteral("cleanup")).toObject(), group.cleanup))
        return QStringLiteral("分组元数据字段无效：%1；该组清理已暂停。").arg(group.name);
    group.id = json.value(QStringLiteral("id")).toString();
    group.isDefault = json.value(QStringLiteral("default")).toBool();
    group.createdAtMs = json.value(QStringLiteral("createdAt")).toInteger();
    return {};
}
QString readIndex(const QString& groupPath, QJsonObject& index)
{
    const QString path = itemsFile(groupPath);
    // 无索引时全部按外部图片处理；绝不靠文件名重建受管身份。
    if (!QFileInfo::exists(path) && !QFileInfo(path).isSymLink())
        return {};
    QJsonObject json;
    QString error = readJson(path, json);
    if (!error.isEmpty())
        return error;
    if (!json.value(QStringLiteral("items")).isObject())
        return QStringLiteral("历史索引缺少记录对象：%1；该组清理已暂停。").arg(path);
    const auto entries = json.value(QStringLiteral("items")).toObject();
    for (auto it = entries.begin(); it != entries.end(); ++it)
    {
        const auto item = it.value().toObject();
        const bool managed = item.value(QStringLiteral("managed")).toBool();
        const QString source = item.value(QStringLiteral("source")).toString();
        if (!imageFileName(it.key()) || !it.value().isObject() ||
            !item.value(QStringLiteral("starred")).isBool() ||
            !item.value(QStringLiteral("managed")).isBool() ||
            (managed &&
             (!managedFileName(it.key()) ||
              (source != QStringLiteral("overlay") && source != QStringLiteral("scroll")) ||
              !validTimestamp(item.value(QStringLiteral("createdAt"))))) ||
            (!managed && source != QStringLiteral("external")) ||
            (item.contains(QStringLiteral("createdAt")) &&
             !validTimestamp(item.value(QStringLiteral("createdAt")))))
            return QStringLiteral("历史索引记录无效：%1；该组清理已暂停。").arg(path);
    }
    index = entries;
    return {};
}
QString saveIndex(const QString& path, const QJsonObject& index)
{
    return writeJson(itemsFile(path),
                     {{QStringLiteral("schemaVersion"), 1}, {QStringLiteral("items"), index}});
}
QString rootError(const QString& root)
{
    const QFileInfo info(root);
    if (!QDir::isAbsolutePath(root) || !info.isDir() || info.isSymLink() || !info.isReadable())
        return QStringLiteral("历史根目录不存在、不可读或不是普通绝对目录：%1").arg(root);
    return {};
}
QString lockError(QLockFile& lock)
{
    if (!lock.tryLock(0))
        return QStringLiteral("历史库正在被其他操作使用，或无法创建锁文件，请稍后重试。");
    return {};
}
QStringList rootNames(const QString& root, const QString& except = {})
{
    QStringList names;
    for (const auto& info : QDir(root).entryInfoList(QDir::AllEntries | QDir::Hidden |
                                                     QDir::System | QDir::NoDotAndDotDot))
        if (info.fileName() != except)
            names.append(info.fileName());
    return names;
}
const HistoryGroupSnapshot* findGroup(const HistoryScanResult& scan, const QString& id,
                                      QString& error)
{
    const HistoryGroupSnapshot* found = nullptr;
    if (id.isEmpty())
        error = QStringLiteral("分组 UUID 不能为空。");
    else
        for (const auto& snapshot : scan.groups)
            if (snapshot.group.id == id)
            {
                if (found)
                {
                    error = QStringLiteral("分组 UUID 重复且未能修复，已停止操作。");
                    return nullptr;
                }
                found = &snapshot;
            }
    if (found && !found->error.isEmpty())
        error = found->error;
    if (!found && error.isEmpty())
        error = scan.error.isEmpty() ? QStringLiteral("找不到指定的历史分组。") : scan.error;
    return error.isEmpty() ? found : nullptr;
}
}
HistoryStore::HistoryStore(QString rootDirectory, CleanupPolicy defaultCleanup, HistoryTrash trash)
    : rootDirectory_(rootDirectory.isEmpty() ? defaultRootDirectory() : std::move(rootDirectory)),
      defaultCleanup_(defaultCleanup), trash_(std::move(trash))
{
}
QString HistoryStore::defaultRootDirectory()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::PicturesLocation))
        .filePath(QStringLiteral("WaibuSnap"));
}
HistoryScanResult HistoryStore::scanUnlocked() const
{
    HistoryScanResult result;
    result.error = rootError(rootDirectory_);
    if (!result.error.isEmpty())
        return result;
    QSet<QString> ids;
    bool hasDefault = false;
    // 按文件夹名固定顺序修复复制目录，保证“后出现”可重复测试。
    for (const auto& info :
         QDir(rootDirectory_)
             .entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name))
    {
        if (info.fileName().startsWith(QLatin1Char('.')) ||
            !QFileInfo::exists(groupFile(info.absoluteFilePath())))
            continue;
        HistoryGroupSnapshot snapshot;
        const QString meta = QDir(info.absoluteFilePath()).filePath(QStringLiteral(".waibusnap"));
        if (QFileInfo(meta).isSymLink())
        {
            snapshot.group.name = info.fileName();
            snapshot.group.absolutePath = info.absoluteFilePath();
            snapshot.error = QStringLiteral("分组元数据目录不能是符号链接：%1").arg(meta);
        }
        else
            snapshot.error = readGroup(info, snapshot.group);
        if (snapshot.error.isEmpty())
        {
            if (ids.contains(snapshot.group.id))
            {
                snapshot.group.id = newId();
                // 复制默认组也只能保留一个默认身份。
                if (snapshot.group.isDefault && hasDefault)
                    snapshot.group.isDefault = false;
                snapshot.error =
                    writeJson(groupFile(info.absoluteFilePath()), groupJson(snapshot.group));
            }
            else if (snapshot.group.isDefault && hasDefault)
                snapshot.error = QStringLiteral("发现多个默认分组，已暂停该组操作，请检查元数据。");
            ids.insert(snapshot.group.id);
            hasDefault = hasDefault || snapshot.group.isDefault;
        }
        QJsonObject index;
        if (snapshot.error.isEmpty())
            snapshot.error = readIndex(info.absoluteFilePath(), index);
        for (const auto& file : QDir(info.absoluteFilePath())
                                    .entryInfoList(QDir::Files | QDir::NoSymLinks, QDir::Name))
        {
            if (!imageFileName(file.fileName()))
                continue;
            const auto metadata = index.value(file.fileName()).toObject();
            HistoryItem item;
            item.groupId = snapshot.group.id;
            item.fileName = file.fileName();
            item.absolutePath = file.absoluteFilePath();
            item.bytes = file.size();
            item.createdAtMs = metadata.contains(QStringLiteral("createdAt"))
                                   ? metadata.value(QStringLiteral("createdAt")).toInteger()
                                   : qMax(qint64(0), file.lastModified().toMSecsSinceEpoch());
            item.starred = metadata.value(QStringLiteral("starred")).toBool();
            item.managed = metadata.value(QStringLiteral("managed")).toBool();
            if (item.managed)
                item.source =
                    metadata.value(QStringLiteral("source")) == QJsonValue(QStringLiteral("scroll"))
                        ? HistorySource::Scroll
                        : HistorySource::Overlay;
            snapshot.items.append(item);
        }
        snapshot.items = sortedHistoryItems(std::move(snapshot.items), HistorySort::OldestFirst);
        snapshot.usage = historyUsage(snapshot.items);
        snapshot.cleanupPaused = !snapshot.error.isEmpty();
        if (snapshot.cleanupPaused)
        {
            snapshot.usage.protectedCount = snapshot.usage.managedCount;
            snapshot.usage.protectedBytes = snapshot.usage.managedBytes;
            if (!result.error.isEmpty())
                result.error += QLatin1Char('\n');
            result.error += snapshot.error;
        }
        else
        {
            const auto cleanup = selectCleanupCandidates(snapshot.group.cleanup, snapshot.items,
                                                         QDateTime::currentMSecsSinceEpoch());
            snapshot.usage.protectedCount = cleanup.protectedCount;
            snapshot.usage.protectedBytes = cleanup.protectedBytes;
        }
        result.groups.append(snapshot);
    }
    return result;
}
HistoryScanResult HistoryStore::scan() const
{
    QString error = rootError(rootDirectory_);
    if (!error.isEmpty())
        return {{}, error};
    QLockFile lock(QDir(rootDirectory_).filePath(QStringLiteral(".waibusnap.lock")));
    error = lockError(lock);
    return error.isEmpty() ? scanUnlocked() : HistoryScanResult{{}, error};
}
HistoryScanResult HistoryStore::initialize()
{
    if (!QDir::isAbsolutePath(rootDirectory_) || !QDir().mkpath(rootDirectory_))
        return {{}, QStringLiteral("无法创建历史根目录：%1").arg(rootDirectory_)};
    QString error = rootError(rootDirectory_);
    if (!error.isEmpty())
        return {{}, error};
    QLockFile lock(QDir(rootDirectory_).filePath(QStringLiteral(".waibusnap.lock")));
    error = lockError(lock);
    if (!error.isEmpty())
        return {{}, error};
    auto result = scanUnlocked();
    if (!result.error.isEmpty())
        return result;
    for (const auto& snapshot : result.groups)
        if (snapshot.group.isDefault)
            return result;
    QString name = QStringLiteral("默认分组");
    const auto names = rootNames(rootDirectory_);
    for (int suffix = 2; !historyGroupNameError(name, names).isEmpty(); ++suffix)
        name = QStringLiteral("默认分组-%1").arg(suffix);
    const auto created = createGroupUnlocked(name, true);
    if (!created.success)
    {
        result.error = created.error;
        return result;
    }
    return scanUnlocked();
}
HistoryGroupResult HistoryStore::createGroupUnlocked(const QString& name, bool isDefault)
{
    QString error = historyGroupNameError(name, rootNames(rootDirectory_));
    if (error.isEmpty())
        error = cleanupPolicyError(defaultCleanup_);
    if (!error.isEmpty())
        return {{false, error}, {}};
    HistoryGroup group{newId(),   normalizedHistoryGroupName(name),    {},
                       isDefault, QDateTime::currentMSecsSinceEpoch(), defaultCleanup_};
    group.absolutePath = QDir(rootDirectory_).filePath(group.name);
    // mkdir 不接管既有目录，包括名字竞争；只在新目录中建立 marker。
    if (!QDir(rootDirectory_).mkdir(group.name))
        return {{false, QStringLiteral("无法创建分组目录，目录可能已存在：%1").arg(group.name)},
                {}};
    if (!QDir(group.absolutePath).mkdir(QStringLiteral(".waibusnap")))
        return {{false, QStringLiteral("无法创建分组元数据目录，空目录已保留。")}, {}};
    error = saveIndex(group.absolutePath, {});
    if (error.isEmpty())
        error = writeJson(groupFile(group.absolutePath), groupJson(group));
    return {{error.isEmpty(), error}, group};
}
HistoryGroupResult HistoryStore::createGroup(const QString& name)
{
    QString error = rootError(rootDirectory_);
    if (!error.isEmpty())
        return {{false, error}, {}};
    QLockFile lock(QDir(rootDirectory_).filePath(QStringLiteral(".waibusnap.lock")));
    error = lockError(lock);
    if (!error.isEmpty())
        return {{false, error}, {}};
    return createGroupUnlocked(name, false);
}
HistoryGroupResult HistoryStore::renameGroup(const QString& groupId, const QString& name)
{
    QString error = rootError(rootDirectory_);
    if (!error.isEmpty())
        return {{false, error}, {}};
    QLockFile lock(QDir(rootDirectory_).filePath(QStringLiteral(".waibusnap.lock")));
    error = lockError(lock);
    if (!error.isEmpty())
        return {{false, error}, {}};
    const auto scanned = scanUnlocked();
    const auto* snapshot = findGroup(scanned, groupId, error);
    if (!snapshot)
        return {{false, error}, {}};
    HistoryGroup group = snapshot->group;
    error = historyGroupNameError(name, rootNames(rootDirectory_, group.name));
    if (!error.isEmpty())
        return {{false, error}, {}};
    const QString clean = normalizedHistoryGroupName(name);
    if (clean == group.name)
        return {{true, {}}, group};
    if (!QDir(rootDirectory_).rename(group.name, clean))
        return {{false, QStringLiteral("分组改名失败，原目录和 UUID 已保留。")}, {}};
    group.name = clean;
    group.absolutePath = QDir(rootDirectory_).filePath(clean);
    return {{true, {}}, group};
}
HistoryStatus HistoryStore::deleteGroup(const QString& groupId, const HistoryTrash& trash)
{
    QString error = rootError(rootDirectory_);
    if (!error.isEmpty())
        return {false, error};
    QLockFile lock(QDir(rootDirectory_).filePath(QStringLiteral(".waibusnap.lock")));
    error = lockError(lock);
    if (!error.isEmpty())
        return {false, error};
    const auto scanned = scanUnlocked();
    const auto* snapshot = findGroup(scanned, groupId, error);
    if (!snapshot)
        return {false, error};
    if (snapshot->group.isDefault)
        return {false, QStringLiteral("默认分组不能删除，可以改名。")};
    const auto& callback = trash ? trash : trash_;
    if (!callback)
        return {false, QStringLiteral("未提供回收站服务，分组和图片已保留。")};
    const auto result = callback(snapshot->group.absolutePath);
    if (!result.success)
        return {false, QStringLiteral("分组移入回收站失败，原分组已保留：%1").arg(result.error)};
    if (QFileInfo::exists(snapshot->group.absolutePath))
        return {false, QStringLiteral("回收站服务返回成功但原分组仍存在，请重试。")};
    return {true, {}};
}
HistoryStatus HistoryStore::saveCleanupPolicy(const QString& groupId, const CleanupPolicy& policy)
{
    QString error = cleanupPolicyError(policy);
    if (error.isEmpty())
        error = rootError(rootDirectory_);
    if (!error.isEmpty())
        return {false, error};
    QLockFile lock(QDir(rootDirectory_).filePath(QStringLiteral(".waibusnap.lock")));
    error = lockError(lock);
    if (!error.isEmpty())
        return {false, error};
    const auto scanned = scanUnlocked();
    const auto* snapshot = findGroup(scanned, groupId, error);
    if (!snapshot)
        return {false, error};
    auto group = snapshot->group;
    group.cleanup = policy;
    error = writeJson(groupFile(group.absolutePath), groupJson(group));
    return {error.isEmpty(), error};
}
HistoryWriteResult HistoryStore::writeManagedPng(const QString& groupId, const QImage& image,
                                                 HistorySource source,
                                                 const QString& existingFileName)
{
    if (image.isNull() || (source != HistorySource::Overlay && source != HistorySource::Scroll))
        return {{false, QStringLiteral("受管历史写入需要有效图片与截图来源。")}, {}};
    QString error = rootError(rootDirectory_);
    if (!error.isEmpty())
        return {{false, error}, {}};
    QLockFile lock(QDir(rootDirectory_).filePath(QStringLiteral(".waibusnap.lock")));
    error = lockError(lock);
    if (!error.isEmpty())
        return {{false, error}, {}};
    const auto scanned = scanUnlocked();
    const auto* snapshot = findGroup(scanned, groupId, error);
    if (!snapshot)
        return {{false, error}, {}};
    QJsonObject index;
    error = readIndex(snapshot->group.absolutePath, index);
    if (!error.isEmpty())
        return {{false, error}, {}};
    HistoryItem item;
    if (!existingFileName.isEmpty())
    {
        for (const auto& previous : snapshot->items)
            if (previous.fileName == existingFileName && previous.managed &&
                previous.source == source)
                item = previous;
        if (item.fileName.isEmpty())
            return {{false, QStringLiteral("更新目标不是同来源的既有受管图片，已拒绝覆盖。")}, {}};
    }
    QImage pixels = image;
    pixels.setDevicePixelRatio(1);
    if (existingFileName.isEmpty())
    {
        // 临时文件编码后 rename 原子发布；QFile 的 rename 不覆盖竞争中出现的用户文件。
        QTemporaryFile temporary(
            QDir(snapshot->group.absolutePath).filePath(QStringLiteral(".history-XXXXXX")));
        if (!temporary.open())
            return {{false,
                     QStringLiteral("无法创建历史 PNG 临时文件：%1").arg(temporary.errorString())},
                    {}};
        QImageWriter writer(&temporary, "png");
        if (!writer.write(pixels) || !temporary.flush())
            return {
                {false, QStringLiteral("历史 PNG 编码或写入失败：%1").arg(writer.errorString())},
                {}};
        temporary.close();
        item.groupId = groupId;
        item.managed = true;
        item.source = source;
        item.createdAtMs = QDateTime::currentMSecsSinceEpoch();
        bool published = false;
        for (int attempt = 0; attempt < 32 && !published; ++attempt)
        {
            item.fileName =
                QStringLiteral("WaibuSnap_%1_%2.png")
                    .arg(QDateTime::fromMSecsSinceEpoch(item.createdAtMs, QTimeZone::UTC)
                             .toString(QStringLiteral("yyyyMMdd_HHmmss_zzz")),
                         newId().remove(QLatin1Char('-')).left(6));
            item.absolutePath = QDir(snapshot->group.absolutePath).filePath(item.fileName);
            if (index.contains(item.fileName))
                continue;
            published = temporary.rename(item.absolutePath);
            if (!published && !QFileInfo::exists(item.absolutePath) &&
                !QFileInfo(item.absolutePath).isSymLink())
                return {{false,
                         QStringLiteral("历史 PNG 原子发布失败：%1").arg(temporary.errorString())},
                        {}};
        }
        if (!published)
            return {{false, QStringLiteral("历史文件名多次冲突，请重试。")}, {}};
        temporary.setAutoRemove(false);
        index.insert(item.fileName, QJsonObject{{QStringLiteral("starred"), false},
                                                {QStringLiteral("managed"), true},
                                                {QStringLiteral("source"), sourceText(source)},
                                                {QStringLiteral("createdAt"), item.createdAtMs}});
        error = saveIndex(snapshot->group.absolutePath, index);
        if (!error.isEmpty())
            return {{false, error + QStringLiteral(
                                        "；PNG 已保留，扫描将按外部图片显示，未声明入库成功。")},
                    item};
    }
    else
    {
        QSaveFile file(item.absolutePath);
        file.setDirectWriteFallback(false);
        if (QFileInfo(item.absolutePath).isSymLink() || !file.open(QIODevice::WriteOnly))
            return {
                {false, QStringLiteral("无法打开受管 PNG，原图已保留：%1").arg(file.errorString())},
                {}};
        QImageWriter writer(&file, "png");
        if (!writer.write(pixels) || !file.commit())
            return {
                {false,
                 QStringLiteral("受管 PNG 原子更新失败，原图已保留：%1").arg(file.errorString())},
                {}};
    }
    item.bytes = QFileInfo(item.absolutePath).size();
    item.pixelSize = pixels.size();
    return {{true, {}}, item};
}
HistoryStatus HistoryStore::setStarred(const QString& groupId, const QString& fileName,
                                       bool starred)
{
    QString error = rootError(rootDirectory_);
    if (!error.isEmpty())
        return {false, error};
    QLockFile lock(QDir(rootDirectory_).filePath(QStringLiteral(".waibusnap.lock")));
    error = lockError(lock);
    if (!error.isEmpty())
        return {false, error};
    const auto scanned = scanUnlocked();
    const auto* snapshot = findGroup(scanned, groupId, error);
    if (!snapshot)
        return {false, error};
    const HistoryItem* found = nullptr;
    for (const auto& item : snapshot->items)
        if (item.fileName == fileName)
            found = &item;
    if (!found)
        return {false, QStringLiteral("找不到要切换星标的图片，文件可能已被移动或删除。")};
    QJsonObject index;
    error = readIndex(snapshot->group.absolutePath, index);
    if (!error.isEmpty())
        return {false, error};
    if (!found->managed && !starred)
        index.remove(fileName);
    else
        index.insert(fileName, QJsonObject{{QStringLiteral("starred"), starred},
                                           {QStringLiteral("managed"), found->managed},
                                           {QStringLiteral("source"), sourceText(found->source)},
                                           {QStringLiteral("createdAt"), found->createdAtMs}});
    error = saveIndex(snapshot->group.absolutePath, index);
    return {error.isEmpty(), error};
}
HistoryStatus HistoryStore::validateItem(const HistoryItem& item) const
{
    const QFileInfo root(rootDirectory_);
    const QFileInfo file(item.absolutePath);
    const QFileInfo group(file.absolutePath());
    const QFileInfo metadata(QDir(group.absoluteFilePath()).filePath(QStringLiteral(".waibusnap")));
    const QString expected = QDir(group.absoluteFilePath()).filePath(item.fileName);
    if (!rootError(rootDirectory_).isEmpty() || !imageFileName(item.fileName) ||
        item.absolutePath != expected || !file.isFile() || file.isSymLink() || root.isJunction() ||
        !group.isDir() || group.isSymLink() || group.isJunction() ||
        group.canonicalFilePath() != QDir(root.canonicalFilePath()).filePath(group.fileName()) ||
        QFileInfo(group.absolutePath()).canonicalFilePath() != root.canonicalFilePath() ||
        file.canonicalFilePath() != QDir(group.canonicalFilePath()).filePath(item.fileName) ||
        metadata.isSymLink() || metadata.isJunction())
        return {false, QStringLiteral("图片已丢失、路径越界或包含符号链接，请刷新历史。")};
    return {true, {}};
}
HistoryStatus HistoryStore::deleteManagedItem(const HistoryItem& item, const HistoryTrash& trash)
{
    auto checked = validateItem(item);
    if (!checked.success)
        return checked;
    if (!item.managed || !managedFileName(item.fileName))
        return {false, QStringLiteral("外部图片请在文件管理器中操作。")};
    QLockFile lock(QDir(rootDirectory_).filePath(QStringLiteral(".waibusnap.lock")));
    QString error = lockError(lock);
    if (!error.isEmpty())
        return {false, error};
    const auto scanned = scanUnlocked();
    const auto* group = findGroup(scanned, item.groupId, error);
    if (!group)
        return {false, error};
    bool matched = false;
    for (const auto& current : group->items)
        matched |= current.managed && current.fileName == item.fileName &&
                   current.absolutePath == item.absolutePath && current.source == item.source;
    if (!matched)
        return {false, QStringLiteral("目标不是当前扫描的受管历史项，已拒绝删除。")};
    if (!trash)
        return {false, QStringLiteral("未提供回收站服务，原文件和索引已保留。")};
    QJsonObject index;
    error = readIndex(group->group.absolutePath, index);
    if (!error.isEmpty())
        return {false, error};
    index.remove(item.fileName);
    QSaveFile staged(itemsFile(group->group.absolutePath));
    staged.setDirectWriteFallback(false);
    const QByteArray json = QJsonDocument(QJsonObject{{QStringLiteral("schemaVersion"), 1},
                                                      {QStringLiteral("items"), index}})
                                .toJson();
    if (!staged.open(QIODevice::WriteOnly) || staged.write(json) != json.size())
        return {
            false,
            QStringLiteral("无法暂存历史索引，原文件和索引已保留：%1").arg(staged.errorString())};
    // 回收站与索引不能跨文件原子提交。保留同目录恢复副本，索引失败时恢复原图。
    QTemporaryFile backup(
        QDir(group->group.absolutePath).filePath(QStringLiteral(".history-delete-XXXXXX")));
    QFile original(item.absolutePath);
    if (!backup.open() || !original.open(QIODevice::ReadOnly))
        return {false, QStringLiteral("无法准备删除恢复副本，原图和索引已保留。")};
    while (!original.atEnd())
    {
        const QByteArray block = original.read(1024 * 1024);
        if (original.error() != QFileDevice::NoError || backup.write(block) != block.size())
            return {false, QStringLiteral("删除恢复副本写入失败，原图和索引已保留。")};
    }
    if (!backup.flush())
        return {false, QStringLiteral("删除恢复副本提交失败，原图和索引已保留。")};
    original.close();
    backup.close();
    checked = validateItem(item);
    if (!checked.success)
        return checked;
    const auto moved = trash(item.absolutePath);
    if (!moved.success)
        error = QStringLiteral("图片移入回收站失败：%1").arg(moved.error);
    else if (QFileInfo::exists(item.absolutePath) || QFileInfo(item.absolutePath).isSymLink())
        error = QStringLiteral("回收站返回成功但原文件仍存在，索引已保留。");
    else if (!staged.commit())
        error = QStringLiteral("历史索引提交失败，已尝试恢复原图：%1").arg(staged.errorString());
    else
        return {true, {}};
    if (!QFileInfo::exists(item.absolutePath) && !QFileInfo(item.absolutePath).isSymLink())
    {
        if (backup.rename(item.absolutePath))
            backup.setAutoRemove(false);
        else
        {
            backup.setAutoRemove(false);
            error += QStringLiteral("；原图恢复失败，恢复副本保留于：%1").arg(backup.fileName());
        }
    }
    return {false, error};
}
HistoryItemsResult HistoryStore::itemsForGroup(const QString& groupId, HistorySort order,
                                               const QString& selectedId) const
{
    const auto scanned = scan();
    QString error;
    const auto* snapshot = findGroup(scanned, groupId, error);
    if (!snapshot)
        return {{false, error}, {}, {}};
    auto usage = snapshot->usage;
    const auto cleanup = selectCleanupCandidates(snapshot->group.cleanup, snapshot->items,
                                                 QDateTime::currentMSecsSinceEpoch(), selectedId);
    usage.protectedCount = cleanup.protectedCount;
    usage.protectedBytes = cleanup.protectedBytes;
    return {{true, {}}, sortedHistoryItems(snapshot->items, order), usage};
}
HistoryCleanupResult HistoryStore::cleanupCandidates(const QString& groupId, qint64 nowMs,
                                                     const QString& selectedId) const
{
    const auto scanned = scan();
    QString error;
    const auto* snapshot = findGroup(scanned, groupId, error);
    if (!snapshot)
    {
        HistoryCleanupResult result;
        result.error = error;
        return result;
    }
    return selectCleanupCandidates(snapshot->group.cleanup, snapshot->items, nowMs, selectedId);
}
}
