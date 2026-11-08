#include "app/sticker_manager.h"
#include "interfaces/capture_provider.h"
#include "ui/qr_result_dialog.h"
#include "ui/selection_overlay.h"
#include "ui/sticker_window.h"
#include <QApplication>
#include <QLabel>
#include <QListWidget>
#include <QMutex>
#include <QMutexLocker>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTest>
#include <QThread>
#include <atomic>
#include <utility>
namespace
{
using namespace waibusnap;
CaptureFrame frame()
{
    CaptureFrame result;
    result.display.logicalGeometry = QRect(0, 0, 800, 600);
    result.display.availableLogicalGeometry = result.display.logicalGeometry;
    result.display.devicePixelRatio = 1;
    result.pixels = QImage(800, 600, QImage::Format_ARGB32_Premultiplied);
    result.pixels.fill(Qt::white);
    return result;
}
QPushButton* button(QWidget& widget, const char* name)
{
    return widget.findChild<QPushButton*>(QString::fromLatin1(name));
}
void drag(QWidget& widget, QPoint first, QPoint last)
{
    QTest::mousePress(&widget, Qt::LeftButton, Qt::NoModifier, first);
    QTest::mouseMove(&widget, last);
    QTest::mouseRelease(&widget, Qt::LeftButton, Qt::NoModifier, last);
}
QrCode code(const QString& text, QRectF box) { return {text, std::move(box)}; }
class QrStub final : public QrRecognizer
{
  public:
    mutable std::atomic<int> calls{0};
    mutable std::atomic<bool> worker{false};
    int delayMs = 0;
    QVector<QrRecognitionResult> results;
    QrRecognitionResult recognize(const QImage& image) const override
    {
        const int index = calls.fetch_add(1);
        worker = QThread::currentThread() != qApp->thread();
        {
            QMutexLocker lock(&mutex_);
            input_ = image;
        }
        if (delayMs)
            QThread::msleep(delayMs);
        return results.isEmpty() ? QrRecognitionResult{true, {}, {}}
                                 : results[std::min(index, int(results.size()) - 1)];
    }
    QImage input() const
    {
        QMutexLocker lock(&mutex_);
        return input_;
    }

