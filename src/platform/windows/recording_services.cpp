#include "interfaces/recording.h"
#include <QCoreApplication>
#include <QFileInfo>
#include <QMetaObject>
namespace waibusnap
{
namespace
{
class WindowsRecordingMediaProbe final : public RecordingMediaProbe
{
  public:
    RecordingMediaInfo probe(const QString& path) const override
    {
        if (QFileInfo(path).suffix().compare(QStringLiteral("gif"), Qt::CaseInsensitive) == 0)
            return probeRecordingGif(path);
        RecordingMediaInfo info;
        info.error = QStringLiteral("Windows 录制将在后续版本接入。");
        return info;
    }
    QImage thumbnail(const QString& path, QSize bounds) const override
    {
        return QFileInfo(path).suffix().compare(QStringLiteral("gif"), Qt::CaseInsensitive) == 0
                   ? recordingGifThumbnail(path, bounds)
                   : QImage();
    }
};
} // namespace
std::unique_ptr<RecordingSession> createRecordingSession() { return {}; }
bool recordingCaptureSupported() { return false; }
bool hasScreenRecordingPermission() { return false; }
bool requestScreenRecordingPermission() { return false; }
MicrophonePermission microphonePermission() { return MicrophonePermission::NotDetermined; }
void requestMicrophonePermission(std::function<void(bool)> callback)
{
    if (QCoreApplication::instance())
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [callback = std::move(callback)]
            {
                if (callback)
                    callback(false);
            },
            Qt::QueuedConnection);
}
QString microphonePermissionExplanation()
{
    return QStringLiteral("Windows 麦克风录制将在后续版本接入。");
}
QVector<MicrophoneDevice> microphoneDevices() { return {}; }
QString defaultMicrophoneDeviceId() { return {}; }
std::unique_ptr<RecordingMediaProbe> createRecordingMediaProbe()
{
    return std::make_unique<WindowsRecordingMediaProbe>();
}
QVector<RecordingWindowInfo> recordingWindows() { return {}; }
std::optional<RecordingWindowInfo> recordingWindowInfo(quint64) { return std::nullopt; }
namespace
{
class WindowsRecordingEnvironmentObserver final : public RecordingEnvironmentObserver
{
  public:
    void start(std::function<void()>) override {}
    void stop() override {}
};
}
std::unique_ptr<RecordingEnvironmentObserver> createRecordingEnvironmentObserver()
{
    return std::make_unique<WindowsRecordingEnvironmentObserver>();
}
} // namespace waibusnap
