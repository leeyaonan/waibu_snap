#include "app/multi_display_capture.h"
#include <QDir>
#include <QTemporaryDir>
#include <QTest>
#include <functional>
using namespace waibusnap;
namespace
{
DisplayTarget screen(quint32 id, QRect geometry, qreal dpr = 1.0)
{
    DisplayTarget target;
    target.id = id;
    target.logicalGeometry = geometry;
    target.devicePixelRatio = dpr;
    target.layoutVersion = 1;
    return target;
}
QImage solid(QSize size, QColor color)
{
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(color);
    image.setDevicePixelRatio(1);
    return image;
}
class Topology final : public DisplayTopology
{
  public:
    QVector<DisplayTarget> displays;
    QString error;
    bool matches = true;
    DisplayResult cursorDisplay() const override { return {}; }
    bool stillMatches(const DisplayTarget&) const override { return matches; }
    QVector<DisplayResult> allDisplays() const override
    {
        if (!error.isEmpty())
            return {DisplayResult{{}, error}};
        QVector<DisplayResult> result;
        for (const auto& display : displays)
            result.append({display, {}});
        return result;
    }
};
class Capture final : public CaptureProvider
{
  public:
    bool permission = true;
    // 按显示器 ID 指定失败原因或替换帧；未列出的按该屏物理尺寸返回纯色图。
    QHash<quint32, QString> failures;
    QHash<quint32, QImage> overrideFrames;
    QSet<quint32> nullFrames;
    int calls = 0;
    bool hasPermission() const override { return permission; }
    bool requestPermission() override { return permission; }
    void capture(const DisplayTarget& display,
                 std::function<void(CaptureResult)> completion) override
    {
        ++calls;
        const auto failure = failures.value(display.id);
        if (!failure.isEmpty())
            return completion({{}, CaptureError::Unavailable, failure});
        if (nullFrames.contains(display.id))
            return completion({{display, QImage(), 0}, CaptureError::None, {}});
        QImage frame = overrideFrames.value(display.id);
        if (frame.isNull())
            frame =
                solid(QSize(qRound(display.logicalGeometry.width() * display.devicePixelRatio),
                            qRound(display.logicalGeometry.height() * display.devicePixelRatio)),
                      Qt::red);
        completion({{display, frame, 0}, CaptureError::None, {}});
    }
};
// 每个用例一套独立夹具，避免控制器析构顺序与共享状态互相污染。
class Fixture
{
  public:
    Fixture(const QString& settingsFile, bool supported = true)
        : settings(settingsFile), controller(settings, topology, capture, dependencies(supported))
    {
        QObject::connect(&controller, &MultiDisplayCaptureController::finished,
                         [this](QString summary) { finished.append(summary); });
        QObject::connect(&controller, &MultiDisplayCaptureController::failed,
                         [this](QString reasons) { failed.append(reasons); });
    }
    QString start() { return controller.start(); }
    bool waitIdle()
    {
        for (int attempt = 0; attempt < 300; ++attempt)
        {
            if (!controller.busy() && (!finished.isEmpty() || !failed.isEmpty()))
                return true;
            QTest::qWait(10);
        }
        return !controller.busy();
    }
    AppSettings settings;
    Topology topology;
    Capture capture;
    QStringList notices;
    QVector<QRect> flashes;
    QVector<int> flashOrder;
    MultiDisplaySaveRequest lastSave;
    bool saveSuccess = true;
    bool stitch = false;
    QStringList finished;
    QStringList failed;
    MultiDisplayCaptureController controller;

