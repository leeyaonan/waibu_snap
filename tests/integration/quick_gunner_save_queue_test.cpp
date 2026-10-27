#include "app/quick_gunner_save_queue.h"
#include <QDir>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <atomic>
using namespace waibusnap;
namespace
{
QImage picture(QColor color = Qt::red, QSize size = {32, 24})
{
    QImage image(size, QImage::Format_RGB32);
    image.fill(color);
    return image;
}
struct Release
{
    QSemaphore& gate;
    ~Release() { gate.release(100); }
};
}
class QuickGunnerSaveQueueTest : public QObject
{
    Q_OBJECT
  private slots:
    void formatsOrderAndCollision()
    {
        QTemporaryDir temp;
        AppSettings settings(temp.filePath(QStringLiteral("settings.ini")));
        const auto stamp = QDateTime::fromMSecsSinceEpoch(1700000000123LL);
        QuickGunnerSaveActions actions;
        actions.timestamp = [stamp] { return stamp; };
        QuickGunnerSaveQueue queue(settings, actions);
        queue.setDirectory(temp.path());
        QSignalSpy saved(&queue, &QuickGunnerSaveQueue::saved);
        QSignalSpy failed(&queue, &QuickGunnerSaveQueue::failed);
        queue.enqueue(picture(Qt::red));
        queue.enqueue(picture(Qt::green));
        QVERIFY(settings.saveSavePreferences({{}, ImageFormat::Jpeg}).isEmpty());
        queue.enqueue(picture(Qt::blue));
        QTRY_COMPARE(saved.size(), 3);
        QCOMPARE(failed.size(), 0);
        QCOMPARE(queue.pendingCount(), 0);
        QStringList paths;
        for (const auto& row : saved)
            paths << row.at(0).toString();
        QCOMPARE(QSet<QString>(paths.begin(), paths.end()).size(), 3);
        QVERIFY(paths[0].endsWith(QStringLiteral(".png")));
        QVERIFY(paths[1].endsWith(QStringLiteral(".png")));
        QVERIFY(paths[2].endsWith(QStringLiteral(".jpg")));
        QCOMPARE(QImage(paths[0]).pixelColor(0, 0), QColor(Qt::red));
        QCOMPARE(QImage(paths[1]).pixelColor(0, 0), QColor(Qt::green));
        QVERIFY(QImage(paths[2]).pixelColor(0, 0).blue() > 240);
    }
    void boundedWaiting_data()
    {
        QTest::addColumn<bool>("byteLimit");
        QTest::newRow("eight-frames") << false;
        QTest::newRow("256-megabytes") << true;
    }
    void boundedWaiting()
    {
        QFETCH(bool, byteLimit);
        QTemporaryDir temp;
        QSemaphore gate;
        std::atomic<int> calls{0};
        std::atomic<bool> onGui{false};
        QuickGunnerSaveActions actions;
        actions.write = [&](const QImage& image, const QString&, ImageFormat, QDateTime)
        {
            if (++calls == 1)
                gate.acquire();
            onGui = QThread::currentThread() == thread();
            return ImageFileResult{{true, {}}, QString::number(image.pixelColor(0, 0).red())};
        };
        QuickGunnerSaveQueue queue(AppSettings(temp.filePath(QStringLiteral("settings.ini"))),
                                   actions);
        Release release{gate};
        queue.setDirectory(temp.path());
        QSignalSpy dropped(&queue, &QuickGunnerSaveQueue::dropped);
        QSignalSpy saved(&queue, &QuickGunnerSaveQueue::saved);
        queue.enqueue(picture());
        QTRY_COMPARE(calls.load(), 1);
        const int count = byteLimit ? 5 : 10;
        const QSize size = byteLimit ? QSize(4096, 4096) : QSize(32, 24);
        for (int i = 1; i <= count; ++i)
            queue.enqueue(picture(QColor(i, 0, 0), size));
        QCOMPARE(queue.pendingCount(), byteLimit ? 5 : 9);
        QCOMPARE(queue.pendingBytes(),
                 byteLimit ? qint64(256) * 1024 * 1024 : qint64(8 * 32 * 24 * 4));
        int total = 0;
        for (const auto& row : dropped)
            total += row[0].toInt();
        QCOMPARE(total, byteLimit ? 1 : 2);
        gate.release(100);
        QTRY_COMPARE(queue.pendingCount(), 0);
        QCOMPARE(saved[1][0].toString(), byteLimit ? QStringLiteral("2") : QStringLiteral("3"));
        QVERIFY(!onGui.load());
    }
    void retryAndDirectoryRecovery()
    {
        QTemporaryDir temp;
        const auto directory = temp.filePath(QStringLiteral("shots"));
        QuickGunnerSaveQueue queue(AppSettings(temp.filePath(QStringLiteral("settings.ini"))));
        queue.setDirectory(directory);
        QSignalSpy failed(&queue, &QuickGunnerSaveQueue::failed);
        QSignalSpy saved(&queue, &QuickGunnerSaveQueue::saved);
        queue.enqueue(picture());
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(queue.pendingCount(), 1);
        QCOMPARE(queue.retryCount(), 1);
        queue.enqueue(picture(Qt::green));
        QTRY_COMPARE(failed.size(), 2);
        QCOMPARE(queue.pendingCount(), 2);
        QVERIFY(QDir().mkdir(directory));
        // 不调用 retryNow，验证 2 秒定时恢复。
        QTRY_COMPARE_WITH_TIMEOUT(saved.size(), 2, 5000);
        QCOMPARE(queue.pendingCount(), 0);
        QVERIFY(QDir(directory).removeRecursively());
        queue.enqueue(picture(Qt::blue));
        QTRY_COMPARE(failed.size(), 3);
        const auto other = temp.filePath(QStringLiteral("new-shots"));
        QVERIFY(QDir().mkdir(other));
        queue.setDirectory(other);
        queue.retryNow();
        QTRY_COMPARE(saved.size(), 3);
        QVERIFY(saved.last()[0].toString().startsWith(other));
    }
    void retryBoundedAndOversized()
    {
        QTemporaryDir temp;
        QuickGunnerSaveQueue queue(AppSettings(temp.filePath(QStringLiteral("settings.ini"))));
        queue.setDirectory(temp.filePath(QStringLiteral("missing")));
        QSignalSpy failed(&queue, &QuickGunnerSaveQueue::failed);
        QSignalSpy dropped(&queue, &QuickGunnerSaveQueue::dropped);
        for (int i = 0; i < 10; ++i)
        {
            queue.enqueue(picture(QColor(i, 0, 0)));
            QTRY_COMPARE(failed.size(), i + 1);
        }
        QCOMPARE(queue.pendingCount(), 8);
        QCOMPARE(queue.retryCount(), 8);
        QCOMPARE(dropped.size(), 2);
        QVERIFY(QDir().mkdir(temp.filePath(QStringLiteral("missing"))));
        QSignalSpy saved(&queue, &QuickGunnerSaveQueue::saved);
        queue.retryNow();
        QTRY_COMPARE(saved.size(), 8);
        QCOMPARE(QImage(saved.first()[0].toString()).pixelColor(0, 0).red(), 2);
        queue.enqueue(picture(Qt::red, {8193, 8192}));
        QCOMPARE(queue.pendingCount(), 0);
        QCOMPARE(dropped.size(), 3);
    }
};
QTEST_GUILESS_MAIN(QuickGunnerSaveQueueTest)
#include "quick_gunner_save_queue_test.moc"
