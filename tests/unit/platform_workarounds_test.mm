#include "interfaces/platform_workarounds.h"
#import <AppKit/AppKit.h>
#include <QtTest>
#import <objc/runtime.h>

class PlatformWorkaroundsTest final : public QObject
{
    Q_OBJECT
  private slots:
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
    PlatformWorkaroundsTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "platform_workarounds_test.moc"
