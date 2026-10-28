#pragma once
#include "core/recording.h"
#include "interfaces/display_topology.h"
#include <QImage>
#include <QRect>
#include <QSize>
#include <QString>
#include <QVector>
#include <functional>
#include <memory>
namespace waibusnap
{
enum class RecordingEventType
{
    Started,
    Progress,
    Warning,
    AutoStop,
    Finished,
    Failed,
    PermissionDenied
};
struct RecordingEvent
{
    RecordingEventType type = RecordingEventType::Started;
    qint64 recordedMs = 0;
    qint64 bytes = 0;
    // Finished：成品路径；Failed：可恢复临时路径（如有）。
    QString path;
    QSize pixelSize;
    int droppedFrames = 0;
    bool autoStopped = false;
    QString message;
};
struct RecordingRequest
{
    // R2 只支持 Video；Gif 返回明确错误。
    RecordingOutput output = RecordingOutput::Video;
    RecordingTargetKind targetKind = RecordingTargetKind::Display;
    DisplayTarget display;
    // 区域：显示器内物理像素；全屏：整个 display。
    QRect sourcePixelRect;
    // 窗口目标；区域 / 全屏为 0。
    quint64 windowId = 0;
    // 编码输出尺寸；视频宽高必须是正偶数。
    QSize outputPixels;
    int fps = 30;
    RecordingQuality quality = RecordingQuality::High;
    bool systemAudio = true;
    bool microphoneEnabled = false;
    // 空表示系统默认输入；非空表示指定设备。
    QString microphoneDeviceId;
    double systemGain = 1.0;
    double microphoneGain = 1.0;
    bool showCursor = true;
    // R2 不实现 GIF 限时；视频忽略该值，0 表示不限。
    int maxDurationMs = 0;
    // 目标目录内隐藏 .part；停止成功后原子改名为 finalPath。
    QString tempPath;
    QString finalPath;
};
class RecordingSession
{
  public:
    using EventCallback = std::function<void(RecordingEvent)>;
    virtual ~RecordingSession() = default;
    // GUI 线程 start / pause / resume / stop / discard；事件在 GUI 线程交付。
    // 对象必须存活到 Finished / Failed；析构后不得再回调。
    // start 的校验失败（参数非法、输出为 GIF）直接返回 false + 中文 error；
    // 屏幕录制权限拒绝时在 start 返回 false 前同步交付 PermissionDenied，回调内不得销毁会话。
    virtual bool start(const RecordingRequest& request, EventCallback callback, QString* error) = 0;
    // 暂停 / 恢复 / 停止 / 丢弃均幂等；终态后调用无副作用。
    virtual void pause() = 0;
    virtual void resume() = 0;
    virtual void stop() = 0;
    virtual void discard() = 0;
};
std::unique_ptr<RecordingSession> createRecordingSession();
bool recordingCaptureSupported();
bool hasScreenRecordingPermission();
bool requestScreenRecordingPermission();
// 只校验参数，不启动采集、不查询权限，合法请求返回空字符串。
QString validateRecordingRequest(const RecordingRequest& request);
enum class MicrophonePermission
{
    Authorized,
    Denied,
    NotDetermined
};
// 查询不触发权限弹窗；请求总是异步在 GUI 线程交付，超时 / 拒绝返回 false。
MicrophonePermission microphonePermission();
void requestMicrophonePermission(std::function<void(bool)> callback);
QString microphonePermissionExplanation();
struct MicrophoneDevice
{
    QString id;
    QString name;
    bool isDefault = false;
};
QVector<MicrophoneDevice> microphoneDevices();
QString defaultMicrophoneDeviceId();
struct RecordingMediaInfo
{
    bool valid = false;
    qint64 durationMs = 0;
    QSize pixelSize;
    QString error;
};
class RecordingMediaProbe
{
  public:
    virtual ~RecordingMediaProbe() = default;
    // 可在线程池调用；失败返回 valid=false 与中文原因，不抛异常。
    virtual RecordingMediaInfo probe(const QString& path) const = 0;
    virtual QImage thumbnail(const QString& path, QSize bounds) const = 0;
};
std::unique_ptr<RecordingMediaProbe> createRecordingMediaProbe();
} // namespace waibusnap
