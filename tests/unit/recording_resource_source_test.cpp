#include "../fixtures/recording_resource_fakes.h"
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>
#include <filesystem>
using namespace waibusnap;
using namespace waibusnap::test;
namespace
{
struct Fixture
{
    QTemporaryDir temp;
    QString directory = temp.filePath(QStringLiteral("录制"));
    QString stars = temp.filePath(QStringLiteral("配置/stars.json"));
    QString meta = temp.filePath(QStringLiteral("配置/meta.json"));
    std::shared_ptr<FakeMediaProbe> probe = std::make_shared<FakeMediaProbe>();
    QString revealed, opened, trashed;
    HistoryWindowActions actions;
    Fixture()
    {
        QDir().mkpath(directory);
        actions.reveal = [this](const QString& path)
        {
            revealed = path;
            return HistoryStatus{true, {}};
        };
        actions.trash = [this](const QString& path)
        {
            trashed = path;
            return HistoryStatus{QFile::remove(path), {}};
        };
        actions.openExternal = [this](const QString& path)
        {
            opened = path;
            return ResourceStatus{true, {}};
        };
    }
    std::unique_ptr<RecordingResourceSource> source(RecordingOutput kind = RecordingOutput::Video,
                                                    bool watch = false)
    {
        return std::make_unique<RecordingResourceSource>(
            kind, [this] { return directory; }, stars, meta, probe, actions, nullptr, watch);
    }
};
QJsonDocument json(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll());
}
}
class RecordingResourceSourceTest final : public QObject
{
    Q_OBJECT
  private slots:
    void scanTypesAndCapabilities_data()
    {
        QTest::addColumn<bool>("gif");
        QTest::newRow("video") << false;
        QTest::newRow("gif") << true;
    }
    void scanTypesAndCapabilities()
    {
        QFETCH(bool, gif);
        Fixture f;
        const auto video = writeMedia(f.directory, QStringLiteral("视频.MP4"));
        const auto animation = writeGif(f.directory);
        writeMedia(f.directory, QStringLiteral(".hidden.mp4"));
        writeMedia(f.directory, QStringLiteral(".hidden.gif"));
        writeMedia(f.directory, QStringLiteral("video.mp4.part"));
        writeMedia(f.directory, QStringLiteral("animation.gif.part"));
        writeMedia(f.directory, QStringLiteral("readme.txt"));
        writeMedia(QDir(f.directory).filePath(QStringLiteral("子目录")),
                   QStringLiteral("child.mp4"));
        auto source = f.source(gif ? RecordingOutput::Gif : RecordingOutput::Video);
        const auto snapshot = source->snapshot();
        QCOMPARE(snapshot.items.size(), 1);
        const auto item = snapshot.items.first();
        QCOMPARE(item.id, gif ? animation : video);
        QCOMPARE(item.absolutePath, item.id);
        QCOMPARE(item.bytes, QFileInfo(item.id).size());
        QCOMPARE(item.createdAtMs, QFileInfo(item.id).lastModified().toMSecsSinceEpoch());
        QCOMPARE(item.preview, gif ? ResourcePreview::Gif : ResourcePreview::Video);
        QCOMPARE(item.badges, QStringList{gif ? QStringLiteral("GIF") : QStringLiteral("视频")});
        QVERIFY(item.deletable && !item.hasOriginal && !item.managed);
        QCOMPARE(source->id(),
                 gif ? QStringLiteral("recording-gif") : QStringLiteral("recording-video"));
        QCOMPARE(source->displayName(),
                 gif ? QStringLiteral("录制 GIF") : QStringLiteral("录制视频"));
        QVERIFY(source->visible());
        const auto caps = source->capabilities();
        QVERIFY(caps.star && caps.deleteItem && caps.reveal);
        QCOMPARE(caps.openExternal, !gif);
        QVERIFY(!caps.groups && !caps.cleanup && !caps.originalVariant && !caps.clearAll &&
                !caps.configureDirectory);
        QVERIFY(!source->canCopy() && !source->canPin());
        QVERIFY(!source->clearAll().success);
        QVERIFY(!source->outputImage(item.id, false, false).success);
        QVERIFY(!source->loadImage(item.id, {}, true).success);
    }
    void linksAndPathBoundaries()
    {
        Fixture f;
        const auto inside = writeMedia(f.directory, QStringLiteral("a.mp4"));
        const auto outside = writeMedia(f.temp.path(), QStringLiteral("outside.mp4"));
        const QString linked = QDir(f.directory).filePath(QStringLiteral("linked.mp4"));
        std::error_code error;
        std::filesystem::create_symlink(std::filesystem::u8path(outside.toUtf8().constData()),
                                        std::filesystem::u8path(linked.toUtf8().constData()),
                                        error);
        auto source = f.source();
        QCOMPARE(source->snapshot().items.size(), 1);
        QVERIFY(!source->setStarred(outside, true).success);
        QVERIFY(!source->deleteItem(outside).success);
        QVERIFY(!source->revealItem(outside).success);
        QVERIFY(!source->openExternal(outside).success);
        QVERIFY(!source->deleteItem(QDir(f.directory).filePath(QStringLiteral("../outside.mp4")))
                     .success);
        QVERIFY(!source->loadImage(linked).success);
        if (!error)
            QVERIFY(QFileInfo(linked).isSymLink());
        const auto nested = writeMedia(QDir(f.directory).filePath(QStringLiteral("child")),
                                       QStringLiteral("nested.mp4"));
        QVERIFY(!source->deleteItem(nested).success);
        QVERIFY(source->revealItem(inside).success);
        QCOMPARE(f.revealed, inside);
        QVERIFY(source->openExternal(inside).success);
        QCOMPARE(f.opened, inside);
        f.directory = f.temp.filePath(QStringLiteral("新目录"));
        QVERIFY(!source->openExternal(inside).success);
        QVERIFY(!source->setStarred(inside, true).success);
        QVERIFY(QFileInfo::exists(inside));
    }
    void actionsRevalidateReplacementAndFailure()
    {
        Fixture f;
        const auto path = writeMedia(f.directory, QStringLiteral("a.mp4"));
        auto source = f.source();
        QVERIFY(source->deleteItem(path).success);
        QCOMPARE(f.trashed, path);
        QVERIFY(source->snapshot().items.isEmpty());
        QVERIFY(!source->deleteItem(path).success);
        writeGif(f.directory);
        auto gif = f.source(RecordingOutput::Gif);
        QVERIFY(!gif->openExternal(gif->snapshot().items.first().id).success);
        QVERIFY(f.opened.isEmpty());
        RecordingResourceSource noActions(
            RecordingOutput::Video, [&] { return f.directory; }, f.stars, f.meta, f.probe);
        QVERIFY(!noActions.capabilities().deleteItem && !noActions.capabilities().reveal);
    }
    void metadataCacheAndInvalidation()
    {
        Fixture f;
        const auto path = writeMedia(f.directory, QStringLiteral("a.mp4"));
        auto source = f.source();
        QCOMPARE(f.probe->probes.load(), 0);
        QTRY_COMPARE(source->snapshot().items.first().durationMs, 12500);
        QTRY_VERIFY(json(f.meta).isObject());
        QCOMPARE(f.probe->probes.load(), 1);
        QCOMPARE(source->snapshot().items.first().pixelSize, QSize(640, 360));
        source.reset();
        source = f.source();
        QCOMPARE(source->snapshot().items.first().durationMs, 12500);
        QTest::qWait(80);
        QCOMPARE(f.probe->probes.load(), 1);
        writeMedia(f.directory, QStringLiteral("a.mp4"), "longer replacement");
        source->refresh();
        QCOMPARE(source->snapshot().items.first().durationMs, 0);
        QTRY_COMPARE(f.probe->probes.load(), 2);
        QTRY_COMPARE(source->snapshot().items.first().durationMs, 12500);
        QFile changed(path);
        QVERIFY(changed.open(QIODevice::ReadWrite));
        QVERIFY(changed.setFileTime(QDateTime::currentDateTime().addSecs(5),
                                    QFileDevice::FileModificationTime));
        changed.close();
        source->refresh();
        QTRY_COMPARE(f.probe->probes.load(), 3);
        QTRY_COMPARE(source->snapshot().items.first().durationMs, 12500);
        QVERIFY(QFile::remove(path));
        source->refresh();
        QTRY_VERIFY(json(f.meta).object().value(QStringLiteral("entries")).toObject().isEmpty());
    }
    void corruptionAndWriteFailure()
    {
        Fixture f;
        writeMedia(QFileInfo(f.meta).absolutePath(), QFileInfo(f.meta).fileName(), "broken");
        writeMedia(QFileInfo(f.stars).absolutePath(), QFileInfo(f.stars).fileName(), "broken");
        const auto path = writeMedia(f.directory, QStringLiteral("a.mp4"));
        auto source = f.source();
        QVERIFY(source->snapshot().notice.contains(QStringLiteral("损坏")));
        QVERIFY(!source->snapshot().items.first().starred);
        QVERIFY(source->setStarred(path, true).success);
        QVERIFY(QFile::rename(f.stars, f.stars + QStringLiteral(".backup")));
        QVERIFY(QDir().mkpath(f.stars));
        QVERIFY(!source->setStarred(path, false).success);
        source->refresh();
        QVERIFY(source->snapshot().items.first().starred);
        QVERIFY(QDir().rmdir(f.stars));
        QVERIFY(QFile::rename(f.stars + QStringLiteral(".backup"), f.stars));
        QTRY_COMPARE(source->snapshot().items.first().durationMs, 12500);
        QTRY_VERIFY(json(f.meta).isObject());
        QCOMPARE(json(f.stars).array().size(), 1);
    }
    void starsRoundTripAndPrune()
    {
        Fixture f;
        const auto path = writeMedia(f.directory, QStringLiteral("a.mp4"));
        auto source = f.source();
        QVERIFY(source->setStarred(path, true).success);
        source.reset();
        source = f.source();
        QVERIFY(source->snapshot().items.first().starred);
        QVERIFY(QFile::remove(path));
        source.reset();
        source = f.source();
        QVERIFY(json(f.stars).array().isEmpty());
    }
    void staleInFlightMetadataAndAtomicWriteFailure()
    {
        Fixture f;
        const auto path = writeMedia(f.directory, QStringLiteral("a.mp4"));
        auto source = f.source();
        QTRY_COMPARE(source->snapshot().items.first().durationMs, 12500);
        QTRY_VERIFY(json(f.meta).isObject());
        QFile previous(f.meta);
        QVERIFY(previous.open(QIODevice::ReadOnly));
        const auto oldBytes = previous.readAll();
        previous.close();
        QVERIFY(QFile::rename(f.meta, f.meta + QStringLiteral(".backup")));
        QVERIFY(QDir().mkpath(f.meta));
        f.probe->delayMs = 80;
        writeMedia(f.directory, QStringLiteral("a.mp4"), "second version");
        source->refresh();
        QTRY_COMPARE(f.probe->probes.load(), 2);
        writeMedia(f.directory, QStringLiteral("a.mp4"), "third version is longer");
        source->refresh();
        QTRY_COMPARE(f.probe->probes.load(), 3);
        QTRY_COMPARE(source->snapshot().items.first().durationMs, 12500);
        QTRY_VERIFY(source->snapshot().notice.contains(QStringLiteral("缓存保存失败")));
        QFile preserved(f.meta + QStringLiteral(".backup"));
        QVERIFY(preserved.open(QIODevice::ReadOnly));
        QCOMPARE(preserved.readAll(), oldBytes);
        preserved.close();
        QVERIFY(QDir().rmdir(f.meta));
        QVERIFY(QFile::rename(f.meta + QStringLiteral(".backup"), f.meta));
        source.reset();
        source = f.source();
        QTRY_COMPARE(f.probe->probes.load(), 4);
        QTRY_COMPARE(json(f.meta)
                         .object()
                         .value(QStringLiteral("entries"))
                         .toObject()
                         .value(path)
                         .toObject()
                         .value(QStringLiteral("size"))
                         .toInteger(),
                     QFileInfo(path).size());
    }
    void replacedSymlinkAndShortcutAreRejected()
    {
        Fixture f;
        const auto outside = writeMedia(f.temp.path(), QStringLiteral("outside.mp4"));
        const auto path = writeMedia(f.directory, QStringLiteral("a.mp4"));
        auto source = f.source();
        QVERIFY(QFile::remove(path));
        // Windows 上 QFile::link 写快捷方式，macOS 写符号链接；两者都必须拒绝。
        const auto shortcut = QDir(f.directory).filePath(QStringLiteral("shortcut.mp4"));
        QVERIFY(QFile::link(outside, shortcut));
        source->refresh();
        QVERIFY(source->snapshot().items.isEmpty());
        QVERIFY(!source->deleteItem(shortcut).success);
        QVERIFY(!source->openExternal(shortcut).success);
        QVERIFY(!source->revealItem(shortcut).success);
        QVERIFY(QFileInfo::exists(outside));
    }
    void destructionDuringProbeDropsDelivery()
    {
        Fixture f;
        f.probe->delayMs = 80;
        writeMedia(f.directory, QStringLiteral("a.mp4"));
        auto source = f.source();
        QTRY_COMPARE(f.probe->probes.load(), 1);
        source.reset();
        QCoreApplication::sendPostedEvents();
        QTest::qWait(30);
        QCOMPARE(f.probe->running.load(), 0);
    }
    void boundedAsyncProbeAndLazyThumbnails()
    {
        Fixture f;
        f.probe->delayMs = 30;
        for (int i = 0; i < 80; ++i)
            writeMedia(f.directory, QStringLiteral("%1.mp4").arg(i));
        QElapsedTimer elapsed;
        elapsed.start();
        auto source = f.source();
        QVERIFY(elapsed.elapsed() < 1000);
        QCOMPARE(source->snapshot().items.size(), 80);
        QCOMPARE(f.probe->probes.load(), 0);
        QCOMPARE(f.probe->thumbnails.load(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(f.probe->probes.load(), 80, 5000);
        QVERIFY(f.probe->peak.load() <= 2);
        const auto id = source->snapshot().items.first().id;
        QVERIFY(!source->loadImage(id, {96, 72}).success);
        QTRY_VERIFY(source->loadImage(id, {96, 72}).success);
        QCOMPARE(f.probe->thumbnails.load(), 1);
        QCOMPARE(source->loadImage(id, {96, 72}).image.size(), QSize(96, 54));
        QTRY_VERIFY(source->loadImage(id).success);
        QCOMPARE(source->loadImage(id).image.size(), QSize(640, 360));
        for (const auto& item : source->snapshot().items)
            source->loadImage(item.id, {96, 72});
        QTRY_COMPARE(f.probe->thumbnails.load(), 81);
        QTest::qWait(100);
        source->loadImage(id, {96, 72});
        QTRY_COMPARE(f.probe->thumbnails.load(), 82);
    }
    void failedProbeKeepsFileAndDoesNotRetryLoop()
    {
        Fixture f;
        f.probe->fail = true;
        const auto path = writeMedia(f.directory, QStringLiteral("bad.mp4"));
        auto source = f.source();
        QTRY_VERIFY(!source->snapshot().items.first().mediaError.isEmpty());
        source->refresh();
        QTest::qWait(100);
        QCOMPARE(f.probe->probes.load(), 1);
        QCOMPARE(source->snapshot().items.size(), 1);
        QVERIFY(QFileInfo::exists(path));
        QVERIFY(source->revealItem(path).success);
    }
    void realGifMetadataAndFirstFrame()
    {
        Fixture f;
        const auto path = writeGif(f.directory);
        const auto probe = createRecordingMediaProbe();
        const auto info = probe->probe(path);
        QVERIFY2(info.valid, qPrintable(info.error));
        QCOMPARE(info.pixelSize, QSize(48, 32));
        QCOMPARE(info.frameCount, 3);
        QCOMPARE(info.durationMs, 300);
        QCOMPARE(info.loopCount, -1);
        QCOMPARE(probe->thumbnail(path, {24, 24}).size(), QSize(24, 16));
        QCOMPARE(probe->thumbnail(path, {}).size(), QSize(48, 32));
        auto source = f.source(RecordingOutput::Gif);
        QTRY_COMPARE(source->snapshot().items.first().durationMs, 300);
        QTRY_VERIFY(source->loadImage(path, {24, 24}).success);
        QCOMPARE(source->loadImage(path, {24, 24}).image.size(), QSize(24, 16));
        QVERIFY(source->loadImage(path, {24, 24}).image.pixelColor(0, 0).red() > 200);
    }
    void invalidDirectoryAndActiveMonitoring()
    {
        Fixture f;
        auto source = f.source(RecordingOutput::Video, true);
        source->setActive(true);
        writeMedia(f.directory, QStringLiteral("live.mp4"));
        QTRY_COMPARE_WITH_TIMEOUT(source->snapshot().items.size(), 1, 4000);
        source->setActive(false);
        writeMedia(f.directory, QStringLiteral("later.mp4"));
        QTest::qWait(250);
        QCOMPARE(source->snapshot().items.size(), 1);
        source->setActive(true);
        QCOMPARE(source->snapshot().items.size(), 2);
        f.directory = f.temp.filePath(QStringLiteral("不存在"));
        source->directoryChanged();
        QVERIFY(source->visible());
        QVERIFY(source->snapshot().error.contains(QStringLiteral("设置 → 录制")));
        QVERIFY(!QFileInfo::exists(f.directory));
    }
    void settingsPathsAreIsolated()
    {
        QTemporaryDir temp;
        AppSettings settings(temp.filePath(QStringLiteral("配置/settings.ini")));
        for (const auto& path :
             {settings.recordingVideoStarsPath(), settings.recordingGifStarsPath(),
              settings.recordingVideoMetaPath(), settings.recordingGifMetaPath()})
            QCOMPARE(QFileInfo(path).absolutePath(), temp.filePath(QStringLiteral("配置")));
    }
};
QTEST_MAIN(RecordingResourceSourceTest)
#include "recording_resource_source_test.moc"
