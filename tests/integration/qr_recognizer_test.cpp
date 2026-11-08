#include "core/quirc_qr_recognizer.h"
#include "interfaces/qr_recognizer.h"
#include "ui/qr_recognition_task.h"
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QStringList>
#include <QTest>
#include <memory>
namespace
{
using namespace waibusnap;
QString fixture(const QString& name)
{
    return QStringLiteral(WAIBUSNAP_QR_FIXTURE_DIR) + QLatin1Char('/') + name;
}
QrRecognitionResult recognize(const std::shared_ptr<const QrRecognizer>& engine,
                              const QImage& image, double* elapsed = nullptr)
{
    QElapsedTimer clock;
    clock.start();
    QrRecognitionTask task(engine, image, 42);
    QSignalSpy spy(&task, &QrRecognitionTask::completed);
    task.start();
    if (!spy.wait(20000))
        return {false, QStringLiteral("真实引擎测试超过 20 秒。"), {}};
    if (elapsed)
        *elapsed = clock.nsecsElapsed() / 1e6;
    if (spy.first()[0].toULongLong() != 42)
        return {false, QStringLiteral("识别任务 token 不匹配。"), {}};
    return qvariant_cast<QrRecognitionResult>(spy.first()[1]);
}
QStringList texts(const QrRecognitionResult& result)
{
    QStringList values;
    for (const auto& code : result.codes)
        values.append(code.text);
    return values;
}
} // namespace
class QrRecognizerTest : public QObject
{
    Q_OBJECT
    std::shared_ptr<const QrRecognizer> platform_ =
        std::shared_ptr<const QrRecognizer>(createQrRecognizer());
    std::shared_ptr<const QrRecognizer> quirc_ =
        std::shared_ptr<const QrRecognizer>(createQuircQrRecognizer());
  private slots:
    void initTestCase()
    {
        QVERIFY(platform_);
        QVERIFY(quirc_);
    }
    void platformDecodesUrl()
    {
        const QImage image(fixture(QStringLiteral("qr-url.png")));
        QVERIFY(!image.isNull());
        const auto result = recognize(platform_, image);
        QVERIFY2(result.ok, qPrintable(result.explanation));
        QCOMPARE(texts(result), QStringList{QStringLiteral("https://example.com/waibu?q=1")});
        QVERIFY(!result.codes.first().box.isEmpty());
        QVERIFY(image.rect().contains(result.codes.first().box.toAlignedRect()));
    }
    void platformDecodesText()
    {
        const QImage image(fixture(QStringLiteral("qr-text.png")));
        QVERIFY(!image.isNull());
        const auto result = recognize(platform_, image);
        QVERIFY2(result.ok, qPrintable(result.explanation));
        QCOMPARE(texts(result), QStringList{QStringLiteral("hello waibusnap")});
    }
    void platformDecodesMultipleInReadingOrder()
    {
        const QImage image(fixture(QStringLiteral("qr-multi.png")));
        QVERIFY(!image.isNull());
        const auto result = recognize(platform_, image);
        QVERIFY2(result.ok, qPrintable(result.explanation));
        QCOMPARE(texts(result), (QStringList{QStringLiteral("https://example.com/waibu?q=1"),
                                             QStringLiteral("hello waibusnap")}));
        for (const auto& code : result.codes)
        {
            QVERIFY(!code.box.isEmpty());
            QVERIFY(image.rect().contains(code.box.toAlignedRect()));
        }
    }
    void quircMatchesFixtures()
    {
        const QImage image(fixture(QStringLiteral("qr-multi.png")));
        QVERIFY(!image.isNull());
        const auto result = recognize(quirc_, image);
        QVERIFY2(result.ok, qPrintable(result.explanation));
        QCOMPARE(texts(result), (QStringList{QStringLiteral("https://example.com/waibu?q=1"),
                                             QStringLiteral("hello waibusnap")}));
    }
    void blankAndInvalid()
    {
        QImage blank(640, 480, QImage::Format_RGB32);
        blank.fill(Qt::white);
        const auto blankResult = recognize(platform_, blank);
        QVERIFY2(blankResult.ok, qPrintable(blankResult.explanation));
        QVERIFY(blankResult.codes.isEmpty());
        const auto invalid = recognize(platform_, {});
        QVERIFY(!invalid.ok);
        QVERIFY(invalid.explanation.contains(QStringLiteral("图像")));
    }
    void timing()
    {
        const QImage image(fixture(QStringLiteral("qr-multi.png")));
        double elapsed = 0;
        const auto result = recognize(platform_, image, &elapsed);
        QVERIFY2(result.ok, qPrintable(result.explanation));
        qInfo() << "二维码真实引擎耗时(ms)：" << elapsed;
    }
};
QTEST_GUILESS_MAIN(QrRecognizerTest)
#include "qr_recognizer_test.moc"
