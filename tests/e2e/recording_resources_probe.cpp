#include "app/history_manager.h"
#include "interfaces/file_manager.h"
#include "ui/history_window.h"
#include "ui/recording_resource_source.h"
#include <QApplication>
#include <QComboBox>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QListWidget>
#include <QMovie>
#include <QScreen>
#include <QTest>
#include <QTextStream>
#include <QToolButton>
using namespace waibusnap;
namespace
{
bool waitFor(const std::function<bool()>& condition)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!condition() && elapsed.elapsed() < 15000)
        QTest::qWait(20);
    return condition();
}
}
// 仅操作指定成品的临时副本；真实桌面、外部播放器与文件管理器需人工另行核对。
int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOptions(
        {{QStringLiteral("video"), QStringLiteral("已有 MP4 成品"), QStringLiteral("path")},
         {QStringLiteral("gif"), QStringLiteral("已有 GIF 成品"), QStringLiteral("path")},
         {QStringLiteral("output"), QStringLiteral("新的隔离取证目录"), QStringLiteral("path")},
         {QStringLiteral("screen"), QStringLiteral("目标显示器序号"), QStringLiteral("index"), "0"},
         {QStringLiteral("large-count"), QStringLiteral("额外大目录副本数量"),
          QStringLiteral("count"), QStringLiteral("0")},
         {QStringLiteral("open-external"),
          QStringLiteral("调用系统播放器、Finder / Explorer 与回收站")}});
    parser.process(app);
    const QString root = parser.value(QStringLiteral("output"));
    QTextStream out(stdout);
    if (!QDir::isAbsolutePath(root) || QFileInfo::exists(root) || !QDir().mkpath(root))
    {
        out << "FAIL 取证目录必须为新的绝对目录\n";
        return 1;
    }
    int failures = 0;
    const auto check = [&](bool ok, const QString& description)
    {
        out << (ok ? "PASS " : "FAIL ") << description << Qt::endl;
        failures += int(!ok);
        return ok;
    };
    const QString videos = QDir(root).filePath(QStringLiteral("视频"));
    const QString gifs = QDir(root).filePath(QStringLiteral("GIF"));
    QDir().mkpath(videos);
    QDir().mkpath(gifs);
    const QString video = QDir(videos).filePath(QStringLiteral("受控录制.mp4"));
    const QString gif = QDir(gifs).filePath(QStringLiteral("受控动画.gif"));
    if (!check(QFile::copy(parser.value(QStringLiteral("video")), video) &&
                   QFile::copy(parser.value(QStringLiteral("gif")), gif),
               QStringLiteral("复制真实成品至隔离目录")))
        return 1;
    AppSettings settings(QDir(root).filePath(QStringLiteral("settings.ini")));
    HistoryManager history(settings, QDir(root).filePath(QStringLiteral("历史未启用")));
    const auto manager = createFileManager();
    HistoryWindowActions actions;
    actions.reveal = [&](const QString& path)
    {
        const auto result = manager->revealInFolder(path);
        return HistoryStatus{result.success, result.error};
    };
    actions.trash = [&](const QString& path)
    {
        const auto result = manager->moveToTrash(path);
        return HistoryStatus{result.success, result.error};
    };
    const std::shared_ptr<RecordingMediaProbe> probe = createRecordingMediaProbe();
    RecordingResourceSource videoSource(
        RecordingOutput::Video, [&] { return videos; }, settings.recordingVideoStarsPath(),
        settings.recordingVideoMetaPath(), probe, actions);
    RecordingResourceSource gifSource(
        RecordingOutput::Gif, [&] { return gifs; }, settings.recordingGifStarsPath(),
        settings.recordingGifMetaPath(), probe, actions);
    auto* window = new HistoryWindow(history, settings, actions,
                                     QVector<ResourceSource*>{&videoSource, &gifSource}, false);
    const int screenIndex = parser.value(QStringLiteral("screen")).toInt();
    const auto screens = app.screens();
    if (screenIndex >= 0 && screenIndex < screens.size())
        window->move(screens[screenIndex]->availableGeometry().topLeft() + QPoint(30, 30));
    window->show();
    window->raise();
    auto* view = window->findChild<HistoryImageView*>();
    auto* list = window->findChild<QListWidget*>(QStringLiteral("historyThumbnails"));
    window->selectSource(QStringLiteral("recording-video"));
    check(waitFor(
              [&] {
                  return videoSource.snapshot().items.first().durationMs > 0 &&
                         !view->image().isNull();
              }),
          QStringLiteral("真实视频元数据与首帧"));
    const auto info = videoSource.snapshot().items.first();
    out << "视频 " << info.durationMs << "ms " << info.pixelSize.width() << 'x'
        << info.pixelSize.height() << ' ' << info.bytes << "B DPR=" << window->devicePixelRatioF()
        << Qt::endl;
    check(window->grab().save(QDir(root).filePath(QStringLiteral("video.png"))),
          QStringLiteral("保存视频来源原生窗口图"));
    check(videoSource.setStarred(video, true).success, QStringLiteral("真实目录星标写入"));
    {
        RecordingResourceSource restored(
            RecordingOutput::Video, [&] { return videos; }, settings.recordingVideoStarsPath(),
            settings.recordingVideoMetaPath(), probe, actions);
        check(restored.snapshot().items.first().starred, QStringLiteral("新来源实例重读星标"));
    }
    window->findChild<QComboBox*>(QStringLiteral("historyGroupCombo"))->setCurrentIndex(1);
    check(list->count() == 1, QStringLiteral("星标筛选"));
    window->findChild<QComboBox*>(QStringLiteral("historySortCombo"))->setCurrentIndex(1);
    check(list->count() == 1, QStringLiteral("切换排序"));
    window->findChild<QComboBox*>(QStringLiteral("historyGroupCombo"))->setCurrentIndex(0);
    if (parser.isSet(QStringLiteral("open-external")))
    {
        check(videoSource.revealItem(video).success, QStringLiteral("真实文件管理器定位"));
        QTest::mouseClick(list->viewport(), Qt::LeftButton, {},
                          list->visualItemRect(list->currentItem()).center());
        QTest::mouseDClick(list->viewport(), Qt::LeftButton, {},
                           list->visualItemRect(list->currentItem()).center());
        out << "INFO 已投递视频列表真实双击，外部播放器窗口请人工核对\n";
        QTest::qWait(1000);
    }
    window->selectSource(QStringLiteral("recording-gif"));
    check(waitFor([&] { return window->findChild<QMovie*>(); }),
          QStringLiteral("真实 GIF 创建 QMovie"));
    auto* movie = window->findChild<QMovie*>();
    if (movie)
    {
        const int first = movie->currentFrameNumber();
        view->actualSize();
        check(waitFor([&] { return movie->currentFrameNumber() != first; }),
              QStringLiteral("定时播放推进帧号"));
        check(view->zoom() == 1, QStringLiteral("动画保留缩放"));
        check(movie->cacheMode() == QMovie::CacheNone, QStringLiteral("动画无整段缓存"));
    }
    check(window->grab().save(QDir(root).filePath(QStringLiteral("gif.png"))),
          QStringLiteral("保存 GIF 来源原生窗口图"));
    window->resize(480, 640);
    QTest::qWait(100);
    check(window->findChild<QComboBox*>(QStringLiteral("resourceSourceCombo"))->isVisible(),
          QStringLiteral("窄窗来源可达"));
    check(window->grab().save(QDir(root).filePath(QStringLiteral("narrow.png"))),
          QStringLiteral("保存窄窗原生窗口图"));
    const auto extra = QDir(gifs).filePath(QStringLiteral("外部新增.gif"));
    check(QFile::copy(gif, extra) && waitFor([&] { return list->count() == 2; }),
          QStringLiteral("文件管理器式外部新增实时刷新"));
    check(QFile::remove(extra) && waitFor([&] { return list->count() == 1; }),
          QStringLiteral("外部删除实时刷新"));
    check(!QFileInfo::exists(history.rootDirectory()), QStringLiteral("历史关闭不初始化历史目录"));
    window->selectSource(QStringLiteral("recording-video"));
    check(!window->findChild<QMovie*>(), QStringLiteral("切换来源释放动画"));
    window->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    if (parser.isSet(QStringLiteral("open-external")))
        check(gifSource.deleteItem(gif).success && !QFileInfo::exists(gif),
              QStringLiteral("真实系统回收站删除隔离 GIF 副本"));
    const int largeCount = parser.value(QStringLiteral("large-count")).toInt();
    if (largeCount > 0 && largeCount <= 1000)
    {
        const auto largeDirectory = QDir(root).filePath(QStringLiteral("大目录"));
        QDir().mkpath(largeDirectory);
        for (int i = 0; i < largeCount; ++i)
            if (!QFile::copy(video, QDir(largeDirectory).filePath(QStringLiteral("%1.mp4").arg(i))))
                return 1;
        QElapsedTimer scan;
        scan.start();
        RecordingResourceSource largeSource(
            RecordingOutput::Video, [&] { return largeDirectory; },
            QDir(root).filePath(QStringLiteral("large-stars.json")),
            QDir(root).filePath(QStringLiteral("large-meta.json")), probe, actions);
        const auto scanMs = scan.elapsed();
        check(largeSource.snapshot().items.size() == largeCount && scanMs < 1000,
              QStringLiteral("大目录 %1 项先出条目，扫描 %2 ms").arg(largeCount).arg(scanMs));
        bool ticked = false;
        QTimer::singleShot(0, &app, [&] { ticked = true; });
        check(waitFor([&] { return ticked; }), QStringLiteral("大目录后台探测期间 GUI 事件仍推进"));
    }
    out << "RESULT failures=" << failures << Qt::endl;
    return failures ? 1 : 0;
}
