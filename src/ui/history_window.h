#pragma once
#include "app/history_manager.h"
#include "output/image_output.h"
#include <QGraphicsView>
#include <QWidget>
class QComboBox;
class QLabel;
class QListWidget;
class QSplitter;
class QToolButton;
namespace waibusnap
{
struct HistoryWindowActions
{
    std::function<ImageOutputResult(const QImage&)> copyImage;
    std::function<ImageOutputResult(const QImage&)> pinImage;
    HistoryTrash trash;
    HistoryTrash reveal;
};
class HistoryImageItem;
class HistoryImageView final : public QGraphicsView
{
    Q_OBJECT
  public:
    explicit HistoryImageView(QWidget* parent = nullptr);
    void setImage(QImage image);
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
                           HistoryWindowActions actions = {});
    ~HistoryWindow() override;
    void refresh();
    void navigate(int step, bool allHistory = false);
    QString selectedItemId() const { return selectedId(); }
    bool dismissGroupDialogs();
    void setGroupActionsEnabled(bool enabled);
  signals:
    void groupDialogOpenChanged(bool open);

  protected:
    void closeEvent(QCloseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void rebuildGroups(const QString& view);
    void editGroupName(const QString& groupId = {});
    void showGroupMenu(const QPoint& point);
    void openGroupSettings(const QString& groupId);
    void deleteGroup(const QString& groupId);
    void selectView(const QString& view, bool useDefaultSort);
    void rebuildItems(const QString& selected);
    void showSelection();
    QString selectedId() const;
    void updateActions();
    void toggleStar();
    void outputImage(bool pin, bool closeAfter = false);
    void deleteSelection();
    void updateNotice();
    void updateStatus();
    void showActionResult(const HistoryStatus& result);
    void scheduleThumbnails();
    void loadVisibleThumbnails();
    void updateLayout();
    void restoreWindowState(const HistoryWindowState& state);
    void saveWindowState();
    HistoryManager& manager_;
    AppSettings settings_;
    HistoryWindowActions actions_;
    HistorySnapshot snapshot_;
    QVector<HistoryItem> items_;
    QString view_ = QStringLiteral("all");
    HistorySort order_ = HistorySort::NewestFirst;
    QListWidget* groups_;
    QListWidget* thumbnails_;
    QComboBox* groupCombo_;
    QComboBox* sortCombo_;
    QToolButton* collapse_;
    QToolButton* star_;
    QToolButton* pin_;
    QToolButton* copy_;
    QToolButton* reveal_;
    QToolButton* delete_;
    QSplitter* splitter_;
    QSplitter* imageSplitter_;
    HistoryImageView* imageView_;
    QLabel* empty_;
    QLabel* notice_;
    QLabel* status_;
    bool thumbnailsPending_ = false;
    bool narrow_ = false;
    bool groupActionsEnabled_ = true;
};
}
