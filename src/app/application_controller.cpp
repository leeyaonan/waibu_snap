#include "app/application_controller.h"
#include "session/monotonic_clock.h"
#include <QAction>
#include <QDebug>
#include <QMessageBox>
#include <QPainter>
#include <QScreen>
#include <QTimer>
namespace waibusnap
{
ApplicationController::ApplicationController(QApplication& application, RunOptions options)
    : application_(application), options_(std::move(options)), hotkey_(createGlobalHotkey()),
      displays_(createDisplayTopology()), capture_(createCaptureProvider())
{
    application_.setQuitOnLastWindowClosed(false);
    captureTimeout_.setSingleShot(true);
    connect(&captureTimeout_, &QTimer::timeout, this,
            [this]
            {
                if (active_)
                    fail(6, QStringLiteral("截图会话超时，请重新尝试。"));
            });
    menu_.addAction(QStringLiteral("截图"), this, [this] { trigger(QStringLiteral("tray")); });
    menu_.addAction(QStringLiteral("退出"), this, &ApplicationController::quit);
    QPixmap icon(32, 32);
    icon.fill(Qt::transparent);
    QPainter painter(&icon);
    painter.setPen(QPen(QColor(245, 130, 35), 3));
    painter.drawRoundedRect(QRectF(5, 8, 22, 18), 3, 3);
    painter.drawEllipse(QPointF(16, 17), 5, 5);
    painter.end();
    tray_.setIcon(QIcon(icon));
    tray_.setContextMenu(&menu_);
    // 热插拔或屏幕参数变更只响应通知，立即取消当前会话。
    connect(&application_, &QGuiApplication::screenRemoved, this,
            [this](QScreen*)
            {
                if (active_)
                    finish({}, 4);
            });
    const auto watchScreen = [this](QScreen* screen)
    {
        connect(screen, &QScreen::geometryChanged, this,
                [this]
                {
                    if (active_)
                        finish({}, 4);
                });
        connect(screen, &QScreen::logicalDotsPerInchChanged, this,
                [this]
                {
                    if (active_)
                        finish({}, 4);
                });
    };
    for (QScreen* screen : QGuiApplication::screens())
        watchScreen(screen);
    connect(&application_, &QGuiApplication::screenAdded, this,
            [this, watchScreen](QScreen* screen)
            {
                if (active_)
                    finish({}, 4);
                watchScreen(screen);
            });
}
void ApplicationController::start()
{
    if (options_.smokeTest)
    {
        startSmokeTest();
        return;
    }
    if (!QSystemTrayIcon::isSystemTrayAvailable())
    {
        qCritical("系统托盘不可用，应用无法驻留。请在正常桌面环境启动。");
        QTimer::singleShot(0, &application_, [this] { application_.exit(2); });
        return;
    }
    const auto registration = hotkey_->registerF1([this] { trigger(QStringLiteral("hotkey")); });
    tray_.setToolTip(QStringLiteral("WaibuSnap · %1").arg(registration.explanation));
    tray_.show();
    if (!registration.enabled)
        tray_.showMessage(QStringLiteral("快捷键未启用"), registration.explanation,
                          QSystemTrayIcon::Warning);
    if (options_.testMode)
    {
        // 仅受控模式有这些定时器；空闲采样必须走普通启动。
        QTimer::singleShot(options_.stableMs, this,
                           [this] { trigger(QStringLiteral("injected")); });
    }
}
void ApplicationController::trigger(const QString& source)
{
    const qint64 t0 = monotonicNs();
    if (active_ || quitting_)
        return;
    active_ = true;
    const quint64 token = ++token_;
    metrics_ = {};
    metrics_.sequence = ++sequence_;
    metrics_.trigger = source;
    metrics_.t0 = t0;
    const auto display = displays_->cursorDisplay();
    if (!display.error.isEmpty())
    {
        fail(5, display.error);
        return;
    }
    if (!capture_->hasPermission())
    {
        // 脚本不弹授权窗、不将权限时间混入性能。首次授权由用户从 F1 / 托盘执行。
        if (options_.testMode || !capture_->requestPermission())
        {
            fail(3, QStringLiteral("请在系统设置 → 隐私与安全性 → 屏幕与系统音频录制中允许 "
                                   "WaibuSnap。允许后重试；系统要求时重启。未授权不会生成截图。"));
            return;
        }
    }
    metrics_.captureStart = monotonicNs();
    QPointer<ApplicationController> self(this);
    capture_->capture(display.target,
                      [self, token](CaptureResult result) mutable
                      {
                          if (self)
                              self->captureCompleted(token, std::move(result));
                      });
    // 仅会话存活时存在超时保护；空闲不轮询。
    captureTimeout_.start(15000);
}
void ApplicationController::captureCompleted(quint64 token, CaptureResult result)
{
    if (!active_ || token != token_)
        return;
    metrics_.captureComplete = monotonicNs();
    if (result.error != CaptureError::None)
    {
        fail(7, result.explanation);
        return;
    }
    if (!displays_->stillMatches(result.frame.display))
    {
        fail(4, QStringLiteral("显示器布局已变化，本次截图已取消，请重试。"));
        return;
    }
    metrics_.frameWidth = result.frame.pixels.width();
    metrics_.frameHeight = result.frame.pixels.height();
    overlay_ = new SelectionOverlay(std::move(result.frame));
    overlay_->winId();
    metrics_.windowCreated = monotonicNs();
    connect(overlay_, &SelectionOverlay::finished, this,
            [this](QRect pixels, bool confirmed) { finish(pixels, confirmed ? 1 : 2); });
    connect(overlay_, &SelectionOverlay::firstPaintCompleted, this,
            [this, token](qint64 timestamp)
            {
                metrics_.paintComplete = timestamp;
                // 排到 backing store flush 之后，再观察目标屏刷新；不能用 paintEvent 当可见终点。
                QTimer::singleShot(
                    0, this,
                    [this, token]
                    {
                        if (!active_ || token != token_ || !overlay_)
                            return;
                        observation_ = observePresentation(
                            overlay_->windowHandle(), this,
                            [this, token](PresentationSample sample)
                            {
                                if (!active_ || token != token_ || !overlay_)
                                    return;
                                if (!sample.available || !overlay_->isVisible() ||
                                    !overlay_->isActiveWindow())
                                {
                                    fail(8,
                                         QStringLiteral(
                                             "未观察到可交互覆盖窗口，本次不计入有效性能样本。"));
                                    return;
                                }
                                captureTimeout_.stop();
                                metrics_.visibleProxy = sample.observedAtNs;
                                metrics_.interactive = monotonicNs();
                                metrics_.refreshIntervalMs = sample.refreshIntervalMs;
                                if (options_.testMode)
                                    QTimer::singleShot(options_.holdMs, this,
                                                       [this, token]
                                                       {
                                                           if (active_ && token == token_)
                                                               finish({}, 2);
                                                       });
                            });
                    });
            });
    overlay_->show();
    overlay_->raise();
    overlay_->activateWindow();
    overlay_->setFocus(Qt::ActiveWindowFocusReason);
}
void ApplicationController::finish(QRect pixels, int outcome)
{
    if (!active_)
        return;
    ++token_;
    captureTimeout_.stop();
    observation_.reset();
    metrics_.width = pixels.width();
    metrics_.height = pixels.height();
    metrics_.outcome = outcome;
    if (overlay_)
    {
        overlay_->disconnect(this);
        overlay_->hide();
        // 延迟销毁避开正在执行的窗口事件；销毁后不留冻结像素或隐藏窗口。
        overlay_->deleteLater();
        overlay_ = nullptr;
    }
    const bool written = appendMetrics(options_.metricsFile, metrics_);
    active_ = false;
    if (!written)
        qCritical("无法写入会话测量日志。");
    if (options_.testMode && !quitting_)
    {
        if (!written || !metrics_.interactive)
            QTimer::singleShot(0, &application_, [this] { application_.exit(3); });
        else if (sequence_ >= options_.testCount)
            QTimer::singleShot(0, this, &ApplicationController::quit);
        else
            QTimer::singleShot(options_.intervalMs, this,
                               [this] { trigger(QStringLiteral("injected")); });
    }
}
void ApplicationController::fail(int outcome, const QString& explanation)
{
    if (options_.testMode)
    {
        qWarning().noquote() << explanation;
        finish({}, outcome);
        return;
    }
    qWarning().noquote() << explanation;
    // 对话框期间仍持有会话锁，避免连续 F1 堆叠提示。
    QMessageBox::information(nullptr, QStringLiteral("截图未完成"), explanation);
    finish({}, outcome);
}
void ApplicationController::quit()
{
    quitting_ = true;
    if (active_)
        finish({}, 9);
    hotkey_.reset();
    tray_.hide();
    application_.quit();
}
void ApplicationController::startSmokeTest()
{
    // 无头环境无法显示系统托盘；仍构造真实菜单并验证窗口关闭不会退出事件循环。
    QTimer::singleShot(
        0, this,
        [this]
        {
            if (application_.quitOnLastWindowClosed() || menu_.actions().size() != 2 ||
                !QApplication::topLevelWidgets().contains(&menu_))
            {
                application_.exit(20);
                return;
            }
            auto* transient = new QWidget;
            transient->setAttribute(Qt::WA_DeleteOnClose);
            transient->show();
            QTimer::singleShot(50, this,
                               [this, transient]
                               {
                                   if (!transient->isVisible())
                                   {
                                       application_.exit(21);
                                       return;
                                   }
                                   transient->close();
                                   QTimer::singleShot(
                                       50, this,
                                       [this]
                                       {
                                           if (QApplication::topLevelWidgets().size() != 1)
                                           {
                                               application_.exit(22);
                                               return;
                                           }
                                           qInfo("托盘生命周期已验证");
                                           menu_.actions().last()->trigger();
                                       });
                               });
        });
}
}
