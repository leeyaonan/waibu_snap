#include "interfaces/platform_info.h"
#include "ui/startup_window.h"

#include <QApplication>

#ifdef WAIBUSNAP_ENABLE_SMOKE_TEST
#include <QTimer>
#endif

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    QApplication::setApplicationName(QStringLiteral("WaibuSnap"));
    application.setQuitOnLastWindowClosed(true);

    waibusnap::StartupWindow window;
    window.setWindowTitle(QStringLiteral("WaibuSnap — %1").arg(waibusnap::platformName()));
    window.show();

#ifdef WAIBUSNAP_ENABLE_SMOKE_TEST
    if (application.arguments().contains(QStringLiteral("--smoke-test")))
    {
        // 由正常的窗口关闭触发退出，不能用 quit() 掩盖生命周期错误。
        QTimer::singleShot(0, &window,
                           [&application, &window]()
                           {
                               if (!window.isVisible())
                               {
                                   application.exit(2);
                                   return;
                               }
                               window.close();
                           });
    }
#endif

    return application.exec();
}
