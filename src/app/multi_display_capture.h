#pragma once
#include "app/app_settings.h"
#include "app/capture_directory.h"
#include "core/multi_display.h"
#include "interfaces/capture_provider.h"
#include "interfaces/display_topology.h"
#include "output/image_output.h"
#include "output/multi_display_output.h"
#include <QImage>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>
namespace waibusnap
{
struct MultiDisplayCaptureDependencies
{
    // 平台是否支持多屏采集（Windows 为 false，入口置灰）。
    std::function<bool()> supported = [] { return true; };
    // 常驻水印：单屏原图各打一次，拼接图整体再打一次。
    std::function<QImage(const QImage&)> watermark = [](const QImage& image) { return image; };
    std::function<MultiDisplaySaveResult(const MultiDisplaySaveRequest&)> saveGroup;
    std::function<bool()> stitchEnabled;
    std::function<QStringList()> reservedDirectories;
    std::function<void(const QString&)> notice;
    // index 为该屏在排布中的序号，用于错开闪光时机。
    std::function<void(QRect, int index)> flash;
};
// 一次触发 = 一组：并发采集全部启用显示器，全部成功才落盘，任一失败整组放弃。
class MultiDisplayCaptureController final : public QObject
{
    Q_OBJECT
  public:
    MultiDisplayCaptureController(AppSettings settings, DisplayTopology& displays,
                                  CaptureProvider& capture,
                                  MultiDisplayCaptureDependencies dependencies = {},
                                  QObject* parent = nullptr);
    ~MultiDisplayCaptureController() override;
    bool busy() const { return busy_; }
    int pendingDisplays() const { return pending_; }
    // 空串表示已开始采集；否则为可直接展示的拒绝原因。
    QString start();
  signals:
    void started();
    void finished(QString summary);
    void failed(QString reasons);
    void busyChanged(bool busy);

  private:
    struct Pending
    {
        QVector<DisplayTarget> displays;
        QVector<QImage> frames;
        QVector<bool> done;
        QStringList failures;
        quint64 token = 0;
    };
    void collect(const std::shared_ptr<Pending>& pending);
    void conclude(const std::shared_ptr<Pending>& pending);
    void setBusy(bool busy);
    AppSettings settings_;
    DisplayTopology& displays_;
    CaptureProvider& capture_;
    MultiDisplayCaptureDependencies dependencies_;
    bool busy_ = false;
    int pending_ = 0;
    quint64 token_ = 0;
};
}
