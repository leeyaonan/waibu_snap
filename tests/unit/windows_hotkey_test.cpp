#include "app/hotkey_rules.h"
#include "platform/windows/global_hotkey.h"
#include <QAbstractEventDispatcher>
#include <QMap>
#include <QTest>
using namespace waibusnap;
namespace
{
struct FakeApi
{
    QMap<int, WindowsHotkeyChord> active;
    QVector<int> registered, released;
    DWORD error = ERROR_SUCCESS;
    bool failWithoutError = false;
    std::function<void()> beforeRegistration;
    WindowsHotkeyApi api()
    {
        return {[this](int id, UINT modifiers, UINT key) -> std::optional<DWORD>
                {
                    registered.append(id);
                    if (beforeRegistration)
                        beforeRegistration();
                    if (error != ERROR_SUCCESS || failWithoutError)
                        return error;
                    active.insert(id, {key, modifiers});
                    return {};
                },
                [this](int id)
                {
                    released.append(id);
                    active.remove(id);
                }};
    }
    MSG message(int id) const
    {
        const auto chord = active.value(id);
        MSG value{};
        value.message = WM_HOTKEY;
        value.wParam = WPARAM(id);
        value.lParam = MAKELPARAM(chord.modifiers & ~MOD_NOREPEAT, chord.key);
        return value;
    }
};
bool dispatch(WindowsHotkey& hotkey, MSG message)
{
    qintptr result = -1;
    return hotkey.nativeEventFilter("windows_dispatcher_MSG", &message, &result) && result == 0;
}
}
class WindowsHotkeyTest final : public QObject
{
    Q_OBJECT
  private slots:
    void mappingCoversLegalRules()
    {
        // 穷举共享规则允许的四种修饰键组合，防止平台只支持默认 F 键。
        const std::array<Qt::KeyboardModifier, 4> flags = {Qt::ControlModifier, Qt::AltModifier,
                                                           Qt::ShiftModifier, Qt::MetaModifier};
        const std::array<UINT, 4> native = {MOD_CONTROL, MOD_ALT, MOD_SHIFT, MOD_WIN};
        for (unsigned bits = 0; bits < 16; ++bits)
        {
            Qt::KeyboardModifiers modifiers;
            UINT expected = 0;
            for (size_t i = 0; i < flags.size(); ++i)
                if (bits & (1u << i))
                {
                    modifiers |= flags[i];
                    expected |= native[i];
                }
            const auto verify = [&](Qt::Key key, UINT vk)
            {
                const QKeySequence sequence(QKeyCombination(modifiers, key));
                const bool legal = hotkeyValidationError(sequence).isEmpty();
                const auto mapped = windowsHotkeyMapping(sequence);
                QCOMPARE(mapped.has_value(), legal);
                if (mapped)
                {
                    QCOMPARE(mapped->key, vk);
                    QCOMPARE(mapped->modifiers, expected);
                }
            };
            for (int key = Qt::Key_F1; key <= Qt::Key_F12; ++key)
                verify(Qt::Key(key), VK_F1 + UINT(key - Qt::Key_F1));
            for (int key = Qt::Key_A; key <= Qt::Key_Z; ++key)
                verify(Qt::Key(key), UINT(key));
            for (int key = Qt::Key_0; key <= Qt::Key_9; ++key)
                verify(Qt::Key(key), UINT(key));
        }
    }
    void invalidUnsupportedAndDuplicate()
    {
        FakeApi fake;
        WindowsHotkey hotkey(fake.api());
        for (const auto& sequence :
             {QKeySequence(), QKeySequence("Ctrl+A, Ctrl+B"), QKeySequence(Qt::Key_F13),
              QKeySequence(Qt::CTRL | Qt::Key_Space), QKeySequence(Qt::Key_Tab),
              QKeySequence(Qt::Key_Escape), QKeySequence(Qt::Key_A),
              QKeySequence(Qt::SHIFT | Qt::Key_3), QKeySequence(Qt::Key_unknown),
              QKeySequence(Qt::KeypadModifier | Qt::Key_F1),
              QKeySequence(Qt::GroupSwitchModifier | Qt::Key_A)})
        {
            QVERIFY(!windowsHotkeyMapping(sequence));
            QVERIFY(!hotkey.registerHotkey(HotkeyAction::Screenshot, sequence, [] {}).enabled);
        }
        QVERIFY(!hotkey.registerHotkey(HotkeyAction(-1), QKeySequence(Qt::Key_F1), [] {}).enabled);
        hotkey.unregister(HotkeyAction(-1));
        QVERIFY(fake.registered.isEmpty());
        QVERIFY(hotkey.registerHotkey(HotkeyAction::Screenshot, QKeySequence(Qt::Key_F1), [] {})
                    .enabled);
        const auto duplicate =
            hotkey.registerHotkey(HotkeyAction::HistoryNext, QKeySequence(Qt::Key_F1), [] {});
        QVERIFY(!duplicate.enabled);
        QVERIFY(duplicate.explanation.contains(QStringLiteral("重复")));
        QCOMPARE(fake.registered.size(), 1);
        QCOMPARE(fake.active.size(), 1);
    }
    void actionIdsIndependentReleaseAndDestruction()
    {
        FakeApi fake;
        std::array<int, hotkeyActions.size()> calls{};
        {
            WindowsHotkey hotkey(fake.api());
            std::array<int, hotkeyActions.size()> ids{};
            for (size_t i = 0; i < hotkeyActions.size(); ++i)
            {
                QVERIFY(hotkey
                            .registerHotkey(hotkeyActions[i], defaultHotkey(hotkeyActions[i]),
                                            [&, i] { ++calls[i]; })
                            .enabled);
                ids[i] = fake.registered.last();
                QCOMPARE((ids[i] - 0x5700) % int(hotkeyActions.size()), int(i));
                QVERIFY(fake.active[ids[i]].modifiers & MOD_NOREPEAT);
                QVERIFY(dispatch(hotkey, fake.message(ids[i])));
                QCOMPARE(calls[i], 1);
            }
            const auto removed = fake.message(ids[1]);
            hotkey.unregister(HotkeyAction::HistoryPrevious);
            hotkey.unregister(HotkeyAction::HistoryPrevious);
            QVERIFY(!dispatch(hotkey, removed));
            QCOMPARE(fake.released, QVector<int>{ids[1]});
            for (size_t i = 0; i < hotkeyActions.size(); ++i)
            {
                if (i == 1)
                    continue;
                QVERIFY(dispatch(hotkey, fake.message(ids[i])));
                QCOMPARE(calls[i], 2);
            }
        }
        QVERIFY(fake.active.isEmpty());
        QCOMPARE(fake.released.size(), qsizetype(hotkeyActions.size()));
    }
    void replaceRollbackAndStaleEvents()
    {
        FakeApi fake;
        WindowsHotkey hotkey(fake.api());
        int oldCalls = 0, newCalls = 0;
        QVERIFY(hotkey
                    .registerHotkey(HotkeyAction::Screenshot, QKeySequence(Qt::Key_F1),
                                    [&] { ++oldCalls; })
                    .enabled);
        const auto old = fake.message(fake.registered.last());
        fake.error = ERROR_HOTKEY_ALREADY_REGISTERED;
        const auto failed = hotkey.registerHotkey(HotkeyAction::Screenshot,
                                                  QKeySequence(Qt::Key_F2), [&] { ++newCalls; });
        QVERIFY(!failed.enabled);
        QVERIFY(failed.explanation.contains(QStringLiteral("占用")));
        QVERIFY(failed.explanation.contains(QStringLiteral("原绑定保留")));
        QVERIFY(fake.released.isEmpty());
        QVERIFY(dispatch(hotkey, old));
        QCOMPARE(oldCalls, 1);
        QCOMPARE(newCalls, 0);
        fake.error = ERROR_SUCCESS;
        // 同键只更新回调，无需重复注册或释放系统组合。
        const auto attempts = fake.registered.size();
        QVERIFY(hotkey
                    .registerHotkey(HotkeyAction::Screenshot, QKeySequence(Qt::Key_F1),
                                    [&] { ++newCalls; })
                    .enabled);
        QCOMPARE(fake.registered.size(), attempts);
        QVERIFY(dispatch(hotkey, old));
        QCOMPARE(newCalls, 1);
        bool oldStillRegistered = false;
        fake.beforeRegistration = [&]
        { oldStillRegistered = fake.active.contains(int(old.wParam)); };
        QVERIFY(hotkey
                    .registerHotkey(HotkeyAction::Screenshot, QKeySequence(Qt::Key_F2),
                                    [&] { ++newCalls; })
                    .enabled);
        QVERIFY(oldStillRegistered);
        fake.beforeRegistration = {};
        QCOMPARE(fake.active.size(), 1);
        QCOMPARE(fake.released, QVector<int>{int(old.wParam)});
        QVERIFY(!dispatch(hotkey, old));
        const auto replaced = fake.message(fake.registered.last());
        QVERIFY(dispatch(hotkey, replaced));
        // 换回原组合也不复用 token，拒绝更早排队的同键事件。
        QVERIFY(hotkey
                    .registerHotkey(HotkeyAction::Screenshot, QKeySequence(Qt::Key_F1),
                                    [&] { ++newCalls; })
                    .enabled);
        QVERIFY(!dispatch(hotkey, old));
        QVERIFY(!dispatch(hotkey, replaced));
        QVERIFY(dispatch(hotkey, fake.message(fake.registered.last())));
        QCOMPARE(newCalls, 3);
        hotkey.unregister();
        QVERIFY(fake.active.isEmpty());
    }
    void singleFailureKeepsOtherActions()
    {
        FakeApi fake;
        WindowsHotkey hotkey(fake.api());
        std::array<int, hotkeyActions.size()> calls{};
        std::array<MSG, hotkeyActions.size()> messages{};
        for (size_t i = 0; i < hotkeyActions.size(); ++i)
        {
            QVERIFY(hotkey
                        .registerHotkey(hotkeyActions[i], defaultHotkey(hotkeyActions[i]),
                                        [&, i] { ++calls[i]; })
                        .enabled);
            messages[i] = fake.message(fake.registered.last());
        }
        fake.error = ERROR_HOTKEY_ALREADY_REGISTERED;
        QVERIFY(!hotkey
                     .registerHotkey(HotkeyAction::HistoryPrevious, QKeySequence(Qt::Key_F10),
                                     [] { QFAIL("失败的新处理器不应生效。"); })
                     .enabled);
        QCOMPARE(fake.active.size(), qsizetype(hotkeyActions.size()));
        QVERIFY(fake.released.isEmpty());
        for (const auto& message : messages)
            QVERIFY(dispatch(hotkey, message));
        for (const auto count : calls)
            QCOMPARE(count, 1);
        fake.error = ERROR_SUCCESS;
        QVERIFY(hotkey
                    .registerHotkey(HotkeyAction::HistoryNext, QKeySequence(Qt::Key_F11),
                                    [&] { ++calls[2]; })
                    .enabled);
        QVERIFY(dispatch(hotkey, messages[1]));
        QCOMPARE(calls[1], 2);
        QCOMPARE(fake.active.size(), qsizetype(hotkeyActions.size()));
    }
    void errorReasons_data()
    {
        QTest::addColumn<quint32>("code");
        QTest::addColumn<QString>("reason");
        QTest::addColumn<int>("key");
        QTest::newRow("occupied") << quint32(ERROR_HOTKEY_ALREADY_REGISTERED)
                                  << QStringLiteral("占用") << int(Qt::Key_F2);
        QTest::newRow("invalid") << quint32(ERROR_INVALID_FLAGS) << QStringLiteral("不支持")
                                 << int(Qt::Key_F2);
        QTest::newRow("parameter")
            << quint32(ERROR_INVALID_PARAMETER) << QStringLiteral("不支持") << int(Qt::Key_F2);
        QTest::newRow("denied") << quint32(ERROR_ACCESS_DENIED) << QStringLiteral("拒绝")
                                << int(Qt::Key_F2);
        QTest::newRow("unknown") << quint32(0xffff) << QStringLiteral("注册热键失败")
                                 << int(Qt::Key_F2);
        QTest::newRow("reserved-f12")
            << quint32(ERROR_INVALID_FLAGS) << QStringLiteral("调试器保留") << int(Qt::Key_F12);
    }
    void errorReasons()
    {
        QFETCH(quint32, code);
        QFETCH(QString, reason);
        QFETCH(int, key);
        FakeApi fake;
        fake.error = code;
        WindowsHotkey hotkey(fake.api());
        const auto failed =
            hotkey.registerHotkey(HotkeyAction::HistoryOpen, QKeySequence(key), [] {});
        QVERIFY(!failed.enabled);
        QVERIFY(failed.explanation.contains(reason));
        QVERIFY(failed.explanation.contains(QString::number(code)));
        fake.error = ERROR_SUCCESS;
        QVERIFY(hotkey.registerHotkey(HotkeyAction::HistoryNext, QKeySequence(Qt::Key_F8), [] {})
                    .enabled);
        QCOMPARE(fake.active.size(), 1);
    }
    void failedApiWithZeroLastError()
    {
        FakeApi fake;
        WindowsHotkey hotkey(fake.api());
        int calls = 0;
        QVERIFY(hotkey
                    .registerHotkey(HotkeyAction::Screenshot, QKeySequence(Qt::Key_F1),
                                    [&] { ++calls; })
                    .enabled);
        const auto original = fake.message(fake.registered.last());
        fake.failWithoutError = true;
        const auto failed =
            hotkey.registerHotkey(HotkeyAction::Screenshot, QKeySequence(Qt::Key_F2), [] {});
        QVERIFY(!failed.enabled);
        QVERIFY(failed.explanation.contains(QStringLiteral("系统错误码 0")));
        QVERIFY(failed.explanation.contains(QStringLiteral("原绑定保留")));
        QVERIFY(fake.released.isEmpty());
        QVERIFY(dispatch(hotkey, original));
        QCOMPARE(calls, 1);
    }
    void nativeFilterAndSelfRelease()
    {
        FakeApi fake;
        WindowsHotkey hotkey(fake.api());
        int calls = 0;
        QVERIFY(hotkey
                    .registerHotkey(HotkeyAction::HistoryOpen, QKeySequence(Qt::CTRL | Qt::Key_7),
                                    [&]
                                    {
                                        ++calls;
                                        hotkey.unregister();
                                    })
                    .enabled);
        const auto message = fake.message(fake.registered.last());
        auto invalid = message;
        invalid.message = WM_KEYDOWN;
        QVERIFY(!dispatch(hotkey, invalid));
        invalid = message;
        invalid.wParam = 1;
        QVERIFY(!dispatch(hotkey, invalid));
        invalid = message;
        invalid.lParam = MAKELPARAM(MOD_ALT, '7');
        QVERIFY(!dispatch(hotkey, invalid));
        invalid = message;
        invalid.lParam = MAKELPARAM(MOD_CONTROL, '8');
        QVERIFY(!dispatch(hotkey, invalid));
        invalid = message;
        invalid.hwnd = reinterpret_cast<HWND>(quintptr(1));
        QVERIFY(!dispatch(hotkey, invalid));
        invalid = message;
        QVERIFY(!hotkey.nativeEventFilter("unrelated", &invalid, nullptr));
        QVERIFY(!hotkey.nativeEventFilter("windows_dispatcher_MSG", nullptr, nullptr));
        QCOMPARE(calls, 0);
        // 走 Qt 已安装的原生过滤器链，仍只构造 MSG，不注册真实系统热键。
        qintptr result = -1;
        QVERIFY(QAbstractEventDispatcher::instance()->filterNativeEvent("windows_dispatcher_MSG",
                                                                        &invalid, &result));
        QCOMPARE(result, qintptr(0));
        QCOMPARE(calls, 1);
        QVERIFY(fake.active.isEmpty());
        QVERIFY(!dispatch(hotkey, message));
    }
};
QTEST_GUILESS_MAIN(WindowsHotkeyTest)
#include "windows_hotkey_test.moc"
