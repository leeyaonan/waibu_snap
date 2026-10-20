#include "app/hotkey_rules.h"
#include "interfaces/global_hotkey.h"
#include <QTest>
using namespace waibusnap;
class WindowsHotkeyPendingTest final : public QObject
{
    Q_OBJECT
  private slots:
    void everyActionExplicitlyPending()
    {
        auto hotkey = createGlobalHotkey();
        int callbacks = 0;
        for (const auto action : hotkeyActions)
        {
            const auto result =
                hotkey->registerHotkey(action, defaultHotkey(action), [&] { ++callbacks; });
            QVERIFY(!result.enabled);
            QVERIFY(result.explanation.contains(QStringLiteral("Windows 多动作全局热键待批次 7b")));
            hotkey->unregister(action);
        }
        hotkey->unregister();
        QCOMPARE(callbacks, 0);
    }
};
QTEST_GUILESS_MAIN(WindowsHotkeyPendingTest)
#include "windows_hotkey_pending_test.moc"
