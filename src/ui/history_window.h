#pragma once
#include "ui/history_resource_source.h"
#include <QGraphicsView>
#include <QPointer>
#include <QWidget>
#include <optional>
class QComboBox;
class QLabel;
class QListWidget;
class QSplitter;
class QToolButton;
namespace waibusnap
{
class HistoryImageItem;
class HistoryImageView final : public QGraphicsView
{
    Q_OBJECT
  public:
    explicit HistoryImageView(QWidget* parent = nullptr);
    void setImage(QImage image, bool preserveView = false);
    QImage image() const;
    qreal zoom() const { return transform().m11(); }
    void fitImage();
    void actualSize();

  protected:
    void wheelEvent(QWheelEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void drawBackground(QPainter* painter, const QRectF& rect) override;

  private:
    HistoryImageItem* item_;
    bool fitting_ = true;
};
class HistoryWindow final : public QWidget
{
    Q_OBJECT
  public:
    explicit HistoryWindow(HistoryManager& manager, AppSettings settings,
                           HistoryWindowActions actions = {},
                           ResourceSource* additionalSource = nullptr, bool historyEnabled = true);
    ~HistoryWindow() override;
    void refresh();
    void selectSource(const QString& sourceId);
    QString selectedSourceId() const { return source_ ? source_->id() : QString(); }
    void setHistoryEnabled(bool enabled);
    void navigate(int step, bool allHistory = false);
    QString selectedItemId() const { return selectedId(); }
    bool dismissGroupDialogs();
    void setGroupActionsEnabled(bool enabled);
  signals:
    void groupDialogOpenChanged(bool open);
    void sourceChanged(QString sourceId);
    void directoryRequested(QString sourceId);

  protected:
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void refreshFromSource();
    void rebuildSources();
    void clearSource();
    void rebuildGroups(const QString& view);
    void editGroupName(const QString& groupId = {});
    void showGroupMenu(const QPoint& point);
    void selectView(const QString& view, bool useDefaultSort);
    void rebuildItems(const QString& selected, int fallbackRow = 0);
    void showSelection();
    QString selectedId() const;
    std::optional<ResourceItem> findItem(const QString& id) const;
    void updateActions();
    void toggleStar();
    void outputImage(bool pin, bool closeAfter = false);
    void deleteSelection();
    void updateNotice();
    void updateStatus();
    void showActionResult(const ResourceStatus& result);
    void scheduleThumbnails();
    void loadVisibleThumbnails();
    void updateLayout();
    void restoreWindowState(const HistoryWindowState& state);
    void saveWindowState();
    HistoryResourceSource* historySource_;
    QPointer<ResourceSource> source_;
    QVector<ResourceSource*> sources_;
    struct SourceView
    {
        QString view = QStringLiteral("all");
        QString selected;
        ResourceSort order = ResourceSort::NewestFirst;
    };
    QHash<QString, SourceView> sourceViews_;
    AppSettings settings_;
    ResourceSnapshot snapshot_;
    QVector<ResourceItem> items_;
    QString displayedId_;
    bool displayedOriginal_ = false;
    QString view_ = QStringLiteral("all");
    ResourceSort order_ = ResourceSort::NewestFirst;
    QListWidget* groups_;
    QListWidget* thumbnails_;
    QComboBox* sourceCombo_;
    QComboBox* groupCombo_;
    QComboBox* sortCombo_;
    QToolButton* collapse_;
    QToolButton* star_;
    QToolButton* pin_;
    QToolButton* copy_;
    QToolButton* original_;
    QToolButton* reveal_;
    QToolButton* delete_;
    QToolButton* clear_;
    QToolButton* configure_;
    QSplitter* splitter_;
    QSplitter* imageSplitter_;
    HistoryImageView* imageView_;
    QLabel* empty_;
    QLabel* notice_;
    QLabel* status_;
    bool showOriginal_ = false;
    bool thumbnailsPending_ = false;
    bool narrow_ = false;
    bool groupActionsEnabled_ = true;
};
}
