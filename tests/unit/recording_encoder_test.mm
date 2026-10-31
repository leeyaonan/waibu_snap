#include "../fixtures/recording_control_fakes.h"
#include "platform/macos/recording_services.h"
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QtTest>

using namespace waibusnap;

class RecordingEncoderTest final : public QObject
{
    Q_OBJECT
  private slots:
    void readOnlyCapabilityQueryIsCached()
    {
        const bool available = macH264EncoderAvailable();
        QCOMPARE(macH264EncoderAvailable(), available);
        qInfo() << "本环境 H.264 编码器列表可用：" << available;
    }
    void unavailableEncoder_data()
    {
        QTest::addColumn<bool>("quitDuringStart");
        QTest::newRow("normal-failure") << false;
        QTest::newRow("save-and-quit-during-start") << true;
    }
    void unavailableEncoder()
    {
        QFETCH(bool, quitDuringStart);
        RecordingHarness h;
        auto dependencies = h.dependencies();
        QString temporary;
        int queries = 0;
        bool partialCreated = false;
        dependencies.sessionFactory = [&]
        {
            return createMacRecordingSession(
                [&]
                {
                    ++queries;
                    // 在 start 参数校验后模拟残留，走真实 failSession 与析构清理。
                    QFile partial(temporary);
                    partialCreated =
                        partial.open(QIODevice::WriteOnly) && partial.write("partial") == 7;
                    return false;
                });
        };
        RecordingController controller(std::move(dependencies));
        QSignalSpy failed(&controller, &RecordingController::failed);
        QSignalSpy ready(&controller, &RecordingController::quitReady);
        QSignalSpy finished(&controller, &RecordingController::finished);
        controller.startSelection(h.preferences(3));
        h.drain();
        h.select();
        temporary = controller.request().tempPath;
        const auto finalPath = controller.request().finalPath;
        h.fake->clock += 3000;
        controller.poll();
        QCOMPARE(controller.state(), RecordingController::State::Preparing);
        QCOMPARE(queries, 1);
        QVERIFY(partialCreated);
        if (quitDuringStart)
        {
            controller.quitRequested(RecordingController::QuitChoice::Save);
            controller.quitRequested(RecordingController::QuitChoice::Save);
            QCOMPARE(controller.state(), RecordingController::State::Finalizing);
        }
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(controller.state(), RecordingController::State::Idle);
        QCOMPARE(ready.size(), quitDuringStart ? 1 : 0);
        QCOMPARE(finished.size(), 0);
        QCOMPARE(h.fake->toasts, 1);
        QCOMPARE(h.fake->lastToast.type, RecordingEventType::Failed);
        const QString reason = QStringLiteral("当前环境不支持 H.264 编码，无法开始录制");
        QVERIFY(failed.first().first().toString().contains(reason));
        QVERIFY(h.fake->lastToast.message.contains(reason));
        QVERIFY(h.fake->lastToast.path.isEmpty());
        QTRY_VERIFY(!QFileInfo::exists(temporary));
        QVERIFY(!QFileInfo::exists(finalPath));
        const auto parts = QDir(h.directory.path())
                               .entryList({QStringLiteral("*.part")}, QDir::Files | QDir::Hidden);
        QVERIFY(parts.isEmpty());
        h.advance(controller, 30000);
        QCOMPARE(failed.size(), 1);
        QCOMPARE(ready.size(), quitDuringStart ? 1 : 0);
    }
};
QTEST_MAIN(RecordingEncoderTest)
#include "recording_encoder_test.moc"
