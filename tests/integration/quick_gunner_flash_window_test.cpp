#include "interfaces/region_capture.h"
#include "ui/quick_gunner_flash_window.h"
#include <QSignalSpy>
#include <QTest>
using namespace waibusnap;
class QuickGunnerFlashWindowTest : public QObject
{
    Q_OBJECT
  private slots:
    void flagsGeometryPixelsAndTimeout()
    {
        QuickGunnerFlashWindow window;
        QVERIFY(!window.isVisible());
        for (auto flag : {Qt::FramelessWindowHint, Qt::WindowStaysOnTopHint,
                          Qt::WindowTransparentForInput, Qt::WindowDoesNotAcceptFocus})
            QVERIFY(window.windowFlags().testFlag(flag));
        QVERIFY(window.testAttribute(Qt::WA_ShowWithoutActivating));
        QVERIFY(window.testAttribute(Qt::WA_MacAlwaysShowToolWindow));
        QVERIFY(window.testAttribute(Qt::WA_TranslucentBackground));
        window.flash({100, 120, 80, 60});
        QCOMPARE(window.geometry(), QRect(98, 118, 84, 64));
        QVERIFY(window.isVisible());
        QImage pixels(window.size(), QImage::Format_ARGB32_Premultiplied);
        pixels.fill(Qt::transparent);
        window.render(&pixels);
        QCOMPARE(pixels.pixelColor(0, 30), QColor(QStringLiteral("#f0a030")));
        QCOMPARE(pixels.pixelColor(1, 30), QColor(QStringLiteral("#f0a030")));
        QCOMPARE(pixels.pixelColor(2, 30).alpha(), 0);
        QCOMPARE(pixels.pixelColor(40, 30).alpha(), 0);
        QTest::qWait(60);
        QVERIFY(window.isVisible());
        window.flash({120, 140, 80, 60});
        QTest::qWait(70);
        QVERIFY(window.isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 200);
    }
};
QTEST_MAIN(QuickGunnerFlashWindowTest)
#include "quick_gunner_flash_window_test.moc"
