#pragma once
#include "interfaces/recording.h"
#include "ui/history_resource_source.h"
#include <QCache>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QQueue>
#include <QSet>
#include <QThreadPool>
#include <QTimer>
namespace waibusnap
{
class RecordingResourceSource final : public ResourceSource
{
    Q_OBJECT
  public:
    using DirectoryProvider = std::function<QString()>;
    explicit RecordingResourceSource(RecordingOutput kind, DirectoryProvider directory,
                                     QString starsPath, QString metaPath,
                                     std::shared_ptr<RecordingMediaProbe> probe,
                                     HistoryWindowActions actions = {}, QObject* parent = nullptr,
                                     bool watchDirectories = true);
    ~RecordingResourceSource() override;
    QString id() const override;
    QString displayName() const override;
    bool visible() const override { return true; }
    ResourceCapabilities capabilities() const override;
    ResourceSnapshot snapshot() const override { return snapshot_; }
    void refresh() override;
    void setActive(bool active) override;
    void directoryChanged();
    ResourceImageResult loadImage(const QString& id, QSize bounds = {},
                                  bool original = false) const override;
    ResourceStatus setStarred(const QString& id, bool starred) override;
    ResourceStatus deleteItem(const QString& id) override;
    ResourceStatus revealItem(const QString& id) const override;
    ResourceStatus openExternal(const QString& id) const override;
    void setProtectedItem(const QString&) override {}
    bool canCopy() const override { return false; }
    bool canPin() const override { return false; }
    ResourceStatus outputImage(const QString&, bool, bool) const override;

  private:
    struct Metadata
    {
        qint64 size = 0;
        qint64 mtimeMs = 0;
        RecordingMediaInfo info;
    };
    struct Job
    {
        QString path;
        QString key;
        qint64 size = 0;
        qint64 mtimeMs = 0;
        QSize bounds;
        bool thumbnail = false;
    };
    QString configuredDirectory() const;
    bool mediaFile(const QFileInfo& file) const;
    bool validItem(const QString& path) const;
    static QString fileKey(const QString& path);
    ResourceStatus writeStars(const QSet<QString>& stars) const;
    void readMetadata();
    bool writeMetadata();
    void enqueue(Job job);
    void dispatch();
    void applyMetadata(ResourceItem& item) const;
    void updateMonitoring();
    void rescanAndNotify();
    QByteArray signature() const;
    RecordingOutput kind_;
    DirectoryProvider provider_;
    QString starsPath_, metaPath_, directory_, configuredDirectory_;
    std::shared_ptr<RecordingMediaProbe> probe_;
    HistoryWindowActions actions_;
    ResourceSnapshot snapshot_;
    QSet<QString> stars_;
    QHash<QString, Metadata> metadata_;
    QHash<QString, QString> failures_;
    QCache<QString, QImage> thumbnails_{32};
    QSet<QString> thumbnailFailures_;
    QQueue<Job> jobs_;
    QSet<QString> pending_;
    QThreadPool pool_;
    int running_ = 0;
    QFileSystemWatcher watcher_;
    QTimer debounce_, poll_, delivery_;
    QString cacheNotice_;
    bool metadataDirty_ = false;
    bool active_ = false;
    bool dispatchScheduled_ = false;
    bool watchDirectories_;
};
}
