#pragma once
#include "app/history_manager.h"
#include "output/image_output.h"
#include "ui/resource_source.h"
class QWidget;
namespace waibusnap
{
struct HistoryWindowActions
{
    std::function<ImageOutputResult(const QImage&)> copyImage;
    std::function<ImageOutputResult(const QImage&)> pinImage;
    HistoryTrash trash;
    HistoryTrash reveal;
    std::function<ResourceStatus(const QString&)> openExternal;
};
class HistoryResourceSource final : public ResourceSource
{
    Q_OBJECT
  public:
    explicit HistoryResourceSource(HistoryManager& manager, HistoryWindowActions actions = {},
                                   QObject* parent = nullptr);
    QString id() const override;
    QString displayName() const override;
    bool visible() const override;
    void setEnabled(bool enabled);
    ResourceCapabilities capabilities() const override;
    ResourceSnapshot snapshot() const override;
    void refresh() override;
    ResourceImageResult loadImage(const QString& id, QSize bounds = {},
                                  bool original = false) const override;
    ResourceStatus setStarred(const QString& id, bool starred) override;
    ResourceStatus deleteItem(const QString& id) override;
    ResourceStatus revealItem(const QString& id) const override;
    void setProtectedItem(const QString& id) override;
    bool canCopy() const override;
    bool canPin() const override;
    ResourceStatus outputImage(const QString& id, bool original, bool pin) const override;
    // 历史专属的受控界面入口，不属于通用来源契约。
    void editGroupName(const QString& groupId, QWidget* parent);
    void showGroupMenu(const QString& groupId, const QPoint& position, QWidget* parent);
    void setGroupActionsEnabled(bool enabled) { groupActionsEnabled_ = enabled; }
  signals:
    void groupDialogOpenChanged(bool open);
    void groupViewRequested(QString groupId);
    void actionFinished(waibusnap::ResourceStatus result);

  private:
    void openGroupSettings(const QString& groupId, QWidget* parent);
    void deleteGroup(const QString& groupId, QWidget* parent);
    HistoryManager& manager_;
    HistoryWindowActions actions_;
    bool groupActionsEnabled_ = true;
    bool enabled_ = true;
};
}
