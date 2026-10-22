#include "ui/annotation_text_edit.h"
#include <QApplication>
#include <QFontMetricsF>
#include <QInputMethodEvent>
#include <QSignalSpy>
#include <QTest>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextLayout>

class AnnotationTextEditTest final : public QObject
{
    Q_OBJECT
  private slots:
    void transparentPixels_data()
    {
        QTest::addColumn<QColor>("background");
        QTest::newRow("light") << QColor(Qt::white);
        QTest::newRow("dark") << QColor(20, 20, 20);
        QTest::newRow("color") << QColor(35, 175, 110);
    }
    void transparentPixels()
    {
        QFETCH(QColor, background);
        QWidget canvas;
        canvas.resize(500, 300);
        QPalette palette = canvas.palette();
        palette.setColor(QPalette::Window, background);
        canvas.setPalette(palette);
        canvas.setAutoFillBackground(true);
        canvas.setFocusPolicy(Qt::StrongFocus);
        waibusnap::AnnotationTextEdit editor(&canvas);
        editor.setPlaceholderText({});
        editor.setEditingArea({50, 50}, canvas.rect());
        canvas.show();
        editor.hide();
        const QImage before = canvas.grab().toImage();
        editor.show();
        canvas.setFocus();
        QCoreApplication::processEvents();
        for (auto group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled})
        {
            QCOMPARE(editor.palette().color(group, QPalette::Base).alpha(), 0);
            QCOMPARE(editor.viewport()->palette().color(group, QPalette::Base).alpha(), 0);
        }
        QVERIFY(!editor.autoFillBackground());
        QVERIFY(!editor.viewport()->autoFillBackground());
        QVERIFY(!editor.testAttribute(Qt::WA_OpaquePaintEvent));
        QVERIFY(!editor.viewport()->testAttribute(Qt::WA_OpaquePaintEvent));
        QCOMPARE(editor.frameShape(), QFrame::NoFrame);
        QCOMPARE(editor.document()->documentMargin(), qreal(0));
        const QImage after = canvas.grab().toImage();
        QVERIFY(after != before);
        // 边界显示橙色虚线；框内仍保持画布背景透明。
        const qreal scale = qreal(after.width()) / canvas.width();
        const QRect logical = editor.geometry();
        const QRect area(qRound(logical.x() * scale), qRound(logical.y() * scale),
                         qRound(logical.width() * scale), qRound(logical.height() * scale));
        int borderPixels = 0;
        for (int x = area.left(); x <= area.right(); ++x)
        {
            const QColor top = after.pixelColor(x, area.top() + 1);
            const QColor bottom = after.pixelColor(x, area.bottom() - 1);
            borderPixels += top.red() > 180 && top.green() > 60 && top.blue() < 120;
            borderPixels += bottom.red() > 180 && bottom.green() > 60 && bottom.blue() < 120;
        }
        QVERIFY(borderPixels > 0);
        const QPoint inside(area.center().x(), area.center().y());
        QCOMPARE(after.pixelColor(inside), before.pixelColor(inside));
    }
    void focusedEmptyHintIsVisible_data()
    {
        QTest::addColumn<QColor>("background");
        QTest::newRow("light") << QColor(Qt::white);
        QTest::newRow("dark") << QColor(20, 20, 20);
    }
    void focusedEmptyHintIsVisible()
    {
        QFETCH(QColor, background);
        QWidget canvas;
        canvas.resize(500, 300);
        QPalette palette = canvas.palette();
        palette.setColor(QPalette::Window, background);
        canvas.setPalette(palette);
        canvas.setAutoFillBackground(true);
        canvas.setAttribute(Qt::WA_StyledBackground, true);
        canvas.setStyleSheet(QStringLiteral("background-color: %1;").arg(background.name()));
        canvas.show();
        waibusnap::AnnotationTextEdit editor(&canvas);
        editor.setEditingArea({50, 50}, canvas.rect());
        editor.show();
        editor.setFocus();
        QCoreApplication::processEvents();
        QVERIFY(editor.hasFocus());
        const QRect hintArea(56, 50, 120, 24);
        const QImage withHint = canvas.grab().toImage();
        editor.setPlaceholderText({});
        editor.viewport()->update();
        QCoreApplication::processEvents();
        const QImage withoutHint = canvas.grab().toImage();
        const qreal scale = qreal(withHint.width()) / canvas.width();
        const QRect hintPixels(qRound(hintArea.x() * scale), qRound(hintArea.y() * scale),
                               qRound(hintArea.width() * scale), qRound(hintArea.height() * scale));
        QVERIFY(withHint.copy(hintPixels) != withoutHint.copy(hintPixels));
        const QColor actualBackground = withHint.pixelColor(0, 0);
        bool contrast = false;
        for (int y = hintPixels.top(); y <= hintPixels.bottom(); ++y)
            for (int x = hintPixels.left(); x <= hintPixels.right(); ++x)
            {
                const int luminance = qGray(withHint.pixel(x, y));
                contrast |= actualBackground.lightness() > 128 ? luminance < 80 : luminance > 180;
            }
        QVERIFY(contrast);
        editor.hide();
    }
    void singleLineAndLimits_data()
    {
        QTest::addColumn<int>("pixels");
        for (int size : {9, 14, 18, 28, 40, 56})
            QTest::newRow(qPrintable(QString::number(size))) << size;
    }
    void singleLineAndLimits()
    {
        QFETCH(int, pixels);
        QWidget canvas;
        canvas.resize(800, 600);
        waibusnap::AnnotationTextEdit editor(&canvas);
        QFont font = editor.font();
        font.setPixelSize(pixels);
        editor.setFont(font);
        editor.setEditingArea({40, 40}, canvas.rect());
        canvas.show();
        editor.show();
        const int singleHeight = editor.height();
        QCOMPARE(editor.width(), 240);
        QVERIFY(singleHeight >= QFontMetricsF(font).height());
        QVERIFY(singleHeight < QFontMetricsF(font).height() * 2);
        QVERIFY(!editor.placeholderText().contains('\n'));
        QCOMPARE(editor.lineWrapMode(), QTextEdit::NoWrap);
        const int characters = qRound(300 / QFontMetricsF(font).horizontalAdvance('W'));
        editor.setPlainText(QString(characters, 'W') + QStringLiteral("\nA"));
        QVERIFY(editor.width() > 280 && editor.width() < 330);
        editor.setPlainText(QStringLiteral("A\nB\nC"));
        QVERIFY(editor.height() > singleHeight * 2);
        editor.setPlainText(QString(200, 'W'));
        QCOMPARE(editor.width(), 400);
        QCOMPARE(editor.height(), singleHeight);
        editor.moveCursor(QTextCursor::End);
        editor.ensureCursorVisible();
        QTRY_VERIFY(editor.viewport()->rect().intersects(editor.cursorRect()));
        editor.setPlainText(QStringLiteral("A\n").repeated(100));
        QCOMPARE(editor.height(), 240);
        editor.moveCursor(QTextCursor::End);
        editor.ensureCursorVisible();
        QTRY_VERIFY(editor.viewport()->rect().intersects(editor.cursorRect()));
        editor.setPlainText(QStringLiteral("A"));
        QCOMPARE(editor.width(), 240);
        QCOMPARE(editor.height(), singleHeight);
    }
    void bounds_data()
    {
        QTest::addColumn<QRect>("area");
        QTest::addColumn<QPoint>("origin");
        QTest::newRow("bottom-right") << QRect(0, 0, 800, 600) << QPoint(799, 599);
        QTest::newRow("narrow") << QRect(0, 0, 140, 80) << QPoint(139, 79);
        QTest::newRow("one-pixel") << QRect(0, 0, 1, 1) << QPoint(0, 0);
        QTest::newRow("outside") << QRect(0, 0, 320, 200) << QPoint(-10, -10);
        QTest::newRow("offset") << QRect(20, 30, 500, 400) << QPoint(510, 420);
    }
    void bounds()
    {
        QFETCH(QRect, area);
        QFETCH(QPoint, origin);
        QWidget canvas;
        waibusnap::AnnotationTextEdit editor(&canvas);
        editor.setEditingArea(origin, area);
        QVERIFY(area.contains(editor.geometry()));
        const QPoint anchored = editor.pos();
        editor.setPlainText((QString(200, 'W') + '\n').repeated(100));
        QVERIFY(area.contains(editor.geometry()));
        QCOMPARE(editor.pos(), anchored);
        QVERIFY(editor.width() <= 400);
        QVERIFY(editor.height() <= 240);
        QCOMPARE(editor.horizontalScrollBarPolicy(), Qt::ScrollBarAlwaysOff);
        QCOMPARE(editor.verticalScrollBarPolicy(), Qt::ScrollBarAlwaysOff);
    }
    void preeditSurvivesLayoutAndShortcut()
    {
        QWidget canvas;
        canvas.resize(800, 600);
        waibusnap::AnnotationTextEdit editor(&canvas);
        editor.setEditingArea({50, 50}, canvas.rect());
        canvas.show();
        editor.show();
        editor.setFocus();
        editor.insertPlainText(QStringLiteral("原文"));
        const int cursor = editor.textCursor().position();
        const QString candidate = QStringLiteral("中文候选").repeated(40);
        QInputMethodEvent preedit(candidate, {});
        QApplication::sendEvent(&editor, &preedit);
        QVERIFY(editor.isComposing());
        QCOMPARE(editor.width(), 400);
        editor.setEditingArea({60, 60}, canvas.rect());
        QCOMPARE(editor.document()->firstBlock().layout()->preeditAreaText(), candidate);
        QCOMPARE(editor.textCursor().position(), cursor);
        QCOMPARE(editor.toPlainText(), QStringLiteral("原文"));
        QSignalSpy commit(&editor, &waibusnap::AnnotationTextEdit::commitRequested);
        QSignalSpy cancel(&editor, &waibusnap::AnnotationTextEdit::cancelRequested);
        QTest::keyClick(&editor, Qt::Key_Return, Qt::ControlModifier);
        QCOMPARE(commit.count(), 0);
        QVERIFY(editor.isComposing());
        QInputMethodEvent converted;
        converted.setCommitString(QStringLiteral("中文"));
        QApplication::sendEvent(&editor, &converted);
        QVERIFY(!editor.isComposing());
        QCOMPARE(editor.toPlainText(), QStringLiteral("原文中文"));
        QTest::keyClick(&editor, Qt::Key_Return);
        editor.insertPlainText(QStringLiteral("下一行"));
        QCOMPARE(editor.toPlainText(), QStringLiteral("原文中文\n下一行"));
        QTest::keyClick(&editor, Qt::Key_Return, Qt::ControlModifier);
        QCOMPARE(commit.count(), 1);
        QApplication::sendEvent(&editor, &preedit);
        QTest::keyClick(&editor, Qt::Key_Escape);
        QCOMPARE(cancel.count(), 0);
        QVERIFY(!editor.isComposing());
        QTest::keyClick(&editor, Qt::Key_Escape);
        QCOMPARE(cancel.count(), 1);
    }
    void annotationFontAppliesToEditorAndTyping()
    {
        QWidget canvas;
        canvas.resize(800, 600);
        waibusnap::AnnotationTextEdit editor(&canvas);
        editor.setEditingArea({50, 50}, canvas.rect());
        canvas.show();
        editor.show();
        editor.setFocus();
        QFont font = editor.font();
        font.setPixelSize(28);
        font.setBold(true);
        font.setItalic(true);
        font.setUnderline(true);
        editor.setAnnotationFont(font);
        QTest::keyClicks(&editor, "ABC");
        QCOMPARE(editor.toPlainText(), QStringLiteral("ABC"));
        QVERIFY(editor.font().bold());
        QVERIFY(editor.font().italic());
        QVERIFY(editor.font().underline());
        QTextCursor cursor = editor.document()->find(QStringLiteral("A"));
        QVERIFY(!cursor.isNull());
        cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
        const QTextCharFormat format = cursor.charFormat();
        QVERIFY(format.fontWeight() >= QFont::Bold);
        QVERIFY(format.fontItalic());
        QVERIFY(format.fontUnderline());
        QSignalSpy formats(&editor, &waibusnap::AnnotationTextEdit::formatToggleRequested);
        QTest::keyClick(&editor, Qt::Key_B, Qt::ControlModifier);
        QTest::keyClick(&editor, Qt::Key_I, Qt::ControlModifier);
        QTest::keyClick(&editor, Qt::Key_U, Qt::ControlModifier);
        QCOMPARE(formats.count(), 3);
        QCOMPARE(formats.at(0).at(0).toInt(), 0);
        QCOMPARE(formats.at(1).at(0).toInt(), 1);
        QCOMPARE(formats.at(2).at(0).toInt(), 2);
    }
    void textUndoRedoKeepsOnlyTextHistory()
    {
        QWidget canvas;
        canvas.resize(800, 600);
        waibusnap::AnnotationTextEdit editor(&canvas);
        editor.setEditingArea({50, 50}, canvas.rect());
        canvas.show();
        editor.show();
        editor.setFocus();
        editor.clear();
        editor.setEditingArea({50, 50}, canvas.rect());
        QTest::keyClicks(&editor, "abc");
        editor.undo();
        QVERIFY(editor.toPlainText().isEmpty());
        QVERIFY(!editor.document()->isUndoAvailable());
        QVERIFY(editor.document()->isRedoAvailable());
        editor.redo();
        QCOMPARE(editor.toPlainText(), QStringLiteral("abc"));
    }
};
QTEST_MAIN(AnnotationTextEditTest)
#include "annotation_text_edit_test.moc"
