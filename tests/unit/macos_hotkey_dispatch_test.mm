#include "platform/macos/hotkey_dispatch.h"
#include <QTest>
using namespace waibusnap;
class MacHotkeyDispatchTest final : public QObject
{
    Q_OBJECT
  private slots:
    void fiveActionsAndStaleEvents()
    {
        std::array<MacHotkeyCallback, 5> bindings;
        std::array<int, 5> calls{};
        for (size_t i = 0; i < bindings.size(); ++i)
            bindings[i] = {UInt32(i + 1), [&, i] { ++calls[i]; }};
        const auto dispatch = [&](UInt32 signature, UInt32 id)
        {
            EventRef event = nullptr;
            if (CreateEvent(nullptr, kEventClassKeyboard, kEventHotKeyPressed, 0,
                            kEventAttributeNone, &event) != noErr)
                qFatal("无法创建 Carbon 测试事件。");
            EventHotKeyID key = {signature, id};
            if (SetEventParameter(event, kEventParamDirectObject, typeEventHotKeyID, sizeof(key),
                                  &key) != noErr)
                qFatal("无法写入 Carbon 测试事件。");
            const auto result = dispatchMacHotkeyEvent(event, bindings);
            ReleaseEvent(event);
            return result;
        };
        for (UInt32 i = 1; i <= bindings.size(); ++i)
            QCOMPARE(dispatch(0x5742534e, i), noErr);
        for (const auto count : calls)
            QCOMPARE(count, 1);
        QCOMPARE(dispatch(0, 1), eventNotHandledErr);
        bindings[1].id = 6;
        QCOMPARE(dispatch(0x5742534e, 2), eventNotHandledErr);
        QCOMPARE(dispatch(0x5742534e, 6), noErr);
        QCOMPARE(calls[1], 2);
        bindings[2] = {};
        QCOMPARE(dispatch(0x5742534e, 3), eventNotHandledErr);
        QCOMPARE(dispatch(0x5742534e, 1), noErr);
        QCOMPARE(calls[0], 2);
    }
    void malformedEventAndSelfRelease()
    {
        EventRef event = nullptr;
        QCOMPARE(CreateEvent(nullptr, kEventClassKeyboard, kEventHotKeyPressed, 0,
                             kEventAttributeNone, &event),
                 noErr);
        std::array<MacHotkeyCallback, 5> bindings;
        QCOMPARE(dispatchMacHotkeyEvent(event, bindings), eventNotHandledErr);
        EventHotKeyID key = {0x5742534e, 5};
        QCOMPARE(
            SetEventParameter(event, kEventParamDirectObject, typeEventHotKeyID, sizeof(key), &key),
            noErr);
        bool completed = false;
        bindings[4] = {5, [&]
                       {
                           bindings = {};
                           completed = true;
                       }};
        QCOMPARE(dispatchMacHotkeyEvent(event, bindings), noErr);
        QVERIFY(completed);
        QCOMPARE(dispatchMacHotkeyEvent(event, bindings), eventNotHandledErr);
        ReleaseEvent(event);
    }
};
QTEST_GUILESS_MAIN(MacHotkeyDispatchTest)
#include "macos_hotkey_dispatch_test.moc"
