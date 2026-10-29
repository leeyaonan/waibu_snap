#include "ui/recording_resource_source.h"
#include <QDataStream>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUrl>
namespace waibusnap
{
namespace
{
bool saveJson(const QString& path, const QJsonDocument& document)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    QSaveFile file(path);
    const auto data = document.toJson(QJsonDocument::Compact);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size() && file.commit();
}
}
RecordingResourceSource::RecordingResourceSource(RecordingOutput kind, DirectoryProvider directory,
                                                 QString starsPath, QString metaPath,
                                                 std::shared_ptr<RecordingMediaProbe> probe,
                                                 HistoryWindowActions actions, QObject* parent,
                                                 bool watchDirectories)
    : ResourceSource(parent), kind_(kind), provider_(std::move(directory)),
      starsPath_(std::move(starsPath)), metaPath_(std::move(metaPath)), probe_(std::move(probe)),
      actions_(std::move(actions)), watchDirectories_(watchDirectories)
{
    pool_.setMaxThreadCount(2);
    QFile stars(starsPath_);
    if (stars.open(QIODevice::ReadOnly))
    {
        const auto document = QJsonDocument::fromJson(stars.readAll());
        if (!document.isArray())
            cacheNotice_ = QStringLiteral("录制星标文件损坏，已回退为空。");
        else
            for (const auto& value : document.array())
            {
                const QFileInfo file(value.toString());
                if (value.isString() && QDir::isAbsolutePath(value.toString()))
                    stars_.insert(file.canonicalFilePath().isEmpty()
                                      ? QDir::cleanPath(file.absoluteFilePath())
                                      : file.canonicalFilePath());
            }
    }
    // Windows 的原子替换要求先释放星标读取句柄，剪枝才可提交。
    stars.close();
    readMetadata();
    debounce_.setInterval(200);
    debounce_.setSingleShot(true);
    poll_.setInterval(2000);
    delivery_.setInterval(50);
    delivery_.setSingleShot(true);
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, this, [this] { debounce_.start(); });
    connect(&debounce_, &QTimer::timeout, this, &RecordingResourceSource::rescanAndNotify);
    connect(&poll_, &QTimer::timeout, this, &RecordingResourceSource::rescanAndNotify);
    connect(&delivery_, &QTimer::timeout, this,
            [this]
            {
                if (metadataDirty_)
                    writeMetadata();
                snapshot_.notice = cacheNotice_;
                emit changed();
            });
    refresh();
}
RecordingResourceSource::~RecordingResourceSource()
{
    // 工作线程只交付到本对象；析构期间不处理事件，等待在途任务后由 QObject 移除排队回调。
    pool_.waitForDone();
    if (metadataDirty_)
        writeMetadata();
}
QString RecordingResourceSource::id() const
{
    return kind_ == RecordingOutput::Gif ? QStringLiteral("recording-gif")
                                         : QStringLiteral("recording-video");
}
QString RecordingResourceSource::displayName() const
{
    return kind_ == RecordingOutput::Gif ? QStringLiteral("录制 GIF") : QStringLiteral("录制视频");
}
ResourceCapabilities RecordingResourceSource::capabilities() const
{
    ResourceCapabilities result;
    result.star = true;
    result.deleteItem = bool(actions_.trash);
    result.reveal = bool(actions_.reveal);
    result.openExternal = kind_ == RecordingOutput::Video;
    return result;
}
QString RecordingResourceSource::configuredDirectory() const
{
    const auto configured = provider_ ? provider_() : QString();
    return configured.isEmpty() ? (kind_ == RecordingOutput::Gif ? defaultRecordingGifDirectory()
                                                                 : defaultRecordingVideoDirectory())
                                : configured;
}
bool RecordingResourceSource::mediaFile(const QFileInfo& file) const
{
    return file.isFile() && !file.fileName().startsWith(QLatin1Char('.')) && !file.isHidden() &&
           !file.isSymLink() && !file.isJunction() && !file.isShortcut() &&
           file.suffix().compare(kind_ == RecordingOutput::Gif ? QStringLiteral("gif")
                                                               : QStringLiteral("mp4"),
                                 Qt::CaseInsensitive) == 0;
}
bool RecordingResourceSource::validItem(const QString& path) const
{
    const QFileInfo root(configuredDirectory_);
    if (directory_.isEmpty() || configuredDirectory() != configuredDirectory_ || root.isSymLink() ||
        root.isJunction() || root.canonicalFilePath() != directory_ ||
        !QDir::isAbsolutePath(path) || QDir::cleanPath(path) != path)
        return false;
    const QFileInfo file(path);
    return file.absolutePath() == directory_ && mediaFile(file);
}
QString RecordingResourceSource::fileKey(const QString& path)
{
    const QFileInfo file(path);
    return path + QChar::Null + QString::number(file.size()) + QChar::Null +
           QString::number(file.lastModified().toMSecsSinceEpoch());
}
void RecordingResourceSource::applyMetadata(ResourceItem& item) const
{
    const auto cached = metadata_.constFind(item.id);
    if (cached != metadata_.cend() && cached->size == item.bytes &&
        cached->mtimeMs == item.createdAtMs)
    {
        item.pixelSize = cached->info.pixelSize;
        item.durationMs = cached->info.durationMs;
    }
    item.mediaError = failures_.value(fileKey(item.id));
}
void RecordingResourceSource::refresh()
{
    configuredDirectory_ = configuredDirectory();
    const QFileInfo root(configuredDirectory_);
    directory_.clear();
    snapshot_ = {};
    snapshot_.notice = cacheNotice_;
    if (!QDir::isAbsolutePath(configuredDirectory_) || configuredDirectory_.contains(QChar::Null) ||
        !root.isDir() || root.isSymLink() || root.isJunction() || !root.isReadable() ||
        !root.isWritable())
        snapshot_.error = QStringLiteral("录制目录不存在或不可写：%1。请在设置 → 录制中重新选择。")
                              .arg(configuredDirectory_);
    else
    {
        directory_ = root.canonicalFilePath();
        snapshot_.groups.append({{}, displayName(), false, false, directory_});
        for (const auto& file :
             QDir(directory_).entryInfoList(QDir::Files | QDir::NoSymLinks, QDir::Name))
        {
            if (!mediaFile(file))
                continue;
            ResourceItem item;
            item.id = QDir::cleanPath(file.canonicalFilePath());
            item.absolutePath = item.id;
            item.sourceId = id();
            item.fileName = file.fileName();
            item.bytes = file.size();
            item.createdAtMs = file.lastModified().toMSecsSinceEpoch();
            item.starred = stars_.contains(item.id);
            item.deletable = true;
            item.preview =
                kind_ == RecordingOutput::Gif ? ResourcePreview::Gif : ResourcePreview::Video;
            item.badges = {kind_ == RecordingOutput::Gif ? QStringLiteral("GIF")
                                                         : QStringLiteral("视频")};
            applyMetadata(item);
            if (!item.pixelSize.isValid() && item.mediaError.isEmpty())
                enqueue({item.id, fileKey(item.id), item.bytes, item.createdAtMs, {}, false});
            snapshot_.items.append(item);
            ++snapshot_.usage.externalCount;
            snapshot_.usage.externalBytes += item.bytes;
            snapshot_.usage.starredCount += int(item.starred);
        }
        snapshot_.groupUsage.insert({}, snapshot_.usage);
    }
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
    for (auto it = metadata_.begin(); it != metadata_.end();)
        if (!QFileInfo::exists(it.key()))
        {
            it = metadata_.erase(it);
            metadataDirty_ = true;
        }
        else
            ++it;
    if (metadataDirty_ && !delivery_.isActive())
        delivery_.start();
    updateMonitoring();
}
QByteArray RecordingResourceSource::signature() const
{
    QByteArray result;
    QDataStream stream(&result, QIODevice::WriteOnly);
    stream << configuredDirectory_ << snapshot_.error << snapshot_.notice;
    for (const auto& item : snapshot_.items)
        stream << item.id << item.bytes << item.createdAtMs << item.starred << item.pixelSize
               << item.durationMs << item.mediaError;
    return result;
}
void RecordingResourceSource::rescanAndNotify()
{
    const auto previous = signature();
    refresh();
    if (signature() != previous)
        emit changed();
}
void RecordingResourceSource::directoryChanged() { rescanAndNotify(); }
void RecordingResourceSource::setActive(bool active)
{
    if (active_ == active)
        return;
    active_ = active;
    if (active)
        rescanAndNotify();
    else
        updateMonitoring();
}
void RecordingResourceSource::updateMonitoring()
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
    // 轮询同时兜底文件原地替换和 watcher 丢事件；仅激活来源运行。
    if (!poll_.isActive())
        poll_.start();
}
void RecordingResourceSource::enqueue(Job job)
{
    const QString key =
        job.key + (job.thumbnail ? QStringLiteral("/thumb") : QStringLiteral("/meta"));
    if (pending_.contains(key))
        return;
    pending_.insert(key);
    if (job.thumbnail)
        jobs_.prepend(std::move(job));
    else
        jobs_.enqueue(std::move(job));
    if (!dispatchScheduled_)
    {
        dispatchScheduled_ = true;
        QTimer::singleShot(0, this,
                           [this]
                           {
                               dispatchScheduled_ = false;
                               dispatch();
                           });
    }
}
void RecordingResourceSource::dispatch()
{
    while (running_ < 2 && !jobs_.isEmpty())
    {
        const auto job = jobs_.dequeue();
        const QString pendingKey =
            job.key + (job.thumbnail ? QStringLiteral("/thumb") : QStringLiteral("/meta"));
        if (!validItem(job.path) || (!job.thumbnail && fileKey(job.path) != job.key))
        {
            pending_.remove(pendingKey);
            continue;
        }
        ++running_;
        const auto probe = probe_;
        const bool gif = kind_ == RecordingOutput::Gif;
        pool_.start(
            [this, job, pendingKey, probe, gif]
            {
                RecordingMediaInfo info;
                QImage image;
                if (job.thumbnail)
                    image = gif ? recordingGifThumbnail(job.path, job.bounds)
                                : (probe ? probe->thumbnail(job.path, job.bounds) : QImage());
                else if (probe)
                    info = probe->probe(job.path);
                else
                    info.error = QStringLiteral("媒体探测服务不可用。");
                QMetaObject::invokeMethod(
                    this,
                    [this, job, pendingKey, info, image]
                    {
                        --running_;
                        pending_.remove(pendingKey);
                        const QFileInfo file(job.path);
                        if (validItem(job.path) && file.size() == job.size &&
                            file.lastModified().toMSecsSinceEpoch() == job.mtimeMs)
                        {
                            if (job.thumbnail)
                            {
                                if (image.isNull())
                                    thumbnailFailures_.insert(job.key);
                                else
                                    thumbnails_.insert(job.key, new QImage(image));
                            }
                            else
                            {
                                if (info.valid)
                                {
                                    metadata_.insert(job.path, {job.size, job.mtimeMs, info});
                                    metadataDirty_ = true;
                                }
                                else
                                    failures_.insert(
                                        fileKey(job.path),
                                        QStringLiteral("无法读取媒体信息：%1").arg(info.error));
                                for (auto& item : snapshot_.items)
                                    if (item.id == job.path)
                                        applyMetadata(item);
                            }
                            if (!delivery_.isActive())
                                delivery_.start();
                        }
                        dispatch();
                    },
                    Qt::QueuedConnection);
            });
    }
}
ResourceImageResult RecordingResourceSource::loadImage(const QString& path, QSize bounds,
                                                       bool original) const
{
    if (original || !validItem(path))
        return {{false, QStringLiteral("录制文件已丢失或路径不在当前目录内。")}, {}};
    const QString key = fileKey(path) + QLatin1Char('/') + QString::number(bounds.width()) +
                        QLatin1Char('x') + QString::number(bounds.height());
    auto* self = const_cast<RecordingResourceSource*>(this);
    if (const auto* image = self->thumbnails_.object(key))
        return {{true, {}}, *image};
    if (thumbnailFailures_.contains(key))
        return {{false, QStringLiteral("无法读取媒体信息：首帧读取失败。")}, {}};
    const QFileInfo file(path);
    self->enqueue({path, key, file.size(), file.lastModified().toMSecsSinceEpoch(), bounds, true});
    return {{false, QStringLiteral("正在读取媒体首帧…")}, {}};
}
ResourceStatus RecordingResourceSource::writeStars(const QSet<QString>& stars) const
{
    QStringList paths(stars.begin(), stars.end());
    paths.sort();
    QJsonArray values;
    for (const auto& path : paths)
        values.append(path);
    return saveJson(starsPath_, QJsonDocument(values))
               ? ResourceStatus{true, {}}
               : ResourceStatus{false, QStringLiteral("录制星标保存失败，原星标保留。")};
}
ResourceStatus RecordingResourceSource::setStarred(const QString& path, bool starred)
{
    if (!validItem(path))
        return {false, QStringLiteral("录制文件不在当前目录内。")};
    auto candidate = stars_;
    if (starred)
        candidate.insert(path);
    else
        candidate.remove(path);
    const auto result = writeStars(candidate);
    if (result.success)
    {
        stars_ = std::move(candidate);
        rescanAndNotify();
    }
    return result;
}
ResourceStatus RecordingResourceSource::deleteItem(const QString& path)
{
    if (!validItem(path) || !actions_.trash)
        return {false, QStringLiteral("无法删除：录制文件不在当前目录内或回收站服务不可用。")};
    const auto result = actions_.trash(path);
    rescanAndNotify();
    return {result.success, result.error};
}
ResourceStatus RecordingResourceSource::revealItem(const QString& path) const
{
    if (!validItem(path) || !actions_.reveal)
        return {false, QStringLiteral("无法定位：录制文件不在当前目录内或文件服务不可用。")};
    const auto result = actions_.reveal(path);
    return {result.success, result.error};
}
ResourceStatus RecordingResourceSource::openExternal(const QString& path) const
{
    if (kind_ != RecordingOutput::Video || !validItem(path))
        return {false, QStringLiteral("无法打开：此来源不支持外部打开或文件已丢失。")};
    if (actions_.openExternal)
        return actions_.openExternal(path);
    return QDesktopServices::openUrl(QUrl::fromLocalFile(path))
               ? ResourceStatus{true, {}}
               : ResourceStatus{false, QStringLiteral("无法使用系统默认播放器打开这段录制。")};
}
ResourceStatus RecordingResourceSource::outputImage(const QString&, bool, bool) const
{
    return {false, QStringLiteral("录制来源不支持复制为图片或钉图。")};
}
void RecordingResourceSource::readMetadata()
{
    QFile file(metaPath_);
    if (!file.open(QIODevice::ReadOnly))
        return;
    const auto document = QJsonDocument::fromJson(file.readAll());
    const auto root = document.object();
    if (!document.isObject() || root.value(QStringLiteral("version")).toInt() != 1 ||
        !root.value(QStringLiteral("entries")).isObject())
    {
        cacheNotice_ += QStringLiteral("\n录制媒体缓存损坏，已回退为空并重新探测。");
        return;
    }
    const auto entries = root.value(QStringLiteral("entries")).toObject();
    for (auto it = entries.begin(); it != entries.end(); ++it)
    {
        const auto value = it.value().toObject();
        const auto size = value.value(QStringLiteral("size")).toInteger(-1);
        const auto mtime = value.value(QStringLiteral("mtimeMs")).toInteger(-1);
        const auto duration = value.value(QStringLiteral("durationMs")).toInteger(-1);
        const QSize pixels(value.value(QStringLiteral("width")).toInt(),
                           value.value(QStringLiteral("height")).toInt());
        if (!QDir::isAbsolutePath(it.key()) || !QFileInfo::exists(it.key()) || size < 0 ||
            mtime < 0 || duration < 0 || !pixels.isValid() || pixels.isEmpty())
        {
            metadataDirty_ = true;
            continue;
        }
        RecordingMediaInfo info;
        info.valid = true;
        info.durationMs = duration;
        info.pixelSize = pixels;
        metadata_.insert(it.key(), {size, mtime, info});
    }
}
bool RecordingResourceSource::writeMetadata()
{
    QJsonObject entries;
    for (auto it = metadata_.cbegin(); it != metadata_.cend(); ++it)
        entries.insert(it.key(),
                       QJsonObject{{QStringLiteral("size"), it->size},
                                   {QStringLiteral("mtimeMs"), it->mtimeMs},
                                   {QStringLiteral("durationMs"), it->info.durationMs},
                                   {QStringLiteral("width"), it->info.pixelSize.width()},
                                   {QStringLiteral("height"), it->info.pixelSize.height()}});
    const bool success =
        saveJson(metaPath_, QJsonDocument(QJsonObject{{QStringLiteral("version"), 1},
                                                      {QStringLiteral("entries"), entries}}));
    if (!success)
        cacheNotice_ = QStringLiteral("录制媒体缓存保存失败，原缓存保留。");
    metadataDirty_ = false;
    return success;
}
}
