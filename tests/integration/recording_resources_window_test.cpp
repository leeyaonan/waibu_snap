#include "../fixtures/recording_resource_fakes.h"
#include "app/application_controller.h"
#include "ui/history_window.h"
#include "ui/quick_gunner_resource_source.h"
#include <QComboBox>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QMovie>
#include <QPushButton>
#include <QSignalSpy>
#include <QSplitter>
#include <QTest>
#include <QToolButton>
using namespace waibusnap;
using namespace waibusnap::test;
namespace waibusnap
{
struct ApplicationControllerTestAccess
{
    static HistoryWindow* window(ApplicationController& c) { return c.historyWindow_; }
    static void settingsOpen(ApplicationController& c, bool open)
    {
        c.settingsOpen_ = open;
        c.refreshRecordingActions();
    }
    static void finish(ApplicationController& c) { emit c.recording_->finished({}); }
    static QStringList reserved(ApplicationController& c)
    {
        return c.reservedQuickGunnerDirectories();
    }
};
}
namespace
{
template <class T> T* child(QObject* root, const char* name)
{
    auto* found = root->findChild<T*>(QLatin1String(name));
    if (!found)
        qFatal("缺少录制资源测试控件：%s", name);
    return found;
}
struct Fixture
{
    QTemporaryDir temp;
    AppSettings settings{temp.filePath(QStringLiteral("settings.ini"))};
    HistoryManager history{settings, temp.filePath(QStringLiteral("历史"))};
    QString videos = temp.filePath(QStringLiteral("视频"));
    QString gifs = temp.filePath(QStringLiteral("GIF"));
    QString quick = temp.filePath(QStringLiteral("快枪手"));
    std::shared_ptr<FakeMediaProbe> probe = std::make_shared<FakeMediaProbe>();
    HistoryWindowActions actions;
    std::unique_ptr<QuickGunnerResourceSource> quickSource;
    std::unique_ptr<RecordingResourceSource> videoSource, gifSource;
    QPointer<HistoryWindow> window;
    QString opened, revealed, trashed;
    Fixture()
    {
        QDir().mkpath(videos);
        QDir().mkpath(gifs);
        QDir().mkpath(quick);
        writeMedia(videos, QStringLiteral("a.mp4"));
        writeMedia(videos, QStringLiteral("b.mp4"));
        writeGif(gifs);
        actions.openExternal = [this](const QString& path)
        {
            opened = path;
            return ResourceStatus{true, {}};
        };
        actions.trash = [this](const QString& path)
        {
            trashed = path;
            return HistoryStatus{QFile::remove(path), {}};
        };
        actions.reveal = [this](const QString& path)
        {
            revealed = path;
            return HistoryStatus{true, {}};
        };
        quickSource = std::make_unique<QuickGunnerResourceSource>(
            [this] { return quick; }, settings.quickGunnerStarsPath(), actions, nullptr, false);
        videoSource = std::make_unique<RecordingResourceSource>(
            RecordingOutput::Video, [this] { return videos; }, settings.recordingVideoStarsPath(),
            settings.recordingVideoMetaPath(), probe, actions, nullptr, false);
        gifSource = std::make_unique<RecordingResourceSource>(
            RecordingOutput::Gif, [this] { return gifs; }, settings.recordingGifStarsPath(),
            settings.recordingGifMetaPath(), probe, actions, nullptr, false);
    }
    ~Fixture() { delete window.data(); }
    HistoryWindow* open(bool enabled = true)
    {
        if (enabled && !history.initialize().success)
            qFatal("临时历史初始化失败。");
        window = new HistoryWindow(
            history, settings, actions,
            QVector<ResourceSource*>{quickSource.get(), videoSource.get(), gifSource.get()},
            enabled);
        window->show();
        QCoreApplication::processEvents();
        return window;
    }
    void close()
    {
        window->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
};
void answerDelete(HistoryWindow* window, QMessageBox::StandardButton answer, bool* correct)
{
    auto* timer = new QTimer(window);
    timer->setInterval(10);
    QObject::connect(timer, &QTimer::timeout, window,
                     [window, timer, answer, correct]
                     {
                         auto* dialog = window->findChild<QMessageBox*>(
                             QStringLiteral("recordingDeleteConfirm"));
                         if (!dialog)
                             return;
                         *correct =
                             dialog->defaultButton() == dialog->button(QMessageBox::Cancel) &&
                             (dialog->text().contains(QStringLiteral("这段录制")) ||
                              dialog->text().contains(QStringLiteral("这个 GIF")));
                         timer->stop();
                         timer->deleteLater();
                         dialog->done(answer);
                     });
    timer->start();
}
}
class RecordingResourcesWindowTest final : public QObject
{
    Q_OBJECT
  private slots:
    void initTestCase() { QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs); }
    void fourSourcesAndIndependentViews()
    {
        Fixture f;
        auto* w = f.open();
        auto* combo = child<QComboBox>(w, "resourceSourceCombo");
        QCOMPARE(combo->count(), 4);
        const QStringList ids{QStringLiteral("history"), QStringLiteral("quick-gunner"),
                              QStringLiteral("recording-video"), QStringLiteral("recording-gif")};
        for (int i = 0; i < 4; ++i)
            QCOMPARE(combo->itemData(i).toString(), ids[i]);
        QCOMPARE(w->selectedSourceId(), QStringLiteral("history"));
        const auto current = f.history.currentGroupId();
        w->selectSource(QStringLiteral("recording-video"));
        auto* list = child<QListWidget>(w, "historyThumbnails");
        QCOMPARE(list->count(), 2);
        child<QComboBox>(w, "historySortCombo")->setCurrentIndex(1);
        list->setCurrentRow(1);
        const auto selected = w->selectedItemId();
        w->selectSource(QStringLiteral("recording-gif"));
        QCOMPARE(list->count(), 1);
        w->selectSource(QStringLiteral("recording-video"));
        QCOMPARE(w->selectedItemId(), selected);
        QCOMPARE(child<QComboBox>(w, "historySortCombo")->currentIndex(), 1);
        QCOMPARE(f.history.currentGroupId(), current);
    }
    void disabledHistoryLastSourceAndDestroyedFallback()
    {
        Fixture f;
        HistoryWindowState state;
        state.lastSource = QStringLiteral("history");
        QVERIFY(f.settings.saveHistoryWindowState(state).isEmpty());
        auto* w = f.open(false);
        auto* combo = child<QComboBox>(w, "resourceSourceCombo");
        QCOMPARE(combo->count(), 3);
        QCOMPARE(w->selectedSourceId(), QStringLiteral("quick-gunner"));
        QVERIFY(!QFileInfo::exists(f.temp.filePath(QStringLiteral("历史"))));
        f.quickSource.reset();
        QCOMPARE(combo->count(), 2);
        QCOMPARE(w->selectedSourceId(), QStringLiteral("recording-video"));
        f.videoSource.reset();
        QCOMPARE(combo->count(), 1);
        QCOMPARE(w->selectedSourceId(), QStringLiteral("recording-gif"));
        f.close();
        state.lastSource = QStringLiteral("已失效来源");
        QVERIFY(f.settings.saveHistoryWindowState(state).isEmpty());
        // 新 Fixture 的来源补全后，未知来源仍回退首个可见项。
        Fixture fresh;
        QVERIFY(fresh.settings.saveHistoryWindowState(state).isEmpty());
        QCOMPARE(fresh.open()->selectedSourceId(), QStringLiteral("history"));
    }
    void videoDetailsPreviewAndExternalOpen()
    {
        Fixture f;
        auto* w = f.open();
        w->selectSource(QStringLiteral("recording-video"));
        auto* list = child<QListWidget>(w, "historyThumbnails");
        QTRY_VERIFY(list->item(0)->text().contains(QStringLiteral("640×360")));
        QVERIFY(list->item(0)->text().contains(QStringLiteral("00:12")));
        QVERIFY(list->item(0)->text().contains(QStringLiteral("MiB")));
        auto* view = child<HistoryImageView>(w, "historyImageView");
        QTRY_VERIFY(!view->image().isNull());
        QTRY_VERIFY(!list->item(0)->icon().isNull());
        QVERIFY(view->toolTip().contains(QStringLiteral("双击")));
        QTest::mouseClick(list->viewport(), Qt::LeftButton, {},
                          list->visualItemRect(list->currentItem()).center());
        QTest::mouseDClick(list->viewport(), Qt::LeftButton, {},
                           list->visualItemRect(list->currentItem()).center());
        QTRY_COMPARE(f.opened, w->selectedItemId());
        f.opened.clear();
        QTest::mouseDClick(view->viewport(), Qt::LeftButton);
        QCOMPARE(f.opened, w->selectedItemId());
        for (const auto* name : {"historyCopy", "historyPin", "historyOriginalToggle",
                                 "resourceClearAll", "resourceChooseDirectory"})
        {
            const auto* button = child<QToolButton>(w, name);
            QVERIFY(!button->isVisible() || !button->isEnabled());
        }
        const auto before = w->isVisible();
        QTest::keyClick(w, Qt::Key_Return);
        QCOMPARE(w->isVisible(), before);
    }
    void gifAnimatesPreservesZoomAndStops()
    {
        Fixture f;
        auto* w = f.open(false);
        w->selectSource(QStringLiteral("recording-gif"));
        QTRY_VERIFY(w->findChild<QMovie*>());
        auto* movie = w->findChild<QMovie*>();
        QCOMPARE(movie->cacheMode(), QMovie::CacheNone);
        QCOMPARE(movie->loopCount(), -1);
        QSignalSpy frames(movie, &QMovie::frameChanged);
        auto* view = child<HistoryImageView>(w, "historyImageView");
        view->actualSize();
        QTRY_VERIFY(frames.count() >= 4);
        QCOMPARE(view->zoom(), qreal(1));
        QCOMPARE(view->image().size(), QSize(48, 32));
        auto* list = child<QListWidget>(w, "historyThumbnails");
        QTest::mouseClick(list->viewport(), Qt::LeftButton, {},
                          list->visualItemRect(list->currentItem()).center());
        QTest::mouseDClick(list->viewport(), Qt::LeftButton, {},
                           list->visualItemRect(list->currentItem()).center());
        QVERIFY(f.opened.isEmpty());
        w->refresh();
        QCOMPARE(w->findChild<QMovie*>(), movie);
        w->selectSource(QStringLiteral("recording-video"));
        QVERIFY(!w->findChild<QMovie*>());
        w->selectSource(QStringLiteral("recording-gif"));
        QTRY_VERIFY(w->findChild<QMovie*>());
        w->hide();
        QVERIFY(!w->findChild<QMovie*>());
        w->show();
        QTRY_VERIFY(w->findChild<QMovie*>());
        f.close();
        QVERIFY(!f.window);
    }
    void starFilterSortRevealDelete_data()
    {
        QTest::addColumn<bool>("gif");
        QTest::newRow("video") << false;
        QTest::newRow("gif") << true;
    }
    void starFilterSortRevealDelete()
    {
        QFETCH(bool, gif);
        Fixture f;
        auto* w = f.open(false);
        w->selectSource(gif ? QStringLiteral("recording-gif") : QStringLiteral("recording-video"));
        const auto id = w->selectedItemId();
        child<QToolButton>(w, "historyStar")->click();
        child<QComboBox>(w, "historyGroupCombo")->setCurrentIndex(1);
        QCOMPARE(child<QListWidget>(w, "historyThumbnails")->count(), 1);
        child<QToolButton>(w, "historyReveal")->click();
        QCOMPARE(f.revealed, id);
        bool correct = false;
        answerDelete(w, QMessageBox::Cancel, &correct);
        child<QToolButton>(w, "historyDelete")->click();
        QVERIFY(correct);
        QVERIFY(QFileInfo::exists(id));
        correct = false;
        answerDelete(w, QMessageBox::Yes, &correct);
        child<QToolButton>(w, "historyDelete")->click();
        QVERIFY(correct);
        QCOMPARE(f.trashed, id);
        QVERIFY(!QFileInfo::exists(id));
        QVERIFY(!w->findChild<QMovie*>());
    }
    void unreadableAndDirectoryEmptyMessages()
    {
        Fixture f;
        f.probe->fail = true;
        auto* w = f.open(false);
        w->selectSource(QStringLiteral("recording-video"));
        QTRY_VERIFY(child<QListWidget>(w, "historyThumbnails")
                        ->item(0)
                        ->text()
                        .contains(QStringLiteral("无法读取媒体信息")));
        QVERIFY(child<QToolButton>(w, "historyDelete")->isEnabled());
        f.gifs = f.temp.filePath(QStringLiteral("失效"));
        f.gifSource->directoryChanged();
        w->selectSource(QStringLiteral("recording-gif"));
        QVERIFY(child<QLabel>(w, "historyEmpty")->text().contains(QStringLiteral("设置 → 录制")));
        QDir().mkpath(f.gifs);
        f.gifSource->directoryChanged();
        QVERIFY(
            child<QLabel>(w, "historyEmpty")->text().contains(QStringLiteral("还没有录制 GIF")));
        writeMedia(f.gifs, QStringLiteral("broken.gif"), "GIF89a");
        f.gifSource->directoryChanged();
        QTRY_VERIFY(
            child<QLabel>(w, "historyEmpty")->text().contains(QStringLiteral("无法读取媒体信息")));
    }
    void narrowLayoutAndSourceReachability()
    {
        Fixture f;
        auto* w = f.open();
        w->resize(480, 640);
        child<QToolButton>(w, "historyToggleGroups")->click();
        for (const auto& id : {QStringLiteral("history"), QStringLiteral("quick-gunner"),
                               QStringLiteral("recording-video"), QStringLiteral("recording-gif")})
        {
            w->selectSource(id);
            QCOMPARE(w->selectedSourceId(), id);
            QVERIFY(child<QComboBox>(w, "resourceSourceCombo")->isVisible());
            QVERIFY(w->rect().contains(child<QComboBox>(w, "resourceSourceCombo")->geometry()));
        }
        QCOMPARE(child<QSplitter>(w, "historyImageSplitter")->orientation(), Qt::Vertical);
    }
    void trayOpenSettingsGuardAndFinishedRefresh()
    {
        Fixture f;
        RecordingPreferences preferences;
        preferences.videoDirectory = f.videos;
        preferences.gifDirectory = f.gifs;
        QVERIFY(f.settings.saveRecordingPreferences(preferences).isEmpty());
        RunOptions options;
        options.testMode = true;
        options.settingsFile = f.temp.filePath(QStringLiteral("settings.ini"));
        options.historyEnabledOverride = false;
        ApplicationController c(*qApp, options, {}, {}, {}, f.actions);
        const auto reserved = ApplicationControllerTestAccess::reserved(c);
        QVERIFY(reserved.contains(f.videos) && reserved.contains(f.gifs));
        auto* open = child<QAction>(c.menu(), "recordingOpenVideoAction");
        auto* gif = child<QAction>(c.menu(), "recordingOpenGifAction");
        QCOMPARE(open->isEnabled(), recordingCaptureSupported());
        QCOMPARE(gif->isEnabled(), recordingCaptureSupported());
        open->trigger();
        if (!recordingCaptureSupported())
        {
            // R7 禁用 Windows 整个录制子菜单；已有文件仍可从资源浏览器切换来源。
            QVERIFY(!ApplicationControllerTestAccess::window(c));
            c.openResourceBrowser(QStringLiteral("recording-video"));
        }
        auto* w = ApplicationControllerTestAccess::window(c);
        QVERIFY(w);
        QCOMPARE(w->selectedSourceId(), QStringLiteral("recording-video"));
        QVERIFY(child<QComboBox>(w, "resourceSourceCombo")->findData(QStringLiteral("history")) <
                0);
        writeMedia(f.videos, QStringLiteral("finished.mp4"));
        ApplicationControllerTestAccess::finish(c);
        QCOMPARE(child<QListWidget>(w, "historyThumbnails")->count(), 3);
        ApplicationControllerTestAccess::settingsOpen(c, true);
        QVERIFY(!open->isEnabled() && !gif->isEnabled());
        c.openResourceBrowser(QStringLiteral("recording-gif"));
        QCOMPARE(w->selectedSourceId(), QStringLiteral("recording-video"));
        ApplicationControllerTestAccess::settingsOpen(c, false);
        QCOMPARE(open->isEnabled(), recordingCaptureSupported());
        QCOMPARE(gif->isEnabled(), recordingCaptureSupported());
        if (recordingCaptureSupported())
            gif->trigger();
        else
            c.openResourceBrowser(QStringLiteral("recording-gif"));
        QCOMPARE(w->selectedSourceId(), QStringLiteral("recording-gif"));
        w->close();
    }
};
QTEST_MAIN(RecordingResourcesWindowTest)
#include "recording_resources_window_test.moc"
