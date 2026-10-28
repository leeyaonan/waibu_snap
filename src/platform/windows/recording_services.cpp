#include "interfaces/recording.h"
namespace waibusnap
{
namespace
{
class WindowsRecordingMediaProbe final : public RecordingMediaProbe
{
  public:
    RecordingMediaInfo probe(const QString&) const override
    {
        RecordingMediaInfo info;
        info.error = QStringLiteral("Windows 录制将在后续版本接入。");
        return info;
    }
    QImage thumbnail(const QString&, QSize) const override { return {}; }
};
} // namespace
std::unique_ptr<RecordingSession> createRecordingSession() { return {}; }
bool recordingCaptureSupported() { return false; }
bool hasScreenRecordingPermission() { return false; }
bool requestScreenRecordingPermission() { return false; }
std::unique_ptr<RecordingMediaProbe> createRecordingMediaProbe()
{
    return std::make_unique<WindowsRecordingMediaProbe>();
}
} // namespace waibusnap