  private:
    mutable QMutex mutex_;
    mutable QImage input_;
};
QrResultDialog* dialog(QWidget& widget)
{
    return widget.findChild<QrResultDialog*>(QStringLiteral("qrResultDialog"));
}
QString content(QrResultDialog* resultDialog)
{
    auto* edit = resultDialog->findChild<QPlainTextEdit*>(QStringLiteral("qrContentEdit"));
    return edit ? edit->toPlainText() : QString();
}
} // namespace
class QrUiTest final : public QObject
{
    Q_OBJECT
  private slots:
    void overlayCopyAndOpenUrl()
    {
        auto engine = std::make_shared<QrStub>();
        engine->results = {
            {true,
             {},
             {code(QStringLiteral("https://example.com/waibu?q=1"), {20, 20, 120, 120})}}};
        QString copied;
        QUrl opened;
        int opens = 0;
        bool openSucceeds = true;
        OverlayActions actions;
        actions.qrRecognizer = engine;
        actions.setClipboardText = [&](const QString& text) { copied = text; };
        actions.openUrl = [&](const QUrl& url)
        {
            opened = url;
            ++opens;
            return openSucceeds;
        };
        SelectionOverlay overlay(frame(), actions);
        overlay.show();
        drag(overlay, {100, 100}, {650, 450});
        const QRect selection = overlay.selection();
        QTest::mouseClick(button(overlay, "qrToolButton"), Qt::LeftButton);
        QTRY_VERIFY(!overlay.isRecognizingQr());
        QVERIFY(engine->worker.load());
        QCOMPARE(engine->input().size(), selection.size());
        auto* resultDialog = dialog(overlay);
        QVERIFY(resultDialog);
        QCOMPARE(content(resultDialog), QStringLiteral("https://example.com/waibu?q=1"));
        auto* open = resultDialog->findChild<QPushButton*>(QStringLiteral("qrOpenButton"));
        auto* copy = resultDialog->findChild<QPushButton*>(QStringLiteral("qrCopyButton"));
        QVERIFY(open && open->isEnabled());
        QCOMPARE(copy->text(), QStringLiteral("复制链接"));
        QTest::mouseClick(copy, Qt::LeftButton);
        QCOMPARE(copied, QStringLiteral("https://example.com/waibu?q=1"));
        QTest::mouseClick(open, Qt::LeftButton);
        QCOMPARE(opens, 1);
        QCOMPARE(opened, QUrl(QStringLiteral("https://example.com/waibu?q=1")));
        openSucceeds = false;
        QTest::mouseClick(open, Qt::LeftButton);
        QCOMPARE(opens, 2);
        QCOMPARE(resultDialog->findChild<QLabel*>(QStringLiteral("qrCopyFeedback"))->text(),
                 QStringLiteral("无法打开链接"));
        QTest::mouseClick(resultDialog->findChild<QPushButton*>(QStringLiteral("qrCloseButton")),
                          Qt::LeftButton);
        QTRY_VERIFY(!dialog(overlay));
        QVERIFY(overlay.isVisible());
    }
    void overlayKeepsNonLinkCopyOnly()
    {
        auto engine = std::make_shared<QrStub>();
        engine->results = {
            {true, {}, {code(QStringLiteral("hello waibusnap"), {20, 20, 120, 120})}}};
        QString copied;
        int opens = 0;
        OverlayActions actions;
        actions.qrRecognizer = engine;
        actions.setClipboardText = [&](const QString& text) { copied = text; };
        actions.openUrl = [&](const QUrl&)
        {
            ++opens;
            return true;
        };
        SelectionOverlay overlay(frame(), actions);
        overlay.show();
        drag(overlay, {100, 100}, {650, 450});
        QTest::mouseClick(button(overlay, "qrToolButton"), Qt::LeftButton);
        QTRY_VERIFY(!overlay.isRecognizingQr());
        auto* resultDialog = dialog(overlay);
        QVERIFY(resultDialog);
        QCOMPARE(content(resultDialog), QStringLiteral("hello waibusnap"));
        auto* open = resultDialog->findChild<QPushButton*>(QStringLiteral("qrOpenButton"));
        QVERIFY(open && !open->isEnabled());
        QCOMPARE(resultDialog->findChild<QPushButton*>(QStringLiteral("qrCopyButton"))->text(),
                 QStringLiteral("复制内容"));
        QTest::mouseClick(open, Qt::LeftButton);
        QCOMPARE(opens, 0);
        QTest::mouseClick(resultDialog->findChild<QPushButton*>(QStringLiteral("qrCopyButton")),
                          Qt::LeftButton);
        QCOMPARE(copied, QStringLiteral("hello waibusnap"));
    }
    void overlaySwitchesMultipleResults()
    {
        auto engine = std::make_shared<QrStub>();
        engine->results = {{true,
                            {},
                            {code(QStringLiteral("https://example.com/first"), {20, 20, 100, 100}),
                             code(QStringLiteral("plain text"), {200, 20, 100, 100})}}};
        QString copied;
        QUrl opened;
        int opens = 0;
        OverlayActions actions;
        actions.qrRecognizer = engine;
        actions.setClipboardText = [&](const QString& text) { copied = text; };
        actions.openUrl = [&](const QUrl& url)
        {
            opened = url;
            ++opens;
            return true;
        };
        SelectionOverlay overlay(frame(), actions);
        overlay.show();
        drag(overlay, {100, 100}, {650, 450});
        QTest::mouseClick(button(overlay, "qrToolButton"), Qt::LeftButton);
        QTRY_VERIFY(!overlay.isRecognizingQr());
        auto* resultDialog = dialog(overlay);
        QVERIFY(resultDialog);
        auto* list = resultDialog->findChild<QListWidget*>(QStringLiteral("qrResultList"));
        auto* open = resultDialog->findChild<QPushButton*>(QStringLiteral("qrOpenButton"));
        auto* copy = resultDialog->findChild<QPushButton*>(QStringLiteral("qrCopyButton"));
        QVERIFY(list && open && copy);
        QCOMPARE(list->count(), 2);
        QCOMPARE(list->currentRow(), 0);
        QVERIFY(open->isEnabled());
        list->setCurrentRow(1);
        QCOMPARE(content(resultDialog), QStringLiteral("plain text"));
        QVERIFY(!open->isEnabled());
        QTest::mouseClick(copy, Qt::LeftButton);
        QCOMPARE(copied, QStringLiteral("plain text"));
        list->setCurrentRow(0);
        QVERIFY(open->isEnabled());
        QTest::mouseClick(open, Qt::LeftButton);
        QCOMPARE(opens, 1);
        QCOMPARE(opened, QUrl(QStringLiteral("https://example.com/first")));
    }
    void overlayRetryKeepsDialogAndReplacesResult()
    {
        auto engine = std::make_shared<QrStub>();
        engine->results = {
            {true, {}, {code(QStringLiteral("https://example.com/first"), {20, 20, 100, 100})}},
            {true, {}, {code(QStringLiteral("plain text"), {20, 20, 100, 100})}}};
        OverlayActions actions;
        actions.qrRecognizer = engine;
        SelectionOverlay overlay(frame(), actions);
        overlay.show();
        drag(overlay, {100, 100}, {650, 450});
        QTest::mouseClick(button(overlay, "qrToolButton"), Qt::LeftButton);
        QTRY_VERIFY(!overlay.isRecognizingQr());
        auto* resultDialog = dialog(overlay);
        QVERIFY(resultDialog);
        QCOMPARE(content(resultDialog), QStringLiteral("https://example.com/first"));
        QTest::mouseClick(resultDialog->findChild<QPushButton*>(QStringLiteral("qrRetryButton")),
                          Qt::LeftButton);
        QTRY_VERIFY(engine->calls.load() == 2 && !overlay.isRecognizingQr());
        QCOMPARE(dialog(overlay), resultDialog);
        QCOMPARE(content(resultDialog), QStringLiteral("plain text"));
        auto* open = resultDialog->findChild<QPushButton*>(QStringLiteral("qrOpenButton"));
        QVERIFY(open && !open->isEnabled());
    }
    void overlayEmptyAndFailureStatus()
    {
        auto engine = std::make_shared<QrStub>();
        engine->results = {{true, {}, {}}};
        OverlayActions actions;
        actions.qrRecognizer = engine;
        SelectionOverlay overlay(frame(), actions);
        overlay.show();
        drag(overlay, {100, 100}, {650, 450});
        QTest::mouseClick(button(overlay, "qrToolButton"), Qt::LeftButton);
        QTRY_VERIFY(!overlay.isRecognizingQr());
        QVERIFY(!dialog(overlay));
        auto* status = overlay.findChild<QLabel*>(QStringLiteral("outputStatus"));
        QVERIFY(status && status->text().contains(QStringLiteral("未识别到二维码")));
        engine->results = {{false, QStringLiteral("引擎不可用"), {}}};
        QTest::mouseClick(button(overlay, "qrToolButton"), Qt::LeftButton);
        QTRY_VERIFY(!overlay.isRecognizingQr());
        QVERIFY(!dialog(overlay));
        QVERIFY(status->text().contains(QStringLiteral("识别失败")));
        QVERIFY(status->text().contains(QStringLiteral("引擎不可用")));
    }
    void overlayDropsStaleSelection()
    {
        auto engine = std::make_shared<QrStub>();
        engine->delayMs = 250;
        engine->results = {
            {true, {}, {code(QStringLiteral("https://example.com/stale"), {20, 20, 100, 100})}}};
        OverlayActions actions;
        actions.qrRecognizer = engine;
        SelectionOverlay overlay(frame(), actions);
        overlay.show();
        drag(overlay, {100, 100}, {650, 450});
        QTest::mouseClick(button(overlay, "qrToolButton"), Qt::LeftButton);
        QVERIFY(overlay.isRecognizingQr());
        drag(overlay, {600, 400}, {700, 500});
        QTRY_VERIFY(!overlay.isRecognizingQr());
        QVERIFY(!dialog(overlay));
        auto* status = overlay.findChild<QLabel*>(QStringLiteral("outputStatus"));
        QVERIFY(status && status->text().contains(QStringLiteral("选区已变化")));
    }
    void stickerCopyAndOpenUrl()
    {
        auto engine = std::make_shared<QrStub>();
        engine->results = {
            {true, {}, {code(QStringLiteral("https://example.com/sticker"), {20, 20, 100, 100})}}};
        QString copied;
        QUrl opened;
        int opens = 0;
        StickerActions actions;
        actions.qrRecognizer = engine;
        actions.setClipboardText = [&](const QString& text) { copied = text; };
        actions.openUrl = [&](const QUrl& url)
        {
            opened = url;
            ++opens;
            return true;
        };
        StickerManager manager(nullptr, actions);
        QImage image(640, 480, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::white);
        QVERIFY(manager.create(image, {60, 60}, true).success);
        auto sticker = manager.windows().first();
        sticker->setEditing(true);
        auto* toolbar = sticker->findChild<QWidget*>(QStringLiteral("stickerEditToolbar"));
        QVERIFY(toolbar && toolbar->isVisible());
        QTest::mouseClick(button(*sticker, "stickerEditQrToolButton"), Qt::LeftButton);
        QTRY_VERIFY(!sticker->isQrRecognizing());
        QVERIFY(engine->worker.load());
        auto* resultDialog = dialog(*sticker);
        QVERIFY(resultDialog);
        QVERIFY(!toolbar->isVisible());
        QCOMPARE(content(resultDialog), QStringLiteral("https://example.com/sticker"));
        QTest::mouseClick(resultDialog->findChild<QPushButton*>(QStringLiteral("qrCopyButton")),
                          Qt::LeftButton);
        QCOMPARE(copied, QStringLiteral("https://example.com/sticker"));
        QTest::mouseClick(resultDialog->findChild<QPushButton*>(QStringLiteral("qrOpenButton")),
                          Qt::LeftButton);
        QCOMPARE(opens, 1);
        QCOMPARE(opened, QUrl(QStringLiteral("https://example.com/sticker")));
        QTest::mouseClick(resultDialog->findChild<QPushButton*>(QStringLiteral("qrCloseButton")),
                          Qt::LeftButton);
        QTRY_VERIFY(!dialog(*sticker));
        QTRY_VERIFY(toolbar->isVisible());
    }
};
QTEST_MAIN(QrUiTest)
#include "qr_ui_test.moc"
