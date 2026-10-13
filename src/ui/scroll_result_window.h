#pragma once
#include "core/scroll_stitcher.h"
#include "output/image_save.h"
#include <QWidget>
class QLabel;
class QScrollArea;
namespace waibusnap
{
class ScrollResultView;
struct ScrollResultActions : ImageSaveActions
{
    std::function<ImageOutputResult(const QImage&)> copyImage;
    std::function<ImageOutputResult(const QImage&)> pinImage;
    std::function<bool(QWidget*)> confirmDiscard;
};
class ScrollResultWindow final : public QWidget
{
    Q_OBJECT
  public:
    ScrollResultWindow(QImage image, ScrollStitchStats stats, ScrollResultActions actions = {});
    const QImage& image() const { return image_; }
    bool hasOutput() const { return output_; }
    qreal zoom() const;
    void closeWithoutConfirmation();
  signals:
    void retryRequested();

  protected:
    void closeEvent(QCloseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;

  private:
    void fit(bool widthOnly = false);
    void setZoom(qreal scale);
    void setCropping(bool enabled);
    void updateStatus();
    void save();
    void applyCrop();
    void report(ImageOutputResult result);
    QImage image_;
    ScrollResultActions actions_;
    ScrollResultView* view_ = nullptr;
    QScrollArea* scrollArea_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* notice_ = nullptr;
    QWidget* outputBar_ = nullptr;
    QWidget* cropBar_ = nullptr;
    bool output_ = false;
    bool closing_ = false;
    bool saving_ = false;
    bool initialFit_ = false;
};
}
