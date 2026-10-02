#include "app/application_controller.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QStandardPaths>
int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    QApplication::setApplicationName(QStringLiteral("WaibuSnap"));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("WaibuSnap V02 最小截图原型"));
    parser.addHelpOption();
    parser.addOption(
        {"metrics-file", QStringLiteral("会话 JSONL 文件（只记录时间和尺寸）"), "path"});
    parser.addOption({"test-mode", QStringLiteral("显式启用受控截图测试，空闲测量不得启用")});
    parser.addOption({"test-count", QStringLiteral("受控模式截图次数"), "count", "1"});
    parser.addOption({"test-stable-ms", QStringLiteral("新进程稳定等待毫秒"), "ms", "10000"});
    parser.addOption({"test-interval-ms", QStringLiteral("后续截图间隔毫秒"), "ms", "1000"});
    parser.addOption({"test-hold-ms", QStringLiteral("可交互后保持覆盖层毫秒"), "ms", "250"});
#ifdef WAIBUSNAP_ENABLE_SMOKE_TEST
    parser.addOption({"smoke-test", QStringLiteral("无头生命周期冒烟入口")});
#endif
    parser.process(application);
    waibusnap::RunOptions options;
    options.metricsFile = parser.value("metrics-file");
    if (options.metricsFile.isEmpty())
        options.metricsFile =
            QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                .filePath(QStringLiteral("sessions.jsonl"));
    options.testMode = parser.isSet("test-mode");
#ifdef WAIBUSNAP_ENABLE_SMOKE_TEST
    options.smokeTest = parser.isSet("smoke-test");
#endif
    auto parseNumber = [&parser](const QString& name, int minimum, int maximum, int& destination)
    {
        bool ok = false;
        destination = parser.value(name).toInt(&ok);
        return ok && destination >= minimum && destination <= maximum;
    };
    if (!parseNumber("test-count", 1, 1000, options.testCount) ||
        !parseNumber("test-stable-ms", 0, 600000, options.stableMs) ||
        !parseNumber("test-interval-ms", 0, 600000, options.intervalMs) ||
        !parseNumber("test-hold-ms", 0, 60000, options.holdMs))
    {
        qCritical("测试参数超出范围。");
        return 2;
    }
    for (const QString& name : {QStringLiteral("test-count"), QStringLiteral("test-stable-ms"),
                                QStringLiteral("test-interval-ms"), QStringLiteral("test-hold-ms")})
        if (parser.isSet(name) && !options.testMode)
        {
            qCritical("受控参数必须显式启用 --test-mode。");
            return 2;
        }
    waibusnap::ApplicationController controller(application, options);
    controller.start();
    return application.exec();
}
