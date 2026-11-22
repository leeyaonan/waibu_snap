#include "app/capture_directory.h"
#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>
using namespace waibusnap;
class CaptureDirectoryTest final : public QObject
{
    Q_OBJECT
  private slots:
    void rejectsInvalidInput()
    {
        QString cleaned;
        QVERIFY(!validateCaptureDirectory(QString(), {}, cleaned).isEmpty());
        QVERIFY(!validateCaptureDirectory(QStringLiteral("relative/path"), {}, cleaned).isEmpty());
        QVERIFY(cleaned.isEmpty());
        QString withNull = QStringLiteral("/tmp/waibu");
        withNull.append(QChar::Null);
        QVERIFY(!validateCaptureDirectory(withNull, {}, cleaned).isEmpty());
    }
    void rejectsMissingAndNonDirectory()
    {
        QString cleaned;
        QVERIFY(
            !validateCaptureDirectory(QStringLiteral("/nonexistent-waibusnap-dir-xyz"), {}, cleaned)
                 .isEmpty());
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString file = QDir(directory.path()).filePath(QStringLiteral("file.txt"));
        QFile probe(file);
        QVERIFY(probe.open(QIODevice::WriteOnly));
        probe.close();
        QVERIFY(!validateCaptureDirectory(file, {}, cleaned).isEmpty());
    }
    void rejectsSymlink()
    {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const QString target = QDir(parent.path()).filePath(QStringLiteral("real"));
        QVERIFY(QDir().mkpath(target));
        const QString link = QDir(parent.path()).filePath(QStringLiteral("link"));
        if (!QFile::link(target, link))
            QSKIP("当前文件系统无法创建符号链接。");
        QString cleaned;
        QVERIFY(!validateCaptureDirectory(link, {}, cleaned).isEmpty());
    }
    void acceptsWritableDirectory()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QString cleaned;
        QVERIFY2(validateCaptureDirectory(directory.path(), {}, cleaned).isEmpty(),
                 qPrintable(cleaned));
        // 返回规范化后的输入路径（cleanPath），不强制解析成真实路径。
        QCOMPARE(cleaned, QDir::cleanPath(directory.path()));
    }
    void rejectsOverlapWithReserved()
    {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const QString history = QDir(parent.path()).filePath(QStringLiteral("history"));
        const QString capture = QDir(history).filePath(QStringLiteral("shots"));
        QVERIFY(QDir().mkpath(capture));
        QString cleaned;
        // 完全重合、被包含、包含保留目录三种情况都必须拒绝。
        QVERIFY(!validateCaptureDirectory(history, {history}, cleaned).isEmpty());
        QVERIFY(!validateCaptureDirectory(capture, {history}, cleaned).isEmpty());
        QVERIFY(!validateCaptureDirectory(parent.path(), {history}, cleaned).isEmpty());
        // 保留目录不存在但父级可解析时，仅按路径前缀判断重叠（此处不重叠 → 通过）。
        QVERIFY(
            validateCaptureDirectory(capture, {QStringLiteral("/nonexistent-reserved")}, cleaned)
                .isEmpty());
        // 无关目录可以通过。
        const QString other = QDir(parent.path()).filePath(QStringLiteral("other"));
        QVERIFY(QDir().mkpath(other));
        QVERIFY(validateCaptureDirectory(other, {history}, cleaned).isEmpty());
    }
};
QTEST_MAIN(CaptureDirectoryTest)
#include "capture_directory_test.moc"
