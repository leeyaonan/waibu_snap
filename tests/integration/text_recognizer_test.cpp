#include "core/text_layout.h"
#include "ocr_engine_languages.h"
#include "output/annotation_renderer.h"
#include "ui/text_recognition_task.h"
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTest>
#include <algorithm>
#include <cmath>
using namespace waibusnap;
class TextRecognizerTest : public QObject
{
    Q_OBJECT
    std::shared_ptr<const TextRecognizer> recognizer_ = createTextRecognizer();
    QStringList languages_;
    bool available_ = false;
    TextRecognitionResult recognize(const QImage& image, double* elapsed = nullptr)
    {
        QElapsedTimer clock;
        clock.start();
        TextRecognitionTask task(recognizer_, renderAnnotatedSelection(image, {}, image.rect()),
                                 42);
        QSignalSpy spy(&task, &TextRecognitionTask::completed);
        task.start();
        if (!spy.wait(20000))
            return {false, QStringLiteral("真实引擎测试超过 20 秒。"), {}};
        if (elapsed)
            *elapsed = clock.nsecsElapsed() / 1e6;
        if (spy.first()[0].toULongLong() != 42)
            return {false, QStringLiteral("识别任务 token 不匹配。"), {}};
        return qvariant_cast<TextRecognitionResult>(spy.first()[1]);
    }
  private slots:
    void initTestCase()
    {
        languages_ = ocrEngineLanguages();
        available_ = ocrEngineAvailable();
        qInfo().noquote() << "系统 OCR 支持列表：" << languages_.join(',');
        if (ocrRequiresChinese())
            QVERIFY2(!languages_.isEmpty(), "Vision 必须明确返回支持列表，不能跳过测试");
    }
    void fixedSample()
    {
        const QImage image(QStringLiteral(WAIBUSNAP_OCR_FIXTURE));
        QVERIFY(!image.isNull());
        const auto result = recognize(image);
        if (!available_ && !ocrRequiresChinese())
        {
            QVERIFY(!result.ok);
            QVERIFY2(result.explanation.contains(QStringLiteral("语言包")),
                     qPrintable(result.explanation));
            return;
        }
        QVERIFY2(result.ok, qPrintable(result.explanation));
        QVERIFY(result.lines.size() >= 8);
        const auto text = text_layout::assembleFullText(result.lines);
        QVERIFY2(text.contains("WaibuSnap", Qt::CaseInsensitive), qPrintable(text));
        QVERIFY2(text.contains("offline text recognition", Qt::CaseInsensitive), qPrintable(text));
        if (ocrRequiresChinese() && languages_.contains("zh-Hans"))
        {
            QVERIFY2(text.contains(QStringLiteral("离线文字识别")), qPrintable(text));
            QVERIFY2(text.contains(QStringLiteral("手动编辑")), qPrintable(text));
        }
        else if (ocrRequiresChinese())
            QVERIFY(result.explanation.contains(QStringLiteral("未提供简体中文")));
        for (const auto& line : result.lines)
        {
            QVERIFY(!line.box.isEmpty());
            QVERIFY(line.box.left() >= 0 && line.box.top() >= 0);
            QVERIFY(line.box.right() <= image.width() && line.box.bottom() <= image.height());
            for (const auto& token : line.tokens)
                QCOMPARE(token.text, line.text.mid(token.offset, token.length));
        }
    }
    void blankAndInvalid()
    {
        QImage blank(640, 480, QImage::Format_RGB32);
        blank.fill(Qt::white);
        const auto result = recognize(blank);
        if (!available_ && !ocrRequiresChinese())
        {
            QVERIFY(!result.ok);
            QVERIFY(result.explanation.contains(QStringLiteral("语言包")));
        }
        else
        {
            QVERIFY2(result.ok, qPrintable(result.explanation));
            QVERIFY(result.lines.isEmpty());
        }
        const auto invalid = recognize({});
        QVERIFY(!invalid.ok);
        QVERIFY(invalid.explanation.contains(QStringLiteral("图像")));
    }
    void timing()
    {
        const QImage image(QStringLiteral(WAIBUSNAP_OCR_FIXTURE));
        const int count = qEnvironmentVariableIntValue("WAIBUSNAP_OCR_BENCHMARK") == 1 ? 30 : 1;
        QVector<double> times;
        for (int i = 0; i < count; ++i)
        {
            double elapsed = 0;
            const auto result = recognize(image, &elapsed);
            if (!available_ && !ocrRequiresChinese())
            {
                QVERIFY(!result.ok);
                QVERIFY(result.explanation.contains(QStringLiteral("语言包")));
                qInfo() << "引擎不可用，已核对中文失败路径，无识别性能数值。";
                return;
            }
            QVERIFY2(result.ok, qPrintable(result.explanation));
            times.append(elapsed);
        }
        std::sort(times.begin(), times.end());
        qInfo() << "触发到结果可用，次数：" << count << "P95(ms)："
                << times[int(std::ceil(count * 0.95)) - 1] << "最小(ms)：" << times.first()
                << "最大(ms)：" << times.last();
    }
};
QTEST_GUILESS_MAIN(TextRecognizerTest)
#include "text_recognizer_test.moc"