  private:
    MultiDisplayCaptureDependencies dependencies(bool supported)
    {
        MultiDisplayCaptureDependencies result;
        result.supported = [supported] { return supported; };
        result.stitchEnabled = [this] { return stitch; };
        result.saveGroup = [this](const MultiDisplaySaveRequest& request)
        {
            lastSave = request;
            return MultiDisplaySaveResult{saveSuccess, {}, QStringLiteral("注入失败")};
        };
        result.notice = [this](const QString& notice) { notices.append(notice); };
        result.flash = [this](QRect region, int index)
        {
            flashes.append(region);
            flashOrder.append(index);
        };
        result.watermark = [](const QImage& image) { return image; };
        return result;
    }
};
QString configureDirectory(Fixture& fixture, const QString& directory)
{
    QuickGunnerPreferences preferences;
    preferences.directory = directory;
    return fixture.settings.saveQuickGunnerPreferences(preferences);
}
}
class MultiDisplayCaptureTest final : public QObject
{
    Q_OBJECT
  private slots:
    void init()
    {
        m_temp = std::make_unique<QTemporaryDir>();
        QVERIFY(m_temp->isValid());
    }
    void cleanup() { m_temp.reset(); }
    void capturesAllDisplaysAndSorts()
    {
        Fixture fixture(settingsFile());
        QCOMPARE(configureDirectory(fixture, m_temp->path()), QString());
        // 枚举顺序故意打乱：落盘与闪光都必须按系统排布。
        fixture.topology.displays = {screen(30, {1920, 0, 1920, 1080}),
                                     screen(10, {-1920, 0, 1920, 1080}),
                                     screen(20, {0, 0, 1920, 1080})};
        QCOMPARE(fixture.start(), QString());
        QVERIFY(fixture.waitIdle());
        QCOMPARE(fixture.failed.size(), 0);
        QCOMPARE(fixture.finished.size(), 1);
        QCOMPARE(fixture.lastSave.frames.size(), 3);
        QCOMPARE(fixture.flashes.size(), 3);
        QCOMPARE(fixture.flashes[0].left(), -1920);
        QCOMPARE(fixture.flashes[2].left(), 1920);
        QVERIFY(fixture.notices.last().contains(QStringLiteral("已保存 3 张截图")));
    }
    void singleDisplayFailureAbortsWholeGroup()
    {
        Fixture fixture(settingsFile());
        QCOMPARE(configureDirectory(fixture, m_temp->path()), QString());
        fixture.topology.displays = {screen(1, {0, 0, 1920, 1080}),
                                     screen(2, {1920, 0, 1920, 1080})};
        fixture.capture.failures.insert(2, QStringLiteral("屏幕录制权限已被撤销。"));
        QCOMPARE(fixture.start(), QString());
        QVERIFY(fixture.waitIdle());
        QCOMPARE(fixture.finished.size(), 0);
        QCOMPARE(fixture.failed.size(), 1);
        // 整组不落盘：没有保存请求，也没有闪光。
        QVERIFY(fixture.lastSave.frames.isEmpty());
        QVERIFY(fixture.flashes.isEmpty());
        QVERIFY(fixture.failed.first().contains(QStringLiteral("1/2 屏失败")));
        QVERIFY(fixture.failed.first().contains(QStringLiteral("第 2 屏")));
        QVERIFY(fixture.failed.first().contains(QStringLiteral("权限")));
        QCOMPARE(fixture.notices.size(), 1);
    }
    void duplicateReasonsAreMerged()
    {
        Fixture fixture(settingsFile());
        QCOMPARE(configureDirectory(fixture, m_temp->path()), QString());
        fixture.topology.displays = {screen(1, {0, 0, 100, 100}), screen(2, {100, 0, 100, 100}),
                                     screen(3, {200, 0, 100, 100})};
        fixture.capture.failures.insert(1, QStringLiteral("显示器布局已变化。"));
        fixture.capture.failures.insert(3, QStringLiteral("显示器布局已变化。"));
        QCOMPARE(fixture.start(), QString());
        QVERIFY(fixture.waitIdle());
        QCOMPARE(fixture.failed.size(), 1);
        const auto message = fixture.failed.first();
        QVERIFY(message.contains(QStringLiteral("2/3 屏失败")));
        QVERIFY(message.contains(QStringLiteral("第 1 屏、第 3 屏")));
        QCOMPARE(message.count(QStringLiteral("布局已变化")), 1);
    }
    void sizeMismatchIsRejected()
    {
        Fixture fixture(settingsFile());
        QCOMPARE(configureDirectory(fixture, m_temp->path()), QString());
        fixture.topology.displays = {screen(1, {0, 0, 1920, 1080}, 2.0)};
        fixture.capture.overrideFrames.insert(1, solid({100, 100}, Qt::red));
        QCOMPARE(fixture.start(), QString());
        QVERIFY(fixture.waitIdle());
        QCOMPARE(fixture.failed.size(), 1);
        QVERIFY(fixture.failed.first().contains(QStringLiteral("尺寸不匹配")));
    }
    void stitchIsComposedAndSaved()
    {
        Fixture fixture(settingsFile());
        QCOMPARE(configureDirectory(fixture, m_temp->path()), QString());
        fixture.stitch = true;
        fixture.topology.displays = {screen(1, {0, 0, 100, 50}), screen(2, {100, 0, 100, 50})};
        QCOMPARE(fixture.start(), QString());
        QVERIFY(fixture.waitIdle());
        QCOMPARE(fixture.failed.size(), 0);
        QVERIFY(fixture.lastSave.stitch);
        QCOMPARE(fixture.lastSave.stitchImage.size(), QSize(200, 50));
        QVERIFY(fixture.notices.last().contains(QStringLiteral("+ 1 张拼接图")));
    }
    void stitchLimitAbortsGroup()
    {
        Fixture fixture(settingsFile());
        QCOMPARE(configureDirectory(fixture, m_temp->path()), QString());
        fixture.stitch = true;
        fixture.topology.displays = {screen(1, {0, 0, 20000, 100})};
        QCOMPARE(fixture.start(), QString());
        QVERIFY(fixture.waitIdle());
        QCOMPARE(fixture.finished.size(), 0);
        QCOMPARE(fixture.failed.size(), 1);
        QVERIFY(fixture.failed.first().contains(QStringLiteral("拼接图过大")));
        QVERIFY(fixture.lastSave.frames.isEmpty());
    }
    void saveFailureSurfacesReason()
    {
        Fixture fixture(settingsFile());
        QCOMPARE(configureDirectory(fixture, m_temp->path()), QString());
        fixture.saveSuccess = false;
        fixture.topology.displays = {screen(1, {0, 0, 100, 100})};
        QCOMPARE(fixture.start(), QString());
        QVERIFY(fixture.waitIdle());
        QCOMPARE(fixture.finished.size(), 0);
        QCOMPARE(fixture.failed.size(), 1);
        QVERIFY(fixture.failed.first().contains(QStringLiteral("注入失败")));
        QVERIFY(fixture.flashes.isEmpty());
    }
    void busyRejectsSecondTrigger()
    {
        Fixture fixture(settingsFile());
        QCOMPARE(configureDirectory(fixture, m_temp->path()), QString());
        fixture.topology.displays = {screen(1, {0, 0, 100, 100})};
        QCOMPARE(fixture.start(), QString());
        QVERIFY(fixture.controller.busy());
        QVERIFY(fixture.start().contains(QStringLiteral("正在截取全部显示器")));
        QVERIFY(fixture.waitIdle());
        QCOMPARE(fixture.finished.size(), 1);
    }
    void preflightRejections()
    {
        // 目录为空
        {
            Fixture fixture(settingsFile());
            fixture.topology.displays = {screen(1, {0, 0, 100, 100})};
            QVERIFY(fixture.start().contains(QStringLiteral("快枪手目录")));
        }
        // 平台不支持
        {
            Fixture fixture(settingsFile(), false);
            QCOMPARE(configureDirectory(fixture, m_temp->path()), QString());
            fixture.topology.displays = {screen(1, {0, 0, 100, 100})};
            QVERIFY(fixture.start().contains(QStringLiteral("暂不支持")));
        }
        // 无显示器
        {
            Fixture fixture(settingsFile());
            QCOMPARE(configureDirectory(fixture, m_temp->path()), QString());
            QVERIFY(fixture.start().contains(QStringLiteral("未获取到")));
        }
        // 显示器层报错
        {
            Fixture fixture(settingsFile());
            QCOMPARE(configureDirectory(fixture, m_temp->path()), QString());
            fixture.topology.error = QStringLiteral("系统与 Qt 显示器坐标不一致。");
            QVERIFY(fixture.start().contains(QStringLiteral("坐标不一致")));
        }
        // 布局变化
        {
            Fixture fixture(settingsFile());
            QCOMPARE(configureDirectory(fixture, m_temp->path()), QString());
            fixture.topology.displays = {screen(1, {0, 0, 100, 100})};
            fixture.topology.matches = false;
            QVERIFY(fixture.start().contains(QStringLiteral("布局已变化")));
        }
        // 权限缺失
        {
            Fixture fixture(settingsFile());
            QCOMPARE(configureDirectory(fixture, m_temp->path()), QString());
            fixture.topology.displays = {screen(1, {0, 0, 100, 100})};
            fixture.capture.permission = false;
            QVERIFY(!fixture.start().isEmpty());
        }
    }
    void emptyImageIsRejected()
    {
        Fixture fixture(settingsFile());
        QCOMPARE(configureDirectory(fixture, m_temp->path()), QString());
        fixture.topology.displays = {screen(1, {0, 0, 100, 100})};
        fixture.capture.nullFrames.insert(1);
        QCOMPARE(fixture.start(), QString());
        QVERIFY(fixture.waitIdle());
        QCOMPARE(fixture.failed.size(), 1);
    }

  private:
    QString settingsFile() const
    {
        return QDir(m_temp->path()).filePath(QStringLiteral("settings.ini"));
    }
    std::unique_ptr<QTemporaryDir> m_temp;
};
QTEST_MAIN(MultiDisplayCaptureTest)
#include "multi_display_capture_test.moc"
