#pragma once
#include "ui/sticker_window.h"
#include <QObject>
#include <QPointer>
#include <QVector>
namespace waibusnap
{
class StickerManager final : public QObject
{
    Q_OBJECT
  public:
    explicit StickerManager(QObject* parent = nullptr, StickerActions actions = {});
    ~StickerManager() override;
    ImageOutputResult create(const QImage& image, QPoint position, bool saved = false);
    void closeAll();
    int count() const { return int(windows_.size()); }
    QVector<QPointer<StickerWindow>> windows() const { return windows_; }
    void recoverWindows();

  private:
    void watchScreen(QScreen* screen);
    StickerActions actions_;
    QVector<QPointer<StickerWindow>> windows_;
    // 用户关闭后的延迟删除窗口也由管理器兜底，退出不能遗漏待删除资源。
    QVector<QPointer<StickerWindow>> ownedWindows_;
    bool closingAll_ = false;
};
}
