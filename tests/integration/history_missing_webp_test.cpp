#include "app/history_manager.h"
#include "ui/history_window.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QImageReader>
#include <QLabel>
#include <QListWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
using namespace waibusnap;
class HistoryMissingWebpTest final : public QObject
{
    Q_OBJECT
  private slots:
    void missingPluginIsExplicitAndOtherRecordsSurvive()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        // 独立进程已加载 offscreen 平台插件，在首次图片查询前移除动态插件路径。
        // 不移动真实 Qt 插件，也不伪造 supportedImageFormats 的返回值。
        QCoreApplication::setLibraryPaths({temp.path()});
        QVERIFY(!QImageReader::supportedImageFormats().contains("webp"));
        AppSettings settings(temp.filePath(QStringLiteral("settings.ini")));
        HistoryManager manager(settings, temp.filePath(QStringLiteral("历史")));
        QVERIFY(manager.initialize().success);
        const auto folder = manager.currentGroup()->absolutePath;
        const auto webp = QDir(folder).filePath(QStringLiteral("外部.webp"));
        QVERIFY(QFile::copy(QString::fromUtf8(WAIBUSNAP_WEBP_FIXTURE), webp));
        QImage png(3, 2, QImage::Format_ARGB32);
        png.fill(Qt::red);
        QVERIFY(png.save(QDir(folder).filePath(QStringLiteral("正常.png")), "png"));
        const auto scan = manager.refresh();
        QVERIFY(scan.error.isEmpty());
        QCOMPARE(scan.groups[0].items.size(), 2);
        for (const auto& item : scan.groups[0].items)
        {
            const auto loaded = manager.loadImage(historyItemId(item));
            if (item.absolutePath == webp)
            {
                QVERIFY(!loaded.success);
                QVERIFY(loaded.error.contains(QStringLiteral("缺少 Qt WebP 图片插件")));
                QVERIFY(loaded.error.contains(QStringLiteral("qtimageformats")));
                QVERIFY(loaded.image.isNull());
                const auto thumbnail = manager.loadImage(historyItemId(item), {96, 72});
                QCOMPARE(thumbnail.error, loaded.error);
                QVERIFY(manager.setStarred(historyItemId(item), true).success);
                QVERIFY(!manager.findItem(historyItemId(item))->managed);
            }
            else
            {
                QVERIFY2(loaded.success, qPrintable(loaded.error));
                QCOMPARE(loaded.image.size(), QSize(3, 2));
            }
        }
        QVERIFY(manager.error().isEmpty());
        QCOMPARE(manager.refresh().groups[0].items.size(), 2);
        int copies = 0, pins = 0;
        HistoryWindowActions actions;
        actions.copyImage = [&](const QImage&)
        {
            ++copies;
            return ImageOutputResult{true, {}};
        };
        actions.pinImage = [&](const QImage&)
        {
            ++pins;
            return ImageOutputResult{true, {}};
        };
        HistoryWindow window(manager, settings, actions);
        // 栈上窗口由栈正常释放，不使用 close() 的延迟删除。
        window.setAttribute(Qt::WA_DeleteOnClose, false);
        window.show();
        QCoreApplication::processEvents();
        auto* list = window.findChild<QListWidget*>(QStringLiteral("historyThumbnails"));
        auto* empty = window.findChild<QLabel*>(QStringLiteral("historyEmpty"));
        auto* notice = window.findChild<QLabel*>(QStringLiteral("historyNotice"));
        auto* copy = window.findChild<QToolButton*>(QStringLiteral("historyCopy"));
        QVERIFY(list && empty && notice && copy);
        int webpRow = -1, pngRow = -1;
        for (int row = 0; row < list->count(); ++row)
        {
            if (list->item(row)->toolTip().endsWith(QLatin1String(".webp")))
                webpRow = row;
            else
                pngRow = row;
        }
        QVERIFY(webpRow >= 0 && pngRow >= 0);
        list->setCurrentRow(webpRow);
        QVERIFY(empty->isVisible());
        QVERIFY(empty->text().contains(QStringLiteral("缺少 Qt WebP 图片插件")));
        copy->click();
        QTest::keyClick(list, Qt::Key_Return, Qt::ShiftModifier);
        QCOMPARE(copies, 0);
        QCOMPARE(pins, 0);
        QVERIFY(notice->text().contains(QStringLiteral("qtimageformats")));
        list->setCurrentRow(pngRow);
        QVERIFY(!empty->isVisible());
        copy->click();
        QCOMPARE(copies, 1);
        QCOMPARE(manager.refresh().groups[0].items.size(), 2);
    }
};
QTEST_MAIN(HistoryMissingWebpTest)
#include "history_missing_webp_test.moc"
