#include "interfaces/sticker_window_behavior.h"
#import <AppKit/AppKit.h>
#include <QGuiApplication>
#include <QWindow>
#include <QtTest>
#import <objc/message.h>

class StickerWindowBehaviorTest final : public QObject
{
    Q_OBJECT
  private slots:
    void nonactivatingPanelSyncsWindowServerTag()
    {
        if (QGuiApplication::platformName() != QStringLiteral("cocoa"))
        {
            QTest::qSkip("当前不是 Cocoa 后端，无法核对 NSPanel 非激活 tag。", __FILE__, __LINE__);
            return;
        }
        if (QGuiApplication::screens().isEmpty())
        {
            QTest::qSkip("当前环境没有可用屏幕，无法创建贴图 NSPanel。", __FILE__, __LINE__);
            return;
        }
        QWindow window;
        window.setFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
        window.create();
        if (!window.handle())
        {
            QTest::qSkip("当前环境无法创建原生窗口，无法核对贴图 NSPanel。", __FILE__, __LINE__);
            return;
        }
        QVERIFY(waibusnap::configureStickerWindowBehavior(&window, false));
        NSView* view = (__bridge NSView*)reinterpret_cast<void*>(window.winId());
        NSWindow* native = view.window;
        QVERIFY(native);
        QVERIFY([native isKindOfClass:[NSPanel class]]);
        QVERIFY(native.styleMask & NSWindowStyleMaskNonactivatingPanel);

        SEL getter = @selector(_preventsActivation);
        if (![native respondsToSelector:getter])
        {
            QTest::qSkip("当前 AppKit 没有 _preventsActivation，无法核对 WindowServer tag。",
                         __FILE__, __LINE__);
            return;
        }
        using Getter = BOOL (*)(id, SEL);
        const BOOL prevents = reinterpret_cast<Getter>(objc_msgSend)(native, getter);
        QVERIFY2(prevents, "贴图 NSPanel 运行中补 nonactivating mask 时，必须同步 WindowServer "
                           "prevents-activation tag，否则点击贴图会激活应用。");
        window.destroy();
    }
    void keyboardEntryMakesPanelKey()
    {
        if (QGuiApplication::platformName() != QStringLiteral("cocoa"))
        {
            QTest::qSkip("当前不是 Cocoa 后端，无法核对 NSPanel key 状态。", __FILE__, __LINE__);
            return;
        }
        if (QGuiApplication::screens().isEmpty())
        {
            QTest::qSkip("当前环境没有可用屏幕，无法创建贴图 NSPanel。", __FILE__, __LINE__);
            return;
        }
        QWindow window;
        window.setFlags(Qt::Tool | Qt::FramelessWindowHint);
        window.resize(160, 100);
        window.show();
        QTest::qWait(10);
        QVERIFY(waibusnap::configureStickerWindowBehavior(&window, false));
        QVERIFY(waibusnap::makeStickerWindowKeyForKeyboard(&window));
        window.destroy();
    }
};
QTEST_MAIN(StickerWindowBehaviorTest)
#include "sticker_window_behavior_test.moc"
