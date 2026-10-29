#include "interfaces/recording.h"
#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#include <QSet>
#include <QtTest>
#include <limits>
using namespace waibusnap;
class RecordingWindowsTest : public QObject
{
    Q_OBJECT
  private slots:
    void enumerationFiltersAndIdentity()
    {
        const auto windows = recordingWindows();
        QSet<quint64> ids;
        for (const auto& window : windows)
        {
            QVERIFY(window.id > 0);
            QVERIFY(!ids.contains(window.id));
            ids.insert(window.id);
            QVERIFY(window.onScreen);
            QVERIFY(!window.label.trimmed().isEmpty());
            QVERIFY(window.label.size() <= 160);
            QVERIFY(window.logicalFrame.width() >= 32);
            QVERIFY(window.logicalFrame.height() >= 32);
            QVERIFY(qAbs(window.logicalFrame.x()) < 100000);
            QVERIFY(qAbs(window.logicalFrame.y()) < 100000);
            NSArray* raw = CFBridgingRelease(CGWindowListCopyWindowInfo(
                kCGWindowListOptionIncludingWindow, CGWindowID(window.id)));
            if (raw.count)
            {
                NSDictionary* item = raw.firstObject;
                QVERIFY([item[(__bridge NSString*)kCGWindowOwnerPID] intValue] !=
                        NSProcessInfo.processInfo.processIdentifier);
                QCOMPARE([item[(__bridge NSString*)kCGWindowLayer] intValue], 0);
                QVERIFY([item[(__bridge NSString*)kCGWindowAlpha] doubleValue] > 0);
            }
        }
        // 无可见窗口的 CI 仍执行无效 ID 与自有窗口排除断言。
        QVERIFY(!recordingWindowInfo(0));
        QVERIFY(!recordingWindowInfo(std::numeric_limits<quint64>::max()));
    }
    void ownWindowsAreExcludedButQueryable()
    {
        NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(40, 50, 180, 120)
                                                       styleMask:NSWindowStyleMaskBorderless
                                                         backing:NSBackingStoreBuffered
                                                           defer:NO];
        window.releasedWhenClosed = NO;
        [window orderFrontRegardless];
        const auto id = quint64(window.windowNumber);
        QVERIFY(id > 0);
        for (const auto& candidate : recordingWindows())
            QVERIFY(candidate.id != id);
        const auto info = recordingWindowInfo(id);
        // WindowServer 不可用时信息可为空，但枚举必须继续排除本进程。
        if (info)
        {
            QCOMPARE(info->id, id);
            QCOMPARE(info->logicalFrame.size(), QSize(180, 120));
            QVERIFY(!info->label.isEmpty());
        }
        [window orderOut:nil];
        [window close];
    }
    void observerStopDropsQueuedCallback()
    {
        auto observer = createRecordingEnvironmentObserver();
        QVERIFY(observer);
        int calls = 0;
        observer->start([&calls] { ++calls; });
        [NSWorkspace.sharedWorkspace.notificationCenter
            postNotificationName:NSWorkspaceWillSleepNotification
                          object:NSWorkspace.sharedWorkspace];
        observer->stop();
        QCoreApplication::processEvents();
        QCOMPARE(calls, 0);
        observer->start([&calls] { ++calls; });
        [NSWorkspace.sharedWorkspace.notificationCenter
            postNotificationName:NSWorkspaceSessionDidResignActiveNotification
                          object:NSWorkspace.sharedWorkspace];
        QTRY_COMPARE(calls, 1);
        observer->stop();
    }
};
QTEST_MAIN(RecordingWindowsTest)
#include "recording_windows_test.moc"
