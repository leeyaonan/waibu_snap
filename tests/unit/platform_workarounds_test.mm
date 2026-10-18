#include "interfaces/overlay_window_behavior.h"
#include "interfaces/platform_workarounds.h"
#include "interfaces/region_capture.h"
#include "interfaces/window_visibility.h"
#import <AppKit/AppKit.h>
#include <QGuiApplication>
#include <QWindow>
#include <QtTest>
#import <objc/runtime.h>

class PlatformWorkaroundsTest final : public QObject
{
    Q_OBJECT
  private slots:
    void overlayWindowLevelNullWindow()
    {
        waibusnap::lowerOverlayBelowSystemPanels(nullptr);
        waibusnap::raiseOverlayAboveSystemChrome(nullptr);
        QVERIFY(!waibusnap::reassertOverlayWindow(nullptr, true));
    }
    void overlayWindowLevelGuards()
    {
        if (QGuiApplication::screens().isEmpty())
        {
            QTest::qSkip("当前环境没有可用屏幕，无法创建 QWindow 验证无原生句柄守卫。", __FILE__,
                         __LINE__);
            return;
        }
        QWindow window;
        QVERIFY(!window.handle());
        waibusnap::lowerOverlayBelowSystemPanels(&window);
        waibusnap::raiseOverlayAboveSystemChrome(&window);
        // 安全返回不得为尚未显示的窗口创建原生句柄。
        QVERIFY(!window.handle());
        QVERIFY(!waibusnap::reassertOverlayWindow(&window, true));
        QVERIFY(!window.handle());
    }
    void overlayWindowLevelRoundTrip()
    {
        if (QGuiApplication::platformName() != QStringLiteral("cocoa"))
        {
            QTest::qSkip("当前不是 Cocoa 后端，无法核对 AppKit 窗口层级。", __FILE__, __LINE__);
            return;
        }
        if (QGuiApplication::screens().isEmpty())
        {
            QTest::qSkip("当前环境没有可用屏幕，无法核对 AppKit 窗口层级。", __FILE__, __LINE__);
            return;
        }
        QWindow window;
        window.create();
        if (!window.handle())
        {
            QTest::qSkip("当前环境无法创建原生窗口，无法核对 AppKit 窗口层级。", __FILE__,
                         __LINE__);
            return;
        }
        NSView* view = (__bridge NSView*)reinterpret_cast<void*>(window.winId());
        NSWindow* native = view.window;
        if (!native)
        {
            QTest::qSkip("当前环境没有 AppKit 窗口，无法核对原生层级。", __FILE__, __LINE__);
            return;
        }
        // 隐藏窗口只验证层级接口，不替代原生保存面板的真机可见性验收。
        waibusnap::raiseOverlayAboveSystemChrome(&window);
        QCOMPARE(native.level, NSInteger(25));
        waibusnap::lowerOverlayBelowSystemPanels(&window);
        QCOMPARE(native.level, NSInteger(0));
        waibusnap::raiseOverlayAboveSystemChrome(&window);
        QCOMPARE(native.level, NSInteger(25));
    }
    void nativeVisibilityAndRecovery()
    {
        using namespace waibusnap;
        QVERIFY(!queryWindowVisibility(nullptr).available);
        QWindow window;
        QVERIFY(!queryWindowVisibility(&window).available);
        QVERIFY(!window.handle());
        if (QGuiApplication::platformName() != QStringLiteral("cocoa") ||
            QGuiApplication::screens().isEmpty())
            QSKIP("当前无 Cocoa 屏幕，原生在屏验证需真实 WindowServer。");
        window.setFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        window.setGeometry(100, 100, 240, 160);
        window.show();
        NSView* view = (__bridge NSView*)reinterpret_cast<void*>(window.winId());
        NSWindow* native = view.window;
        QVERIFY(native);
        native.hidesOnDeactivate = NO;
        QTRY_VERIFY(queryWindowVisibility(&window).onScreen);
        const auto before = queryWindowVisibility(&window);
        QCOMPARE(before.windowNumber, quint64(native.windowNumber));
        QVERIFY(before.available);
        QVERIFY(before.isOnActiveSpace >= 0);
        QVERIFY(before.occlusionState >= 0);
        // 真实原生 orderOut 不改变 Qt 可见状态，复刻本次缺失的检测条件。
        [native orderOut:nil];
        QVERIFY(window.isVisible());
        QTRY_VERIFY(!queryWindowVisibility(&window).onScreen);
        qInfo().noquote() << "原生在屏故障注入：" << queryWindowVisibility(&window).diagnostic();
        QVERIFY(reassertOverlayWindow(&window, false));
        QTRY_VERIFY(queryWindowVisibility(&window).onScreen);
        QCOMPARE(native.level, NSInteger(25));
        qInfo().noquote() << "原生在屏恢复：" << queryWindowVisibility(&window).diagnostic();
        lowerOverlayBelowSystemPanels(&window);
        [native orderOut:nil];
        QVERIFY(!reassertOverlayWindow(&window, true));
        QCOMPARE(native.level, NSInteger(0));
        QTRY_VERIFY(!queryWindowVisibility(&window).onScreen);
        raiseOverlayAboveSystemChrome(&window);
        QVERIFY(reassertOverlayWindow(&window, false));
        QTRY_VERIFY(queryWindowVisibility(&window).onScreen);
        // 只评估位掩码合法性，不把同一 Space 在屏当作多 Space / 全屏验证。
        const auto original = native.collectionBehavior;
        native.collectionBehavior = (original & ~(NSWindowCollectionBehaviorMoveToActiveSpace |
                                                  NSWindowCollectionBehaviorFullScreenPrimary |
                                                  NSWindowCollectionBehaviorFullScreenNone)) |
                                    NSWindowCollectionBehaviorCanJoinAllSpaces |
                                    NSWindowCollectionBehaviorFullScreenAuxiliary;
        qInfo().noquote() << "候选 collectionBehavior（仅当前 Space）："
                          << queryWindowVisibility(&window).diagnostic();
        QVERIFY(queryWindowVisibility(&window).onScreen);
        native.collectionBehavior = original;
    }
    void overlayNotificationsAndLifetime()
    {
        using namespace waibusnap;
        if (QGuiApplication::platformName() != QStringLiteral("cocoa") ||
            QGuiApplication::screens().isEmpty())
            QSKIP("当前无 Cocoa 屏幕，原生在屏验证需真实 WindowServer。");
        QWindow window;
        window.create();
        QObject context;
        int callbacks = 0;
        auto observer =
            observeOverlayWindow(&window, &context, [&](const QString&) { ++callbacks; });
        QVERIFY(observer);
        NSView* view = (__bridge NSView*)reinterpret_cast<void*>(window.winId());
        const auto post = [&]
        {
            [NSWorkspace.sharedWorkspace.notificationCenter
                postNotificationName:NSWorkspaceActiveSpaceDidChangeNotification
                              object:nil];
            [NSWorkspace.sharedWorkspace.notificationCenter
                postNotificationName:NSWorkspaceDidWakeNotification
                              object:nil];
            [NSNotificationCenter.defaultCenter
                postNotificationName:NSApplicationDidChangeScreenParametersNotification
                              object:nil];
            [NSNotificationCenter.defaultCenter
                postNotificationName:NSWindowDidChangeOcclusionStateNotification
                              object:view.window];
        };
        [NSWorkspace.sharedWorkspace.notificationCenter
            postNotificationName:NSWorkspaceActiveSpaceDidChangeNotification
                          object:nil];
        QTRY_COMPARE(callbacks, 1);
        [NSWorkspace.sharedWorkspace.notificationCenter
            postNotificationName:NSWorkspaceDidWakeNotification
                          object:nil];
        QTRY_COMPARE(callbacks, 2);
        [NSNotificationCenter.defaultCenter
            postNotificationName:NSApplicationDidChangeScreenParametersNotification
                          object:nil];
        QTRY_COMPARE(callbacks, 3);
        [NSNotificationCenter.defaultCenter
            postNotificationName:NSWindowDidChangeOcclusionStateNotification
                          object:view.window];
        QTRY_COMPARE(callbacks, 4);
        post();
        QCOMPARE(callbacks, 4);
        QTRY_COMPARE(callbacks, 5);
        post();
        observer.reset();
        QTest::qWait(150);
        QCOMPARE(callbacks, 5);
        post();
        QTest::qWait(150);
        QCOMPARE(callbacks, 5);
    }
    void regionWindowIdGuardsAndNativeNumber()
    {
        QCOMPARE(waibusnap::regionCaptureWindowId(nullptr), quint64(0));
        QWindow window;
        QVERIFY(!window.handle());
        QCOMPARE(waibusnap::regionCaptureWindowId(&window), quint64(0));
        QVERIFY(!window.handle());
        if (QGuiApplication::platformName() != QStringLiteral("cocoa") ||
            QGuiApplication::screens().isEmpty())
            return;
        window.create();
        NSView* view = (__bridge NSView*)reinterpret_cast<void*>(window.winId());
        QVERIFY(view.window);
        QCOMPARE(waibusnap::regionCaptureWindowId(&window), quint64(view.window.windowNumber));
        QVERIFY(waibusnap::regionCaptureWindowId(&window) > 0);
    }
    void safeClickCount()
    {
        @autoreleasepool
        {
            NSEvent* event = [NSEvent otherEventWithType:NSEventTypeAppKitDefined
                                                location:NSZeroPoint
                                           modifierFlags:0
                                               timestamp:0
                                            windowNumber:0
                                                 context:nil
                                                 subtype:0
                                                   data1:0
                                                   data2:0];
            QVERIFY(event);
            bool threw = false;
            @try
            {
                (void)event.clickCount;
            }
            @catch (NSException* exception)
            {
                threw = true;
                QCOMPARE(QString::fromNSString(exception.name),
                         QStringLiteral("NSInternalInconsistencyException"));
            }
            // 先证明真实 AppKit 会拒绝非鼠标事件，避免绕行测试虚假通过。
            QVERIFY(threw);

            waibusnap::installPlatformCompatibilityWorkarounds();
            @try
            {
                QCOMPARE(event.clickCount, NSInteger(1));
                NSEvent* mouse = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown
                                                    location:NSZeroPoint
                                               modifierFlags:0
                                                   timestamp:0
                                                windowNumber:0
                                                     context:nil
                                                 eventNumber:1
                                                  clickCount:2
                                                    pressure:1];
                QVERIFY(mouse);
                QCOMPARE(mouse.clickCount, NSInteger(2));

                const Method method =
                    class_getInstanceMethod([NSEvent class], @selector(clickCount));
                const IMP installed = method_getImplementation(method);
                waibusnap::installPlatformCompatibilityWorkarounds();
                waibusnap::installPlatformCompatibilityWorkarounds();
                QVERIFY(method_getImplementation(method) == installed);
                QCOMPARE(event.clickCount, NSInteger(1));
                QCOMPARE(mouse.clickCount, NSInteger(2));
            }
            @catch (NSException* exception)
            {
                QFAIL(qPrintable(QString::fromNSString(exception.reason)));
            }
        }
    }
};
int main(int argc, char* argv[])
{
    QGuiApplication app(argc, argv);
    PlatformWorkaroundsTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "platform_workarounds_test.moc"
