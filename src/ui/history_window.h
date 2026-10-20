#pragma once
#include "app/history_manager.h"
#include <QGraphicsView>
#include <QWidget>
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
    explicit HistoryWindow(HistoryManager& manager, AppSettings settings);
    void refresh();

  protected:
    void closeEvent(QCloseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void rebuildGroups(const QString& view);
    void selectView(const QString& view, bool useDefaultSort);
    void rebuildItems(const QString& selected);
    void showSelection();
    void scheduleThumbnails();
    void loadVisibleThumbnails();
    void updateLayout();
    void restoreWindowState(const HistoryWindowState& state);
    void saveWindowState();
    HistoryManager& manager_;
    AppSettings settings_;
    HistorySnapshot snapshot_;
    QVector<HistoryItem> items_;
    QString view_ = QStringLiteral("all");
    HistorySort order_ = HistorySort::NewestFirst;
    QListWidget* groups_;
    QListWidget* thumbnails_;
    QComboBox* groupCombo_;
    QComboBox* sortCombo_;
    QToolButton* collapse_;
    QSplitter* splitter_;
    QSplitter* imageSplitter_;
    HistoryImageView* imageView_;
    QLabel* empty_;
    QLabel* notice_;
    QLabel* status_;
    bool thumbnailsPending_ = false;
    bool narrow_ = false;
};
}
