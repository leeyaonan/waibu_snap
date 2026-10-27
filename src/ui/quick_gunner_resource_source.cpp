#include "ui/quick_gunner_resource_source.h"
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
namespace waibusnap
{
namespace
{
bool imageFile(const QFileInfo& file)
{
    const auto suffix = file.suffix().toLower();
    return file.isFile() && !file.isSymLink() && !file.isJunction() &&
           (suffix == QStringLiteral("png") || suffix == QStringLiteral("jpg") ||
            suffix == QStringLiteral("jpeg") || suffix == QStringLiteral("webp"));
}
}
QuickGunnerResourceSource::QuickGunnerResourceSource(DirectoryProvider directory, QString starsPath,
                                                     HistoryWindowActions actions, QObject* parent,
                                                     bool watchDirectories)
    : ResourceSource(parent), provider_(std::move(directory)), starsPath_(std::move(starsPath)),
      actions_(std::move(actions)), watchDirectories_(watchDirectories)
{
    QFile file(starsPath_);
    if (file.open(QIODevice::ReadOnly))
    {
        const auto document = QJsonDocument::fromJson(file.readAll());
        if (document.isArray())
            for (const auto& value : document.array())
                if (value.isString() && QDir::isAbsolutePath(value.toString()))
                    stars_.insert(QDir::cleanPath(value.toString()));
    }
    debounce_.setInterval(200);
    debounce_.setSingleShot(true);
    poll_.setInterval(2000);
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, this, [this] { debounce_.start(); });
    connect(&debounce_, &QTimer::timeout, this, &QuickGunnerResourceSource::rescanAndNotify);
    connect(&poll_, &QTimer::timeout, this, &QuickGunnerResourceSource::rescanAndNotify);
    refresh();
}
QString QuickGunnerResourceSource::id() const { return QStringLiteral("quick-gunner"); }
QString QuickGunnerResourceSource::displayName() const { return QStringLiteral("快枪手"); }
bool QuickGunnerResourceSource::visible() const { return !configuredDirectory_.isEmpty(); }
ResourceCapabilities QuickGunnerResourceSource::capabilities() const
{
    return {false,
            false,
            false,
            true,
            bool(actions_.trash),
            bool(actions_.reveal),
            bool(actions_.trash),
            true};
}
void QuickGunnerResourceSource::refresh()
{
    configuredDirectory_ = provider_ ? provider_() : QString();
    const QFileInfo root(configuredDirectory_);
    const QString canonicalRoot = root.canonicalFilePath();
    directory_.clear();
    snapshot_ = {};
    if (configuredDirectory_.isEmpty())
        snapshot_.notice = QStringLiteral("尚未选择快枪手目录，请选择目录。");
    else if (!QDir::isAbsolutePath(configuredDirectory_) ||
             configuredDirectory_.contains(QChar::Null) || canonicalRoot.isEmpty() ||
             !root.isDir() || root.isSymLink() || root.isJunction() || !root.isReadable())
        snapshot_.error =
            QStringLiteral("快枪手目录不存在或不可读取：%1").arg(configuredDirectory_);
    else
    {
        directory_ = canonicalRoot;
        snapshot_.groups.append({{}, displayName(), false, false, directory_});
        for (const auto& file :
             QDir(directory_).entryInfoList(QDir::Files | QDir::NoSymLinks, QDir::Name))
        {
            if (!imageFile(file))
                continue;
            ResourceItem item;
            item.id = QDir::cleanPath(file.absoluteFilePath());
            item.sourceId = id();
            item.fileName = file.fileName();
            item.absolutePath = item.id;
            item.bytes = file.size();
            item.createdAtMs = file.lastModified().toMSecsSinceEpoch();
            item.starred = stars_.contains(item.id);
            item.deletable = true;
            snapshot_.items.append(item);
            ++snapshot_.usage.externalCount;
            snapshot_.usage.externalBytes += item.bytes;
            snapshot_.usage.starredCount += int(item.starred);
        }
        snapshot_.groupUsage.insert({}, snapshot_.usage);
    }
    // 已移动 / 删除的路径不保留悬空星标。
    auto pruned = stars_;
    for (const auto& path : stars_)
        if (!QFileInfo::exists(path))
            pruned.remove(path);
    if (pruned != stars_)
    {
        const auto result = writeStars(pruned);
        if (result.success)
            stars_ = std::move(pruned);
        else
            snapshot_.notice = result.error;
    }
    signature_.clear();
    QDataStream signature(&signature_, QIODevice::WriteOnly);
    signature << configuredDirectory_ << snapshot_.error << snapshot_.notice;
    for (const auto& item : snapshot_.items)
        signature << item.id << item.bytes << item.createdAtMs << item.starred;
    updateMonitoring();
}
void QuickGunnerResourceSource::rescanAndNotify()
{
    const auto previous = signature_;
    refresh();
    if (signature_ != previous)
        emit changed();
}
void QuickGunnerResourceSource::directoryChanged() { rescanAndNotify(); }
void QuickGunnerResourceSource::setActive(bool active)
{
    if (active_ == active)
        return;
    active_ = active;
    if (active_)
        rescanAndNotify();
    else
        updateMonitoring();
}
void QuickGunnerResourceSource::updateMonitoring()
{
    const auto watched = watcher_.directories();
    if (!watched.isEmpty() && (!active_ || watched != QStringList{directory_}))
        watcher_.removePaths(watched);
    if (!active_)
    {
        debounce_.stop();
        poll_.stop();
        return;
    }
    if (watchDirectories_ && !directory_.isEmpty() && watcher_.directories().isEmpty())
        watcher_.addPath(directory_);
    if (watcher_.directories().isEmpty() && !configuredDirectory_.isEmpty())
    {
        if (!poll_.isActive())
            poll_.start();
    }
    else
        poll_.stop();
}
bool QuickGunnerResourceSource::validItem(const QString& path) const
{
    // 每次动作重新核验 provider、父目录与文件类型，拒绝旧目录、子目录和链接替换。
    if (directory_.isEmpty() || !provider_ || provider_() != configuredDirectory_ ||
        QFileInfo(configuredDirectory_).isSymLink() ||
        QFileInfo(configuredDirectory_).isJunction() ||
        QFileInfo(configuredDirectory_).canonicalFilePath() != directory_ ||
        !QDir::isAbsolutePath(path) || QDir::cleanPath(path) != path)
        return false;
    const QFileInfo file(path);
    return file.absolutePath() == directory_ && imageFile(file);
}
ResourceImageResult QuickGunnerResourceSource::loadImage(const QString& id, QSize bounds,
                                                         bool original) const
{
    if (original || !validItem(id))
        return {{false, QStringLiteral("快枪手图片已丢失或路径不在当前目录内。")}, {}};
    QImageReader reader(id);
    reader.setAutoTransform(true);
    if (QFileInfo(id).suffix().compare(QStringLiteral("webp"), Qt::CaseInsensitive) == 0 &&
        !QImageReader::supportedImageFormats().contains(QByteArrayLiteral("webp")))
        return {{false, QStringLiteral("缺少 WebP 图片插件，无法读取此图片。")}, {}};
    if (bounds.isValid())
    {
        const auto size = reader.size();
        if (size.isValid())
            reader.setScaledSize(size.scaled(bounds, Qt::KeepAspectRatio));
    }
    QImage image = reader.read();
    if (image.isNull())
        return {{false, QStringLiteral("快枪手图片读取失败：%1").arg(reader.errorString())}, {}};
    image.setDevicePixelRatio(1);
    return {{true, {}}, image};
}
ResourceStatus QuickGunnerResourceSource::writeStars(const QSet<QString>& stars) const
{
    if (!QDir().mkpath(QFileInfo(starsPath_).absolutePath()))
        return {false, QStringLiteral("无法创建快枪手星标目录。")};
    QStringList sorted(stars.begin(), stars.end());
    sorted.sort();
    QJsonArray values;
    for (const auto& path : sorted)
        values.append(path);
    QSaveFile file(starsPath_);
    const auto data = QJsonDocument(values).toJson(QJsonDocument::Compact);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit())
        return {false, QStringLiteral("快枪手星标保存失败，原星标保留。")};
    return {true, {}};
}
ResourceStatus QuickGunnerResourceSource::setStarred(const QString& id, bool starred)
{
    if (!validItem(id))
        return {false, QStringLiteral("图片不在快枪手当前目录内。")};
    auto candidate = stars_;
    if (starred)
        candidate.insert(id);
    else
        candidate.remove(id);
    const auto result = writeStars(candidate);
    if (result.success)
    {
        stars_ = std::move(candidate);
        rescanAndNotify();
    }
    return result;
}
ResourceStatus QuickGunnerResourceSource::deleteItem(const QString& id)
{
    if (!validItem(id) || !actions_.trash)
        return {false, QStringLiteral("无法删除：图片不在快枪手当前目录内或回收站服务不可用。")};
    const auto result = actions_.trash(id);
    rescanAndNotify();
    return {result.success, result.error};
}
ResourceClearResult QuickGunnerResourceSource::clearAll()
{
    refresh();
    if (!snapshot_.error.isEmpty() || directory_.isEmpty())
        return {{false, snapshot_.error.isEmpty() ? QStringLiteral("请先选择快枪手目录。")
                                                  : snapshot_.error},
                0};
    const auto items = snapshot_.items;
    qsizetype count = 0;
    ResourceStatus failure{true, {}};
    for (const auto& item : items)
    {
        if (!validItem(item.id) || !actions_.trash)
            failure = {false, QStringLiteral("图片已变化或回收站服务不可用。")};
        else
        {
            const auto result = actions_.trash(item.id);
            failure = {result.success, result.error};
        }
        if (!failure.success)
            break;
        ++count;
    }
    rescanAndNotify();
    if (!failure.success)
        return {{false, QStringLiteral("已移入回收站 %1 张，遇到错误后停止：%2")
                            .arg(count)
                            .arg(failure.error)},
                count};
    return {{true, {}}, count};
}
ResourceStatus QuickGunnerResourceSource::revealItem(const QString& id) const
{
    if (!validItem(id) || !actions_.reveal)
        return {false, QStringLiteral("无法定位此快枪手图片。")};
    const auto result = actions_.reveal(id);
    return {result.success, result.error};
}
bool QuickGunnerResourceSource::canCopy() const { return bool(actions_.copyImage); }
bool QuickGunnerResourceSource::canPin() const { return bool(actions_.pinImage); }
ResourceStatus QuickGunnerResourceSource::outputImage(const QString& id, bool original,
                                                      bool pin) const
{
    const auto loaded = loadImage(id, {}, original);
    if (!loaded.success)
        return {false, loaded.error};
    const auto& callback = pin ? actions_.pinImage : actions_.copyImage;
    const auto result = callback ? callback(loaded.image)
                                 : ImageOutputResult{false, QStringLiteral("图片输出服务不可用。")};
    return {result.success, result.explanation};
}
}
