#include "app/history_store.h"
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QTimeZone>
#include <QUuid>
#include <algorithm>
#include <cmath>
namespace waibusnap
{
namespace
{
QString readJson(const QString& path, QJsonObject& object);
QString writeJson(const QString& path, const QJsonObject& object);
QString newId() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
QString metadataPath(const QString& groupPath, const QString& file)
{
    return QDir(QDir(groupPath).filePath(QStringLiteral(".waibusnap"))).filePath(file);
}
QString groupFile(const QString& path) { return metadataPath(path, QStringLiteral("group.json")); }
QString itemsFile(const QString& path) { return metadataPath(path, QStringLiteral("items.json")); }
QString originalsDirectory(const QString& groupPath)
{
    return QDir(QDir(groupPath).filePath(QStringLiteral(".waibusnap")))
        .filePath(QStringLiteral("originals"));
}
QString originalFile(const QString& groupPath, const QString& fileName)
{
    return QDir(originalsDirectory(groupPath)).filePath(fileName);
}
QString writePng(const QString& path, const QImage& image)
{
    if (image.isNull())
        return QStringLiteral("水印原图为空。");
    if (QFileInfo(path).isSymLink() || QFileInfo(QFileInfo(path).absolutePath()).isSymLink())
        return QStringLiteral("水印原图路径不能是符号链接：%1").arg(path);
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return QStringLiteral("无法创建水印原图目录：%1").arg(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly))
        return QStringLiteral("无法打开水印原图：%1").arg(file.errorString());
    QImage copy = image;
    copy.setDevicePixelRatio(1);
    QImageWriter writer(&file, "png");
    if (!writer.write(copy) || !file.commit())
        return QStringLiteral("水印原图写入失败：%1").arg(file.errorString());
    return {};
}

QString registryFile(const QString& root)
{
    return metadataPath(root, QStringLiteral("groups.json"));
}
QString readRegistry(const QString& root, QStringList& paths)
{
    const QString path = registryFile(root);
    if (!QFileInfo::exists(path) && !QFileInfo(path).isSymLink())
        return {};
    QJsonObject json;
    const QString error = readJson(path, json);
    if (!error.isEmpty())
        return QStringLiteral("历史分组登记损坏：%1").arg(error);
    if (!json.value(QStringLiteral("groups")).isArray())
        return QStringLiteral("历史分组登记缺少目录列表：%1").arg(path);
    const auto entries = json.value(QStringLiteral("groups")).toArray();
    for (const auto& entry : entries)
    {
        const QString directory = entry.toString();
        if (directory.isEmpty())
            return QStringLiteral("历史分组登记包含无效目录：%1").arg(path);
        const QString clean = QDir::cleanPath(
            QDir::isAbsolutePath(directory) ? directory : QDir(root).filePath(directory));
        if (!paths.contains(clean))
            paths.append(clean);
    }
    return {};
}
QString writeRegistry(const QString& root, const QStringList& paths)
{
    const QString metadata = metadataPath(root, QString());
    const QFileInfo info(metadata);
    if (info.isSymLink() || info.isJunction())
        return QStringLiteral("历史元数据目录不能是符号链接：%1").arg(metadata);
    if (!QDir(root).mkpath(QStringLiteral(".waibusnap")))
        return QStringLiteral("无法创建历史分组登记目录：%1").arg(metadata);
    QJsonArray entries;
    const QString rootClean = QDir::cleanPath(root);
    for (const auto& path : paths)
    {
        const QString clean = QDir::cleanPath(path);
        entries.append(clean == rootClean || clean.startsWith(rootClean + QLatin1Char('/'))
                           ? QDir(rootClean).relativeFilePath(clean)
                           : clean);
    }
    return writeJson(registryFile(root),
                     {{QStringLiteral("schemaVersion"), 1}, {QStringLiteral("groups"), entries}});
}
bool pathIsInside(const QString& path, const QString& directory)
{
    const QString child = QDir::cleanPath(path);
    const QString parent = QDir::cleanPath(directory);
    return child == parent || child.startsWith(parent + QLatin1Char('/'));
}
bool copyDirectoryContents(const QString& from, const QString& to, QString& error)
{
    QDirIterator iterator(from,
                          QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    while (iterator.hasNext())
    {
        const QString source = iterator.next();
        const QFileInfo info(source);
        const QString relative = QDir(from).relativeFilePath(source);
        const QString target = QDir(to).filePath(relative);
        if (info.isSymLink() || info.isJunction())
        {
            error = QStringLiteral("分组目录包含符号链接，已拒绝迁移：%1").arg(source);
            return false;
        }
        if (info.isDir())
        {
            if (!QDir().mkpath(target))
            {
                error = QStringLiteral("无法创建分组迁移目录：%1").arg(target);
                return false;
            }
        }
        else if (!QFile::copy(source, target))
        {
            error = QStringLiteral("无法复制分组文件：%1").arg(source);
            return false;
        }
    }
    return true;
}
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
            {QStringLiteral("name"), group.name},
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
    const QString storedName = json.value(QStringLiteral("name")).toString();
    if (!storedName.isEmpty())
        group.name = historyGroupNameError(storedName).isEmpty()
                         ? normalizedHistoryGroupName(storedName)
                         : QString();
    if (group.name.isEmpty())
        return QStringLiteral("分组名称无效：%1；该组清理已暂停。").arg(directory.fileName());
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
    QStringList paths;
    const QString registryError = readRegistry(rootDirectory_, paths);
    if (!registryError.isEmpty())
    {
        result.error = registryError;
        return result;
    }
    // 兼容未登记的历史目录：顶层带 marker 的分组仍会被发现，并在初始化时补登记。
    for (const auto& info :
         QDir(rootDirectory_)
             .entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name))
        if (!info.fileName().startsWith(QLatin1Char('.')) &&
            QFileInfo::exists(groupFile(info.absoluteFilePath())))
            paths.append(QDir::cleanPath(info.absoluteFilePath()));
    // 按绝对路径固定顺序，保证复制 UUID 修复与旧版目录扫描结果可重复。
    std::sort(paths.begin(), paths.end(), [](const QString& a, const QString& b) { return a < b; });
    QSet<QString> seen;
    QSet<QString> ids;
    bool hasDefault = false;
    for (const auto& path : paths)
    {
        const QString clean = QDir::cleanPath(path);
        if (seen.contains(clean))
            continue;
        seen.insert(clean);
        const QFileInfo directory(clean);
        HistoryGroupSnapshot snapshot;
        if (!QDir::isAbsolutePath(clean) || clean == QDir::cleanPath(rootDirectory_) ||
            !directory.isDir() || directory.isSymLink() || directory.isJunction())
        {
            // 外置目录可能暂时未挂载；保留登记，等目录恢复后下次扫描重新出现。
            qWarning().noquote() << "历史分组目录暂不可用，已跳过：" << clean;
            continue;
        }
        const QString meta = QDir(clean).filePath(QStringLiteral(".waibusnap"));
        if (QFileInfo(meta).isSymLink() || QFileInfo(meta).isJunction())
        {
            snapshot.group.name = directory.fileName();
            snapshot.group.absolutePath = clean;
            snapshot.error = QStringLiteral("分组元数据目录不能是符号链接：%1").arg(meta);
        }
        else
            snapshot.error = readGroup(directory, snapshot.group);
        if (snapshot.error.isEmpty())
        {
            if (ids.contains(snapshot.group.id))
            {
                snapshot.group.id = newId();
                // 复制默认组也只能保留一个默认身份。
                if (snapshot.group.isDefault && hasDefault)
                    snapshot.group.isDefault = false;
                snapshot.error = writeJson(groupFile(clean), groupJson(snapshot.group));
            }
            else if (snapshot.group.isDefault && hasDefault)
                snapshot.error = QStringLiteral("发现多个默认分组，已暂停该组操作，请检查元数据。");
            ids.insert(snapshot.group.id);
            hasDefault = hasDefault || snapshot.group.isDefault;
        }
        QJsonObject index;
        if (snapshot.error.isEmpty())
            snapshot.error = readIndex(clean, index);
        for (const auto& file :
             QDir(clean).entryInfoList(QDir::Files | QDir::NoSymLinks, QDir::Name))
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
            item.originalPath = originalFile(clean, item.fileName);
            const bool claimsOriginal = metadata.value(QStringLiteral("hasOriginal")).toBool();
            const QFileInfo originalInfo(item.originalPath);
            item.hasOriginal = claimsOriginal && originalInfo.isFile() &&
                               !originalInfo.isSymLink() && !originalInfo.isJunction();
            item.originalBytes = item.hasOriginal ? originalInfo.size() : 0;
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
    const QString metadata = QDir(rootDirectory_).filePath(QStringLiteral(".waibusnap"));
    if (!QDir(rootDirectory_).mkpath(QStringLiteral(".waibusnap")))
        return {{}, QStringLiteral("无法创建历史元数据目录：%1").arg(metadata)};
    if (QFileInfo(metadata).isSymLink() || QFileInfo(metadata).isJunction())
        return {{}, QStringLiteral("历史元数据目录不能是符号链接：%1").arg(metadata)};
    QLockFile lock(QDir(rootDirectory_).filePath(QStringLiteral(".waibusnap.lock")));
    error = lockError(lock);
    if (!error.isEmpty())
        return {{}, error};
    auto result = scanUnlocked();
    if (!result.error.isEmpty())
        return result;
    bool hasDefault = false;
    QStringList names = rootNames(rootDirectory_);
    for (const auto& snapshot : result.groups)
    {
        hasDefault |= snapshot.group.isDefault;
        names.append(snapshot.group.name);
    }
    if (!hasDefault)
    {
        QString name = QStringLiteral("默认分组");
        for (int suffix = 2; !historyGroupNameError(name, names).isEmpty(); ++suffix)
            name = QStringLiteral("默认分组-%1").arg(suffix);
        const auto created =
            createGroupUnlocked(name, true, QDir(rootDirectory_).filePath(name), names);
        if (!created.success)
        {
            result.error = created.error;
            return result;
        }
    }
    result = scanUnlocked();
    if (!result.error.isEmpty())
        return result;
    QStringList paths;
    const QString registryError = readRegistry(rootDirectory_, paths);
    if (!registryError.isEmpty())
        return {{}, registryError};
    for (const auto& snapshot : result.groups)
    {
        const QString path = QDir::cleanPath(snapshot.group.absolutePath);
        if (!paths.contains(path))
            paths.append(path);
    }
    error = writeRegistry(rootDirectory_, paths);
    if (!error.isEmpty())
    {
        result.error = error;
        return result;
    }
    return result;
}
HistoryGroupResult HistoryStore::createGroupUnlocked(const QString& name, bool isDefault,
                                                     const QString& directory,
                                                     const QStringList& existingNames)
{
    QString error = historyGroupNameError(name, existingNames);
    if (error.isEmpty())
        error = cleanupPolicyError(defaultCleanup_);
    if (!error.isEmpty())
        return {{false, error}, {}};
    const QString cleanName = normalizedHistoryGroupName(name);
    const QString target =
        directory.isEmpty() ? QDir(rootDirectory_).filePath(cleanName) : QDir::cleanPath(directory);
    const QString root = QDir::cleanPath(rootDirectory_);
    if (!QDir::isAbsolutePath(target) || target == root ||
        pathIsInside(target, QDir(root).filePath(QStringLiteral(".waibusnap"))) ||
        pathIsInside(root, target))
        return {{false, QStringLiteral("分组目录必须是独立的绝对目录，不能包含或占用历史根目录。")},
                {}};
    const QFileInfo targetInfo(target);
    if (targetInfo.exists())
    {
        if (!targetInfo.isDir() || targetInfo.isSymLink() || targetInfo.isJunction())
            return {{false, QStringLiteral("分组目录已存在，但不是普通目录：%1").arg(target)}, {}};
        if (!QDir(target)
                 .entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)
                 .isEmpty())
            return {{false, QStringLiteral("分组目录必须为空：%1").arg(target)}, {}};
    }
    else
    {
        if (!QDir().mkpath(QFileInfo(target).absolutePath()))
            return {{false, QStringLiteral("无法创建分组目录的上级目录：%1").arg(target)}, {}};
        if (!QDir().mkdir(target))
            return {{false, QStringLiteral("无法创建分组目录：%1").arg(target)}, {}};
    }
    if (!QDir(target).mkdir(QStringLiteral(".waibusnap")))
        return {{false, QStringLiteral("无法创建分组元数据目录，空目录已保留：%1").arg(target)},
                {}};
    HistoryGroup group{
        newId(),        cleanName, target, isDefault, QDateTime::currentMSecsSinceEpoch(),
        defaultCleanup_};
    error = saveIndex(target, {});
    if (error.isEmpty())
        error = writeJson(groupFile(target), groupJson(group));
    return {{error.isEmpty(), error}, group};
}
HistoryGroupResult HistoryStore::createGroup(const QString& name, const QString& directory)
{
    QString error = rootError(rootDirectory_);
    if (!error.isEmpty())
        return {{false, error}, {}};
    QLockFile lock(QDir(rootDirectory_).filePath(QStringLiteral(".waibusnap.lock")));
    error = lockError(lock);
    if (!error.isEmpty())
        return {{false, error}, {}};
    const auto scanned = scanUnlocked();
    if (!scanned.error.isEmpty())
        return {{false, scanned.error}, {}};
    QStringList names = rootNames(rootDirectory_);
    for (const auto& snapshot : scanned.groups)
        names.append(snapshot.group.name);
    const QString target = directory.isEmpty()
                               ? QDir(rootDirectory_).filePath(normalizedHistoryGroupName(name))
                               : QDir::cleanPath(directory);
    for (const auto& snapshot : scanned.groups)
        if (pathIsInside(target, snapshot.group.absolutePath) ||
            pathIsInside(snapshot.group.absolutePath, target))
            return {{false, QStringLiteral("分组目录不能与已有分组目录互相嵌套：%1").arg(target)},
                    {}};
    const auto created = createGroupUnlocked(name, false, directory, names);
    if (!created.success)
        return created;
    QStringList paths;
    const QString registryError = readRegistry(rootDirectory_, paths);
    if (!registryError.isEmpty())
        return {{false, registryError}, created.group};
    for (const auto& snapshot : scanned.groups)
    {
        const QString path = QDir::cleanPath(snapshot.group.absolutePath);
        if (!paths.contains(path))
            paths.append(path);
    }
    const QString createdPath = QDir::cleanPath(created.group.absolutePath);
    if (!paths.contains(createdPath))
        paths.append(createdPath);
    error = writeRegistry(rootDirectory_, paths);
    if (!error.isEmpty())
        return {{false, error}, created.group};
    return created;
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
    QStringList names = rootNames(rootDirectory_);
    for (const auto& current : scanned.groups)
        if (current.group.id != groupId)
            names.append(current.group.name);
    error = historyGroupNameError(name, names);
    if (!error.isEmpty())
        return {{false, error}, {}};
    const QString clean = normalizedHistoryGroupName(name);
    if (clean == group.name)
        return {{true, {}}, group};
    group.name = clean;
    error = writeJson(groupFile(group.absolutePath), groupJson(group));
    return {{error.isEmpty(), error}, group};
}
HistoryGroupResult HistoryStore::changeGroupDirectory(const QString& groupId,
                                                      const QString& directory)
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
    const QString oldPath = QDir::cleanPath(snapshot->group.absolutePath);
    const QString target = QDir::cleanPath(directory);
    const QString root = QDir::cleanPath(rootDirectory_);
    if (!QDir::isAbsolutePath(target) || target == oldPath)
        return {{target == oldPath,
                 target == oldPath ? QString() : QStringLiteral("分组目录必须为绝对路径。")},
                snapshot->group};
    if (target == root || pathIsInside(target, QDir(root).filePath(QStringLiteral(".waibusnap"))) ||
        pathIsInside(root, target))
        return {{false, QStringLiteral("不能把分组移动到包含历史根目录或元数据目录的位置。")},
                snapshot->group};
    if (pathIsInside(target, oldPath) || pathIsInside(oldPath, target))
        return {{false, QStringLiteral("新旧分组目录不能互相嵌套。")}, snapshot->group};
    for (const auto& current : scanned.groups)
        if (current.group.id != groupId && (pathIsInside(target, current.group.absolutePath) ||
                                            pathIsInside(current.group.absolutePath, target)))
            return {{false, QStringLiteral("目标目录与已有分组目录互相嵌套。")}, snapshot->group};
    const QFileInfo targetInfo(target);
    if (targetInfo.exists())
    {
        if (!targetInfo.isDir() || targetInfo.isSymLink() || targetInfo.isJunction())
            return {{false, QStringLiteral("目标目录不是普通空目录：%1").arg(target)},
                    snapshot->group};
        if (!QDir(target)
                 .entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)
                 .isEmpty())
            return {{false, QStringLiteral("目标目录必须为空：%1").arg(target)}, snapshot->group};
        if (!QDir().rmdir(target))
            return {{false, QStringLiteral("无法准备目标空目录：%1").arg(target)}, snapshot->group};
    }
    else if (!QDir().mkpath(QFileInfo(target).absolutePath()))
        return {{false, QStringLiteral("无法创建目标目录的上级目录：%1").arg(target)},
                snapshot->group};
    bool moved = QDir().rename(oldPath, target);
    if (!moved)
    {
        if (!QDir().mkpath(target))
            return {{false, QStringLiteral("无法创建新的分组目录：%1").arg(target)},
                    snapshot->group};
        QString copyError;
        if (!copyDirectoryContents(oldPath, target, copyError))
        {
            QDir(target).removeRecursively();
            return {{false, copyError}, snapshot->group};
        }
        if (!QDir(oldPath).removeRecursively())
            return {{false, QStringLiteral("分组已复制到新目录，但旧目录删除失败，请手动确认：%1")
                                .arg(oldPath)},
                    snapshot->group};
        moved = true;
    }
    HistoryGroup movedGroup = snapshot->group;
    movedGroup.absolutePath = target;
    QStringList paths;
    const QString registryError = readRegistry(rootDirectory_, paths);
    if (!registryError.isEmpty())
        return {{false, registryError}, snapshot->group};
    bool replaced = false;
    for (auto& path : paths)
        if (QDir::cleanPath(path) == oldPath)
        {
            path = target;
            replaced = true;
        }
    if (!replaced)
        paths.append(target);
    for (const auto& current : scanned.groups)
        if (current.group.id != groupId &&
            !paths.contains(QDir::cleanPath(current.group.absolutePath)))
            paths.append(current.group.absolutePath);
    error = writeRegistry(rootDirectory_, paths);
    if (!error.isEmpty())
        return {{false, error}, movedGroup};
    return {{true, {}}, movedGroup};
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
    QStringList paths;
    const QString registryError = readRegistry(rootDirectory_, paths);
    if (!registryError.isEmpty())
        return {false, registryError};
    paths.removeAll(QDir::cleanPath(snapshot->group.absolutePath));
    for (const auto& current : scanned.groups)
        if (current.group.id != groupId &&
            !paths.contains(QDir::cleanPath(current.group.absolutePath)))
            paths.append(current.group.absolutePath);
    error = writeRegistry(rootDirectory_, paths);
    return {error.isEmpty(), error};
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
                                                 const QString& existingFileName,
                                                 const QImage& original)
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
        // 同毫秒 / 时钟回拨时保持组内写入顺序，避免稳定 ID 决胜把刚入库图片当成旧图清理。
        for (const auto& previous : snapshot->items)
            if (previous.managed && previous.createdAtMs >= item.createdAtMs)
            {
                if (previous.createdAtMs >= 9007199254740991LL)
                    return {
                        {false, QStringLiteral("历史时间标记已达索引上限，无法安全写入新记录。")},
                        {}};
                item.createdAtMs = previous.createdAtMs + 1;
            }
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
    const QString originalPath = item.originalPath.isEmpty()
                                     ? originalFile(snapshot->group.absolutePath, item.fileName)
                                     : item.originalPath;
    QString originalError;
    bool hasOriginal = item.hasOriginal;
    qint64 originalBytes = item.originalBytes;
    if (!original.isNull())
    {
        const QString written = writePng(originalPath, original);
        if (written.isEmpty())
        {
            hasOriginal = true;
            originalBytes = QFileInfo(originalPath).size();
        }
        else
            originalError = written;
    }
    else if (QFileInfo::exists(originalPath) || QFileInfo(originalPath).isSymLink())
    {
        if (QFileInfo(originalPath).isSymLink() || !QFile::remove(originalPath))
            originalError = QStringLiteral("旧水印原图删除失败：%1").arg(originalPath);
        else
        {
            hasOriginal = false;
            originalBytes = 0;
        }
    }
    else
    {
        hasOriginal = false;
        originalBytes = 0;
    }
    item.groupId = groupId;
    item.managed = true;
    item.source = source;
    item.absolutePath = QDir(snapshot->group.absolutePath).filePath(item.fileName);
    item.originalPath = originalPath;
    item.hasOriginal = hasOriginal;
    item.originalBytes = originalBytes;
    item.bytes = QFileInfo(item.absolutePath).size();
    item.pixelSize = pixels.size();
    const QJsonObject previous = index.value(item.fileName).toObject();
    QJsonObject metadata = previous;
    metadata.insert(QStringLiteral("starred"), previous.value(QStringLiteral("starred")).toBool());
    metadata.insert(QStringLiteral("managed"), true);
    metadata.insert(QStringLiteral("source"), sourceText(source));
    metadata.insert(QStringLiteral("createdAt"), previous.contains(QStringLiteral("createdAt"))
                                                     ? previous.value(QStringLiteral("createdAt"))
                                                     : item.createdAtMs);
    metadata.insert(QStringLiteral("hasOriginal"), hasOriginal);
    metadata.insert(QStringLiteral("originalBytes"), originalBytes);
    index.insert(item.fileName, metadata);
    error = saveIndex(snapshot->group.absolutePath, index);
    if (!error.isEmpty())
        return {{false, error + QStringLiteral("；图片文件已保留，未声明入库成功。")}, item};
    return {{true, originalError}, item};
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
    {
        QJsonObject metadata = index.value(fileName).toObject();
        metadata.insert(QStringLiteral("starred"), starred);
        metadata.insert(QStringLiteral("managed"), found->managed);
        metadata.insert(QStringLiteral("source"), sourceText(found->source));
        metadata.insert(QStringLiteral("createdAt"), found->createdAtMs);
        index.insert(fileName, metadata);
    }
    error = saveIndex(snapshot->group.absolutePath, index);
    return {error.isEmpty(), error};
}
HistoryStatus HistoryStore::validateItem(const HistoryItem& item) const
{
    const QFileInfo file(item.absolutePath);
    const QFileInfo group(file.absolutePath());
    const QFileInfo metadata(QDir(group.absoluteFilePath()).filePath(QStringLiteral(".waibusnap")));
    const QString expected = QDir(group.absoluteFilePath()).filePath(item.fileName);
    if (!rootError(rootDirectory_).isEmpty() || !QDir::isAbsolutePath(group.absoluteFilePath()) ||
        !imageFileName(item.fileName) || item.absolutePath != expected || !file.isFile() ||
        file.isSymLink() || !group.isDir() || group.isSymLink() || group.isJunction() ||
        metadata.isSymLink() || metadata.isJunction() ||
        !QFileInfo(groupFile(group.absoluteFilePath())).isFile() ||
        file.canonicalFilePath() != QDir(group.canonicalFilePath()).filePath(item.fileName))
        return {false, QStringLiteral("图片已丢失、路径越界或包含符号链接，请刷新历史。")};
    return {true, {}};
}
HistoryStatus HistoryStore::deleteManagedItem(const HistoryItem& item, const HistoryTrash& trash)
{
    return removeManagedItem(item, trash, false);
}
HistoryStatus HistoryStore::permanentlyDeleteManagedItem(const HistoryItem& item, qint64 nowMs,
                                                         const QString& selectedId)
{
    const HistoryTrash remove =
        permanentRemove_
            ? permanentRemove_
            : HistoryTrash(
                  [](const QString& path)
                  {
                      QFile file(path);
                      const bool removed = file.remove();
                      return HistoryStatus{removed, removed ? QString() : file.errorString()};
                  });
    return removeManagedItem(item, remove, true, nowMs, selectedId);
}
HistoryStatus HistoryStore::removeManagedItem(const HistoryItem& item, const HistoryTrash& remove,
                                              bool automatic, qint64 nowMs,
                                              const QString& selectedId)
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
                   current.absolutePath == item.absolutePath && current.source == item.source &&
                   (!automatic || (current.createdAtMs == item.createdAtMs &&
                                   current.bytes == item.bytes && current.starred == item.starred));
    if (!matched)
        return {false, QStringLiteral("目标不是当前扫描的受管历史项，已拒绝删除。")};
    if (automatic)
    {
        const auto plan =
            selectCleanupCandidates(group->group.cleanup, group->items, nowMs, selectedId);
        if (!plan.error.isEmpty())
            return {false, plan.error};
        bool candidate = false;
        for (const auto& current : plan.candidates)
            candidate |= historyItemId(current) == historyItemId(item);
        if (!candidate)
            return {false,
                    QStringLiteral("清理策略或保护状态已改变，已停止清理，原文件和索引已保留。")};
    }
    if (!remove)
        return {false, QStringLiteral("未提供删除服务，原文件和索引已保留。")};
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
    const auto moved = remove(item.absolutePath);
    if (!moved.success)
        error = QStringLiteral("图片删除失败，原文件和索引已保留：%1").arg(moved.error);
    else if (QFileInfo::exists(item.absolutePath) || QFileInfo(item.absolutePath).isSymLink())
        error = QStringLiteral("删除服务返回成功但原文件仍存在，索引已保留。");
    else
    {
        if (item.hasOriginal && !item.originalPath.isEmpty() &&
            (QFileInfo::exists(item.originalPath) || QFileInfo(item.originalPath).isSymLink()))
        {
            if (QFileInfo(item.originalPath).isSymLink())
                error =
                    QStringLiteral("水印原图路径是符号链接，已停止删除：%1").arg(item.originalPath);
            else
            {
                const auto removedOriginal = remove(item.originalPath);
                if (!removedOriginal.success)
                    error = QStringLiteral("水印原图删除失败，主图已开始删除：%1")
                                .arg(removedOriginal.error);
                else if (QFileInfo::exists(item.originalPath) ||
                         QFileInfo(item.originalPath).isSymLink())
                    error = QStringLiteral("删除服务返回成功但水印原图仍存在：%1")
                                .arg(item.originalPath);
            }
        }
        if (!error.isEmpty())
        {
            // 主图已移走；索引尚未提交，下面恢复主图并把错误返回给用户。
        }
        else if (!staged.commit())
            error =
                QStringLiteral("历史索引提交失败，已尝试恢复原图：%1").arg(staged.errorString());
        else
            return {true, {}};
    }
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
