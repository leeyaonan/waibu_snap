#include "core/quick_gunner.h"
#include <QJsonDocument>
#include <QTest>
#include <limits>
using namespace waibusnap;
namespace
{
QuickGunnerRegion region(qreal dpr = 2)
{
    return {{42, {-400, 20, 400, 300}, dpr, 123, {-400, 40, 400, 280}}, {20, 30, 100, 80}};
}
}
class QuickGunnerTest final : public QObject
{
    Q_OBJECT
  private slots:
    void defaults()
    {
        QuickGunnerPreferences preferences;
        QVERIFY(preferences.directory.isEmpty());
        QVERIFY(!preferences.region);
        QVERIFY(!QuickGunnerRegion{}.isValid());
    }
    void roundTrip_data()
    {
        QTest::addColumn<qreal>("dpr");
        QTest::newRow("dpr-1") << qreal(1);
        QTest::newRow("dpr-2") << qreal(2);
    }
    void roundTrip()
    {
        QFETCH(qreal, dpr);
        const auto original = region(dpr);
        const auto json =
            QJsonDocument::fromJson(QJsonDocument(original.toJson()).toJson()).object();
        QString reason = QStringLiteral("旧原因");
        const auto restored = QuickGunnerRegion::fromJson(json, &reason);
        QVERIFY2(restored.has_value(), qPrintable(reason));
        QVERIFY(reason.isEmpty());
        QCOMPARE(restored->toJson(), original.toJson());
    }
    void missingFields_data()
    {
        QTest::addColumn<QString>("field");
        for (const auto* field : {"displayId", "logicalGeometry", "devicePixelRatio",
                                  "layoutVersion", "availableLogicalGeometry", "pixelRect"})
            QTest::newRow(field) << QString::fromLatin1(field);
    }
    void missingFields()
    {
        QFETCH(QString, field);
        auto json = region().toJson();
        json.remove(field);
        QString reason;
        QVERIFY(!QuickGunnerRegion::fromJson(json, &reason));
        QVERIFY(!reason.isEmpty());
        QVERIFY(reason.contains(field));
    }
    void invalidFields_data()
    {
        QTest::addColumn<QString>("field");
        QTest::addColumn<QJsonValue>("value");
        QTest::newRow("id-string") << QStringLiteral("displayId") << QJsonValue("42");
        QTest::newRow("id-negative") << QStringLiteral("displayId") << QJsonValue(-1);
        QTest::newRow("id-fraction") << QStringLiteral("displayId") << QJsonValue(1.5);
        QTest::newRow("id-overflow") << QStringLiteral("displayId") << QJsonValue(4294967296.0);
        QTest::newRow("dpr-zero") << QStringLiteral("devicePixelRatio") << QJsonValue(0);
        QTest::newRow("dpr-string") << QStringLiteral("devicePixelRatio") << QJsonValue("2");
        QTest::newRow("version-negative") << QStringLiteral("layoutVersion") << QJsonValue(-1);
        QTest::newRow("version-fraction") << QStringLiteral("layoutVersion") << QJsonValue(1.5);
        QTest::newRow("version-overflow")
            << QStringLiteral("layoutVersion") << QJsonValue("18446744073709551616");
        QTest::newRow("version-leading-zero")
            << QStringLiteral("layoutVersion") << QJsonValue("01");
        QTest::newRow("version-unsafe-number")
            << QStringLiteral("layoutVersion") << QJsonValue(9007199254740992.0);
        QTest::newRow("rect-string") << QStringLiteral("pixelRect") << QJsonValue("20,30,100,80");
        QTest::newRow("rect-incomplete")
            << QStringLiteral("pixelRect") << QJsonValue(QJsonObject{});
        auto rect = region().toJson().value("pixelRect").toObject();
        rect["width"] = 15;
        QTest::newRow("rect-small") << QStringLiteral("pixelRect") << QJsonValue(rect);
        rect["width"] = 100.5;
        QTest::newRow("rect-fraction") << QStringLiteral("pixelRect") << QJsonValue(rect);
        rect["width"] = 2147483648.0;
        QTest::newRow("rect-overflow") << QStringLiteral("pixelRect") << QJsonValue(rect);
        rect["width"] = 100;
        rect["x"] = -1;
        QTest::newRow("rect-negative") << QStringLiteral("pixelRect") << QJsonValue(rect);
        auto geometry = region().toJson().value("logicalGeometry").toObject();
        geometry["height"] = 0;
        QTest::newRow("display-empty") << QStringLiteral("logicalGeometry") << QJsonValue(geometry);
        auto available = region().toJson().value("availableLogicalGeometry").toObject();
        available["height"] = 0;
        QTest::newRow("available-partial-empty")
            << QStringLiteral("availableLogicalGeometry") << QJsonValue(available);
    }
    void invalidFields()
    {
        QFETCH(QString, field);
        QFETCH(QJsonValue, value);
        auto json = region().toJson();
        json[field] = value;
        QString reason;
        QVERIFY(!QuickGunnerRegion::fromJson(json, &reason));
        QVERIFY(!reason.isEmpty());
    }
    void bounds_data()
    {
        QTest::addColumn<qreal>("dpr");
        QTest::addColumn<QRect>("rect");
        QTest::addColumn<bool>("valid");
        QTest::newRow("minimum") << qreal(1) << QRect(384, 284, 16, 16) << true;
        QTest::newRow("retina-minimum") << qreal(2) << QRect(784, 584, 16, 16) << true;
        QTest::newRow("full") << qreal(2) << QRect(0, 0, 800, 600) << true;
        QTest::newRow("right") << qreal(2) << QRect(785, 584, 16, 16) << false;
        QTest::newRow("bottom") << qreal(1) << QRect(0, 285, 16, 16) << false;
        QTest::newRow("left") << qreal(2) << QRect(-1, 0, 16, 16) << false;
        QTest::newRow("top") << qreal(2) << QRect(0, -1, 16, 16) << false;
        QTest::newRow("narrow") << qreal(2) << QRect(0, 0, 15, 16) << false;
        QTest::newRow("short") << qreal(2) << QRect(0, 0, 16, 15) << false;
        QTest::newRow("empty") << qreal(1) << QRect() << false;
    }
    void bounds()
    {
        QFETCH(qreal, dpr);
        QFETCH(QRect, rect);
        QFETCH(bool, valid);
        auto value = region(dpr);
        value.pixelRect = rect;
        QCOMPARE(value.isValid(), valid);
    }
    void invalidDisplayAndLargeVersion()
    {
        auto value = region();
        value.display.devicePixelRatio = std::numeric_limits<qreal>::quiet_NaN();
        QVERIFY(!value.isValid());
        value.display.devicePixelRatio = std::numeric_limits<qreal>::infinity();
        QVERIFY(!value.isValid());
        value.display.devicePixelRatio = 1e20;
        QVERIFY(!value.isValid());
        value = region();
        value.display.layoutVersion = std::numeric_limits<quint64>::max();
        const auto json = QJsonDocument::fromJson(QJsonDocument(value.toJson()).toJson()).object();
        const auto restored = QuickGunnerRegion::fromJson(json);
        QVERIFY(restored);
        QCOMPARE(restored->display.layoutVersion, value.display.layoutVersion);
    }
};
QTEST_APPLESS_MAIN(QuickGunnerTest)
#include "quick_gunner_test.moc"
