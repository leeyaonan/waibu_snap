#include "core/text_layout.h"
#include <QTest>
using namespace waibusnap;
using namespace waibusnap::text_layout;
class TextLayoutTest : public QObject
{
    Q_OBJECT
  private slots:
    void tokenize_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<QStringList>("words");
        QTest::newRow("empty") << QString() << QStringList();
        QTest::newRow("latin") << QStringLiteral("Hello, can't stop42.")
                               << QStringList{"Hello", "can't", "stop42"};
        QTest::newRow("mixed") << QStringLiteral("你好，世界！OCR2026 中文")
                               << QStringList{QStringLiteral("你好"), QStringLiteral("世界"),
                                              "OCR2026", QStringLiteral("中文")};
        QTest::newRow("emoji") << QString::fromUtf8("A😀 👨‍👩‍👧 B")
                               << QStringList{"A", QString::fromUtf8("😀"),
                                              QString::fromUtf8("👨‍👩‍👧"), "B"};
        QTest::newRow("punctuation") << QStringLiteral(" ,。!\t\n") << QStringList();
        QTest::newRow("extended-cjk")
            << QString::fromUtf8("𠀀中文abc") << QStringList{QString::fromUtf8("𠀀中文"), "abc"};
    }
    void tokenize()
    {
        QFETCH(QString, text);
        QFETCH(QStringList, words);
        QStringList actual;
        for (const auto span : tokenizeLine(text))
            actual.append(text.mid(span.offset, span.length));
        QCOMPARE(actual, words);
    }
    void paragraphs()
    {
        QVector<RecognizedLine> lines = {{"A", {0, 0, 100, 10}, {}},
                                         {"B", {0, 20, 100, 10}, {}},
                                         {"C", {0, 46, 100, 10}, {}},
                                         {"D", {0, 73, 100, 10}, {}}};
        QCOMPARE(assembleFullText(lines), QString("A\nB\nC\n\nD"));
        QCOMPARE(assembleFullText({}), QString());
        lines[0].text = "  A! ";
        QVERIFY(assembleFullText(lines).startsWith("  A! \n"));
    }
    void nearestAndFallback()
    {
        RecognizedLine line{"hello world",
                            {0, 0, 100, 10},
                            {{"hello", {0, 0, 30, 10}, 0, 5}, {"world", {60, 0, 40, 10}, 6, 5}}};
        QCOMPARE(nearestToken(line, -10), 0);
        QCOMPARE(nearestToken(line, 55), 1);
        QCOMPARE(nearestToken(line, 105), 1);
        line.tokens.clear();
        QCOMPARE(nearestToken(line, 55), 0);
        QCOMPARE(selectableTokens(line).first().length, 11);
        QCOMPARE(nearestToken({}, 0), -1);
    }
    void selectionAndSlices()
    {
        QVector<RecognizedLine> lines = {
            {QStringLiteral("Hello, world!  "),
             {0, 0, 100, 10},
             {{"Hello", {0, 0, 30, 10}, 0, 5}, {"world", {60, 0, 40, 10}, 7, 5}}},
            {QString::fromUtf8("  你好，😀 OCR。"),
             {0, 20, 100, 10},
             {{QStringLiteral("你好"), {0, 20, 30, 10}, 2, 2},
              {QString::fromUtf8("😀"), {40, 20, 10, 10}, 5, 2},
              {"OCR", {60, 20, 40, 10}, 8, 3}}}};
        QCOMPARE(selectionText(lines, selectionRange(lines, {0, 0}, {0, 1})),
                 QString("Hello, world"));
        const auto ranges = selectionRange(lines, {1, 1}, {0, 1});
        QCOMPARE(ranges.size(), 2);
        QCOMPARE(selectionText(lines, ranges), QString::fromUtf8("world!  \n  你好，😀"));
        QCOMPARE(selectionText(lines, selectionRange(lines, {1, 1}, {1, 1})),
                 QString::fromUtf8("😀"));
        QVERIFY(selectionRange(lines, {-1, 0}, {0, 0}).isEmpty());
        QVERIFY(selectionRange(lines, {0, 9}, {0, 0}).isEmpty());
    }
};
QTEST_APPLESS_MAIN(TextLayoutTest)
#include "text_layout_test.moc"
