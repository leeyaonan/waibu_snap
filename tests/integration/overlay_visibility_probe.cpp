#include "app/application_controller.h"
#include "interfaces/platform_workarounds.h"
#include <QApplication>
#include <QDebug>
#include <QScreen>
#include <QTemporaryDir>
#include <QTimer>
using namespace waibusnap;
DisplayResult scrollProbeDisplay(int index);
namespace waibusnap
{
class ProbeDisplays final : public DisplayTopology
{
  public:
    explicit ProbeDisplays(DisplayTarget target) : target_(target), real_(createDisplayTopology())
    {
    }
    DisplayResult cursorDisplay() const override { return {target_, {}}; }
    bool stillMatches(const DisplayTarget& target) const override
    {
        return real_->stillMatches(target);
    }

  private:
    DisplayTarget target_;
    std::unique_ptr<DisplayTopology> real_;
};
struct ApplicationControllerTestAccess
{
    static void target(ApplicationController& controller, DisplayTarget display)
    {
        controller.displays_ = std::make_unique<ProbeDisplays>(display);
    }
};
}
// 人工验收吊具：仅替换光标选屏，捕获、覆盖层、在屏查询、就绪及 finish 全走生产路径。
int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    installPlatformCompatibilityWorkarounds();
    app.setApplicationName(QStringLiteral("WaibuSnap"));
    const auto args = app.arguments();
    for (QScreen* screen : app.screens())
        qInfo() << "屏幕：" << screen->name() << screen->geometry() << screen->devicePixelRatio();
    if (args.size() != 5)
    {
        qWarning("用法：overlay_visibility_probe 屏幕序号 次数（0为人工持续验收） 会话JSONL "
                 "临时设置INI");
        return 2;
    }
    bool screenOk = false, countOk = false;
    const int index = args[1].toInt(&screenOk);
    const int count = args[2].toInt(&countOk);
    const auto display = scrollProbeDisplay(index);
    if (!screenOk || !countOk || count < 0 || count > 1000 || !display.error.isEmpty())
    {
        qWarning().noquote() << "参数无效：" << display.error;
        return 2;
    }
    qInfo() << "真实采集目标：" << display.target.logicalGeometry;
    QTemporaryDir history;
    if (!history.isValid())
        return 2;
    RunOptions options;
    options.historyRoot = history.filePath(QStringLiteral("history"));
    options.testMode = count != 0;
    options.testCount = count;
    options.stableMs = 500;
    options.intervalMs = 100;
    options.holdMs = 100;
    options.metricsFile = args[3];
    options.settingsFile = args[4];
    ApplicationController controller(app, options);
    ApplicationControllerTestAccess::target(controller, display.target);
    controller.start();
    if (!count)
        QTimer::singleShot(500, &controller,
                           [&] { controller.trigger(QStringLiteral("manual-probe")); });
    else
        QTimer::singleShot(60000, &app,
                           [&]
                           {
                               qWarning("取证超时");
                               app.exit(10);
                           });
    const int result = app.exec();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    int overlays = 0;
    for (QWidget* widget : QApplication::topLevelWidgets())
        if (qobject_cast<SelectionOverlay*>(widget))
            ++overlays;
    qInfo() << "取证退出：active=" << controller.active() << " 残留覆盖层=" << overlays;
    return result;
}
