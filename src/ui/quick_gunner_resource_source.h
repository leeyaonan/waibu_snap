#pragma once
#include "ui/history_resource_source.h"
#include <QFileSystemWatcher>
#include <QSet>
#include <QTimer>
namespace waibusnap
{
class QuickGunnerResourceSource final : public ResourceSource
{
    Q_OBJECT
  public:
    using DirectoryProvider = std::function<QString()>;
    explicit QuickGunnerResourceSource(DirectoryProvider directory, QString starsPath,
                                       HistoryWindowActions actions = {}, QObject* parent = nullptr,
                                       bool watchDirectories = true);
    QString id() const override;
    QString displayName() const override;
    bool visible() const override;
    ResourceCapabilities capabilities() const override;
    ResourceSnapshot snapshot() const override { return snapshot_; }
    void refresh() override;
    void setActive(bool active) override;
    // provider 更新后由控制器主动通知，即使当前来源未被选中也同步可见性。
    void directoryChanged();
    ResourceImageResult loadImage(const QString& id, QSize bounds = {},
                                  bool original = false) const override;
    ResourceStatus setStarred(const QString& id, bool starred) override;
    ResourceStatus deleteItem(const QString& id) override;
    ResourceStatus revealItem(const QString& id) const override;
    ResourceClearResult clearAll() override;
    void setProtectedItem(const QString&) override {}
    bool canCopy() const override;
    bool canPin() const override;
    ResourceStatus outputImage(const QString& id, bool original, bool pin) const override;

  private:
    bool validItem(const QString& path) const;
    ResourceStatus writeStars(const QSet<QString>& stars) const;
    void updateMonitoring();
    void rescanAndNotify();
    DirectoryProvider provider_;
    QString starsPath_;
    QString directory_;
    QString configuredDirectory_;
    HistoryWindowActions actions_;
    ResourceSnapshot snapshot_;
    QSet<QString> stars_;
    QByteArray signature_;
    QFileSystemWatcher watcher_;
    QTimer debounce_;
    QTimer poll_;
    bool active_ = false;
    bool watchDirectories_;
};
}
