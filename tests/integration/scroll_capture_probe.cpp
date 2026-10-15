#include "app/scroll_capture.h"
#include "interfaces/display_topology.h"
#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QScreen>
#include <QTimer>
using namespace waibusnap;
DisplayResult scrollProbeDisplay(int index);
bool scrollProbeActivate(const QString& bundleId);
namespace waibusnap
{
struct ScrollCaptureTestAccess
{
    static ScrollCaptureSession* create(RegionCaptureRequest request)
    {
        const QPoint point =
            request.display.logicalGeometry.topLeft() +
            QPoint(qRound(request.pixelRect.center().x() / request.display.devicePixelRatio),
                   qRound(request.pixelRect.center().y() / request.display.devicePixelRatio));
        // 取证固定投递位置，避免操作者查看日志时的鼠标位置影响浏览器对照。
        // 暂挂 / 光标命中规则另由冒烟与人工清单验收。
        return new ScrollCaptureSession(
            request, {}, createRegionCaptureSession(request), createScrollInjector(),
            [point] { return point; },
            ScrollStitchLimits::forDisplay(request.display.devicePixelRatio), nullptr);
    }
};
}
// 人工取证吊具：复用真实采集与自动驱动；不自动授权、不读取浏览器内部状态。
int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    const QStringList args = app.arguments();
    for (QScreen* screen : app.screens())
        qInfo() << "屏幕：" << screen->name() << screen->geometry() << screen->devicePixelRatio()
                << screen->refreshRate();
    if (args.size() != 9 && args.size() != 10)
    {
        qInfo("用法：scroll_capture_probe 手动|自动 左 上 宽 高 秒数 输出目录 "
              "屏幕序号 [浏览器 bundle id]；区域为屏内逻辑坐标");
        return 2;
    }
    int values[6]{};
    const int numericArguments[] = {2, 3, 4, 5, 6, 8};
    for (int index = 0; index < 6; ++index)
    {
        bool valid = false;
        values[index] = args[numericArguments[index]].toInt(&valid);
        if (!valid || values[index] < 0)
        {
            qWarning("区域、秒数与屏幕序号必须为非负整数。");
            return 2;
        }
    }
    if ((args[1] != QStringLiteral("手动") && args[1] != QStringLiteral("自动")) ||
        values[2] < 32 || values[3] < 32 || values[4] < 1 || values[4] > 120)
    {
        qWarning("模式须为手动或自动；区域至少 32×32 逻辑像素；时长须为 1–120 秒。");
        return 2;
    }
    if (!regionCaptureSupported())
    {
        qWarning("当前平台不支持真实区域采集。");
        return 3;
    }
    const auto target = scrollProbeDisplay(values[5]);
    if (!target.error.isEmpty())
    {
        qWarning().noquote() << target.error;
        return 4;
    }
    qInfo() << "取证目标：" << target.target.id << target.target.logicalGeometry;
    const QSize available = target.target.logicalGeometry.size();
    if (values[0] > available.width() - values[2] || values[1] > available.height() - values[3])
    {
        qWarning("区域超出当前屏幕；请按本次枚举的屏幕布局重新指定。");
        return 2;
    }
    const qreal dpr = target.target.devicePixelRatio;
    const QRect rectangle(qRound(values[0] * dpr), qRound(values[1] * dpr), qRound(values[2] * dpr),
                          qRound(values[3] * dpr));
    const QDir directory(args[7]);
    if (!directory.exists() || directory.exists(QStringLiteral("result.png")))
    {
        qWarning("输出目录须已存在且不含 result.png，避免覆盖取证。");
        return 5;
    }
    std::unique_ptr<ScrollCaptureSession> owned(
        ScrollCaptureTestAccess::create({target.target, rectangle, {}}));
    auto& session = *owned;
    QObject::connect(&session, &ScrollCaptureSession::permissionMissing, &app,
                     [&]
                     {
                         session.stop();
                         app.exit(7);
                     });
    QObject::connect(&session, &ScrollCaptureSession::noticeChanged,
                     [](const QString& message) { qInfo().noquote() << "提示：" << message; });
    QObject::connect(&session, &ScrollCaptureSession::completed, &app,
                     [&]
                     {
                         QImage result = session.takeResult();
                         qInfo() << "取证结束：" << result.size() << session.stats().warnings;
                         app.exit(
                             result.save(directory.filePath(QStringLiteral("result.png"))) ? 0 : 6);
                     });
    if (args.size() == 10 && !scrollProbeActivate(args[9]))
    {
        qWarning("无法置前指定测试浏览器。");
        return 8;
    }
    QTimer::singleShot(1000, &session,
                       [&]
                       {
                           session.start();
                           if (args[1] == QStringLiteral("自动"))
                               session.setAutomatic(true);
                       });
    QTimer::singleShot((values[4] + 1) * 1000, &session, &ScrollCaptureSession::complete);
    return app.exec();
}
