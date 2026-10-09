#include "output/annotation_renderer.h"
#include "output/image_output.h"
#include <QColorSpace>
#include <QDir>
#include <QFile>
#include <QImageReader>
#include <QTemporaryDir>
#include <QTest>
#include <algorithm>
#include <utility>
class ImageOutputTest final : public QObject
{
    Q_OBJECT
  private slots:
    void jpegRoundTripSuffixAndAnnotations_data()
    {
        QTest::addColumn<QString>("name");
        QTest::newRow("jpg") << QStringLiteral("图片.jpg");
        QTest::newRow("jpeg") << QStringLiteral("图片.jpeg");
        QTest::newRow("uppercase") << QStringLiteral("图片.JPEG");
        QTest::newRow("bare") << QStringLiteral("图片");
    }
    void jpegRoundTripSuffixAndAnnotations()
    {
        using namespace waibusnap;
        QFETCH(QString, name);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString directory = temporary.filePath(QStringLiteral("中文 空格"));
        QVERIFY(QDir().mkpath(directory));
        QImage source(160, 120, QImage::Format_RGB32);
        source.fill(Qt::white);
        source.setColorSpace(QColorSpace::SRgb);
        source.setDevicePixelRatio(2);
        Annotation cover;
        cover.type = AnnotationType::Cover;
        cover.first = {30, 30};
        cover.last = {100, 80};
        cover.style.color = Qt::red;
        const QImage rendered = renderAnnotatedSelection(source, {cover}, source.rect());
        const QString path = QDir(directory).filePath(name);
        const auto result = exportJpegToPath(rendered, path);
        QVERIFY2(result.success, qPrintable(result.explanation));
        QImageReader reader(imageFilePath(path, ImageFormat::Jpeg));
        QCOMPARE(reader.format(), QByteArray("jpeg"));
        QCOMPARE(reader.size(), source.size());
        const QImage loaded = reader.read();
        QVERIFY(!loaded.isNull());
        QCOMPARE(loaded.devicePixelRatio(), qreal(1));
        QVERIFY(!loaded.hasAlphaChannel());
        QCOMPARE(loaded.colorSpace(), QColorSpace(QColorSpace::SRgb));
        int redPixels = 0;
        for (int y = 35; y < 75; ++y)
            for (int x = 35; x < 95; ++x)
            {
                const auto color = loaded.pixelColor(x, y);
                if (color.red() > 220 && color.green() < 30 && color.blue() < 30)
                    ++redPixels;
            }
        QVERIFY(redPixels > 2200);
        QVERIFY(loaded.pixelColor(10, 10).red() > 245);
        QVERIFY(loaded.pixelColor(10, 10).green() > 245);
        QCOMPARE(source.devicePixelRatio(), qreal(2));
        QCOMPARE(imageFilePath(QStringLiteral("图.jpg"), ImageFormat::Jpeg),
                 QStringLiteral("图.jpg"));
        QCOMPARE(imageFilePath(QStringLiteral("图.jpeg"), ImageFormat::Jpeg),
                 QStringLiteral("图.jpeg"));
        QCOMPARE(imageFilePath(QStringLiteral("图"), ImageFormat::Jpeg), QStringLiteral("图.jpg"));
        QVERIFY(imageFilePath({}, ImageFormat::Jpeg).isEmpty());
    }
    void jpegAtomicFailuresPreserveExistingFile()
    {
        using namespace waibusnap;
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QImage image(8, 6, QImage::Format_RGB32);
        image.fill(Qt::blue);
        const QString missing = temporary.filePath(QStringLiteral("不存在/图.jpg"));
        const auto absent = exportJpegToPath(image, missing);
        QVERIFY(!absent.success);
        QVERIFY(!absent.explanation.isEmpty());
        QVERIFY(!QFileInfo::exists(missing));
        const QString path = temporary.filePath(QStringLiteral("只读.jpeg"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray original("原文件内容");
        QCOMPARE(file.write(original), qint64(original.size()));
        file.close();
        QVERIFY(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::ReadUser |
                                                QFileDevice::ReadGroup | QFileDevice::ReadOther));
        const auto denied = exportJpegToPath(image, path);
        const bool restored =
            QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                            QFileDevice::ReadUser | QFileDevice::WriteUser);
        QVERIFY(restored);
        QVERIFY(!denied.success);
        QVERIFY(!denied.explanation.isEmpty());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), original);
        file.close();
        QVERIFY(!exportJpegToPath({}, path).success);
        QVERIFY(!exportJpegToPath(image, {}).success);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), original);
        QCOMPARE(QDir(temporary.path()).entryList(QDir::Files | QDir::Hidden).size(), 1);
    }
    void newImageFilesNeverOverwriteAndCleanFailures()
    {
        using namespace waibusnap;
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QImage image(40, 30, QImage::Format_RGB32);
        image.fill(Qt::green);
        const QDateTime timestamp(QDate(2026, 10, 9), QTime(12, 0));
        for (const auto format : {ImageFormat::Png, ImageFormat::Jpeg})
        {
            const QString originalPath = suggestedImagePath(temporary.path(), format, timestamp);
            QFile file(originalPath);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write("original"), qint64(8));
            file.close();
            const auto first = exportImageToNewPath(image, temporary.path(), format, timestamp);
            const auto second = exportImageToNewPath(image, temporary.path(), format, timestamp);
            QVERIFY2(first.result.success, qPrintable(first.result.explanation));
            QVERIFY2(second.result.success, qPrintable(second.result.explanation));
            QVERIFY(first.path != originalPath && second.path != first.path);
            QVERIFY(file.open(QIODevice::ReadOnly));
            QCOMPARE(file.readAll(), QByteArray("original"));
            file.close();
            QCOMPARE(QImage(first.path).size(), image.size());
            QVERIFY(!exportImageToNewPath({}, temporary.path(), format, timestamp).result.success);
            QVERIFY(!exportImageToNewPath(image, temporary.filePath(QStringLiteral("missing")),
                                          format, timestamp)
                         .result.success);
        }
        QCOMPARE(QDir(temporary.path()).entryList(QDir::Files | QDir::Hidden).size(), 6);
    }
    void mosaicBlockFormulaAndAreaAverage()
    {
        using namespace waibusnap;
        QCOMPARE(mosaicBlockSize({1, 2}), 4);
        QCOMPARE(mosaicBlockSize({53, 100}), 4);
        QCOMPARE(mosaicBlockSize({54, 100}), 5);
        QCOMPARE(mosaicBlockSize({120, 200}), 10);
        QCOMPARE(mosaicBlockSize({576, 900}), 48);
        QCOMPARE(mosaicBlockSize({10000, 20000}), 48);
        QImage source(11, 9, QImage::Format_ARGB32);
        for (int y = 0; y < source.height(); ++y)
            for (int x = 0; x < source.width(); ++x)
                source.setPixelColor(x, y, QColor(x * 20, y * 20, 80, 100));
        source.setDevicePixelRatio(2);
        source.setColorSpace(QColorSpace::SRgb);
        const auto original = source;
        const auto patch = pixelateRegion(source, {1.25, 1.25, 8.5, 6.5});
        QCOMPARE(patch.pixels, QRect(1, 1, 9, 7));
        QCOMPARE(patch.image.devicePixelRatio(), qreal(1));
        QCOMPARE(patch.image.colorSpace(), source.colorSpace());
        QCOMPARE(pixelateRegion(source, {1.25, 1.25, 8.5, 6.5}).image, patch.image);
        // 独立计算线性渐变块平均，完整块及残块均须均匀且不透明。
        for (int y = 0; y < 7; ++y)
            for (int x = 0; x < 9; ++x)
            {
                const int left = 1 + x / 4 * 4, top = 1 + y / 4 * 4;
                const int right = std::min(left + 3, 9), bottom = std::min(top + 3, 7);
                QCOMPARE(patch.image.pixelColor(x, y),
                         QColor((left + right) * 10, (top + bottom) * 10, 80));
            }
        const auto clipped = pixelateRegion(source, {-2.25, -1.25, 10, 8});
        QCOMPARE(clipped.pixels, QRect(0, 0, 8, 7));
        // 原网格起点为 (-3,-2)，裁剪后第一块只有 1×2 像素。
        QCOMPARE(clipped.image.pixelColor(0, 0), QColor(0, 10, 80));
        QCOMPARE(clipped.image.pixelColor(0, 1), QColor(0, 10, 80));
        QCOMPARE(clipped.image.pixelColor(1, 2), QColor(50, 70, 80));
        const auto edge = pixelateRegion(source, {9.25, 7.25, 10, 10});
        QCOMPARE(edge.pixels, QRect(9, 7, 2, 2));
        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 2; ++x)
                QCOMPARE(edge.image.pixelColor(x, y), QColor(190, 150, 80));
        QVERIFY(pixelateRegion(source, {20, 20, 10, 10}).image.isNull());
        QVERIFY(pixelateRegion(source, {1, 1, 0, 8}).image.isNull());
        QVERIFY(pixelateRegion({}, {1, 1, 8, 8}).image.isNull());
        const auto tiny = pixelateRegion(source, {5.2, 6.2, 0.1, 0.1});
        QCOMPARE(tiny.pixels, QRect(5, 6, 1, 1));
        QCOMPARE(tiny.image.pixelColor(0, 0), QColor(100, 120, 80));
        QCOMPARE(source, original);
    }
    void mosaicReplacesShapeAndTextPixels_data()
    {
        QTest::addColumn<qreal>("dpr");
        QTest::addColumn<bool>("reverse");
        QTest::addColumn<QRect>("covered");
        const qreal dprs[] = {1, 1.5, 2};
        const QRect bounds[] = {{13, 11, 63, 39}, {19, 16, 95, 59}, {26, 22, 126, 78}};
        for (int index = 0; index < 3; ++index)
            for (const bool reverse : {false, true})
                QTest::newRow(qPrintable(QStringLiteral("%1x-%2").arg(dprs[index]).arg(reverse)))
                    << dprs[index] << reverse << bounds[index];
    }
    void mosaicReplacesShapeAndTextPixels()
    {
        QFETCH(qreal, dpr);
        QFETCH(bool, reverse);
        QFETCH(QRect, covered);
        using namespace waibusnap;
        QTemporaryDir temporary;
        QImage source(160, 120, QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < source.height(); ++y)
            for (int x = 0; x < source.width(); ++x)
                source.setPixelColor(x, y, QColor(x, y, (x + y) % 256));
        source.setDevicePixelRatio(dpr);
        const auto original = source;
        Annotation rectangle{AnnotationType::Rectangle, {}, {5, 5}, {150, 100}, {}, {}};
        Annotation line{AnnotationType::Line, {}, {0, 30}, {160, 30}, {}, {}};
        line.style.color = Qt::green;
        Annotation text{
            AnnotationType::Text, {}, {10, 15}, {}, {}, QStringLiteral("敏感内容 ABC 123\n第二行")};
        QVector<Annotation> underneath{rectangle, line, text};
        const auto before = renderAnnotatedSelection(source, underneath, source.rect());
        QVERIFY(before != cropFrozenSelection(source, source.rect()));
        Annotation mosaic{AnnotationType::Mosaic,      {}, QPointF(13.25, 11.25) * dpr,
                          QPointF(75.75, 49.75) * dpr, {}, {}};
        if (reverse)
            std::swap(mosaic.first, mosaic.last);
        auto annotations = underneath;
        annotations.append(mosaic);
        const auto output = renderAnnotatedSelection(source, annotations, source.rect());
        const QString path = temporary.filePath(QStringLiteral("马赛克.png"));
        QVERIFY(exportPngToPath(output, path).success);
        const QImage loaded(path);
        const int block = dpr == 2 ? 7 : dpr == 1.5 ? 5 : 4;
        QCOMPARE(mosaicBlockSize(covered.size()), block);
        bool changed = false;
        // 不调用像素化函数构造期望值，独立核对原基图的面积平均及完整网格。
        for (int y = 0; y < loaded.height(); ++y)
            for (int x = 0; x < loaded.width(); ++x)
            {
                QColor expected = before.pixelColor(x, y);
                if (covered.contains(x, y))
                {
                    const int left = covered.x() + (x - covered.x()) / block * block;
                    const int top = covered.y() + (y - covered.y()) / block * block;
                    const int right = std::min(left + block - 1, covered.right());
                    const int bottom = std::min(top + block - 1, covered.bottom());
                    expected = QColor((left + right + 1) / 2, (top + bottom + 1) / 2,
                                      (left + right + top + bottom + 1) / 2);
                    changed |= expected != source.pixelColor(x, y);
                }
                QCOMPARE(output.pixelColor(x, y), expected);
                QCOMPARE(loaded.pixelColor(x, y), expected);
            }
        QVERIFY(changed);
        QImage preview = source;
        AnnotationRenderCache cache;
        {
            QPainter painter(&preview);
            painter.scale(1 / dpr, 1 / dpr);
            paintAnnotations(painter, annotations, source, &cache);
        }
        preview.setDevicePixelRatio(1);
        QCOMPARE(preview, output);
        const QRect crop(20, 20, 80, 60);
        QCOMPARE(renderAnnotatedSelection(source, annotations, crop), output.copy(crop));
        AnnotationHistory history;
        for (const auto& annotation : annotations)
            QVERIFY(history.add(annotation));
        QVERIFY(history.undo());
        QCOMPARE(renderAnnotatedSelection(source, history.annotations(), source.rect()), before);
        QVERIFY(history.redo());
        QCOMPARE(renderAnnotatedSelection(source, history.annotations(), source.rect()), output);
        // 样式不参与马赛克；草稿调用也必须完全一致。
        mosaic.style = {QColor(), 0, 0, {}};
        annotations.last() = mosaic;
        QCOMPARE(renderAnnotatedSelection(source, annotations, source.rect()), output);
        QImage draftPreview = renderAnnotatedSelection(source, underneath, source.rect());
        {
            QPainter painter(&draftPreview);
            paintAnnotation(painter, mosaic, source);
        }
        QCOMPARE(draftPreview, output);
        Annotation cover = mosaic;
        cover.type = AnnotationType::Cover;
        cover.style = {};
        cover.style.color = Qt::blue;
        annotations.append(cover);
        const auto coveredOutput = renderAnnotatedSelection(source, annotations, source.rect());
        for (int y = covered.top(); y <= covered.bottom(); ++y)
            for (int x = covered.left(); x <= covered.right(); ++x)
                QCOMPARE(coveredOutput.pixelColor(x, y), QColor(Qt::blue));
        // 后画马赛克仍采样原基图，覆盖实心遮盖；后画图形仍位于马赛克之上。
        annotations.append(mosaic);
        QCOMPARE(renderAnnotatedSelection(source, annotations, source.rect()), output);
        annotations.append(line);
        QCOMPARE(renderAnnotatedSelection(source, annotations, source.rect()).pixelColor(50, 30),
                 QColor(Qt::green));
        Annotation outside = mosaic;
        outside.first = {200, 200};
        outside.last = {300, 300};
        annotations = {outside};
        QCOMPARE(renderAnnotatedSelection(source, annotations, source.rect()),
                 cropFrozenSelection(source, source.rect()));
        QCOMPARE(source, original);
    }
    void mosaicCacheMatchesDirectRenderingAndInvalidates()
    {
        using namespace waibusnap;
        QImage source(120, 100, QImage::Format_ARGB32_Premultiplied);
        source.fill(Qt::white);
        source.setPixelColor(20, 20, Qt::black);
        QVector<Annotation> annotations{{AnnotationType::Mosaic, {}, {10, 10}, {90, 80}, {}, {}}};
        AnnotationRenderCache cache;
        const auto compareCached = [&]
        {
            QImage preview = source.copy();
            {
                QPainter painter(&preview);
                paintAnnotations(painter, annotations, source, &cache);
            }
            QCOMPARE(preview, renderAnnotatedSelection(source, annotations, source.rect()));
        };
        compareCached();
        compareCached();
        source.setPixelColor(20, 20, Qt::red);
        compareCached();
        annotations.last().last = {30, 30};
        compareCached();
        annotations.clear();
        compareCached();
        annotations.append({AnnotationType::Mosaic, {}, {15, 15}, {70, 65}, {}, {}});
        compareCached();
    }
    void coverReplacesShapeAndTextPixels_data()
    {
        QTest::addColumn<qreal>("dpr");
        QTest::addColumn<bool>("reverse");
        QTest::addColumn<QRect>("covered");
        const qreal dprs[] = {1, 1.5, 2};
        const QRect bounds[] = {{13, 11, 63, 39}, {19, 16, 95, 59}, {26, 22, 126, 78}};
        for (int index = 0; index < 3; ++index)
            for (const bool reverse : {false, true})
                QTest::newRow(qPrintable(QStringLiteral("%1x-%2").arg(dprs[index]).arg(reverse)))
                    << dprs[index] << reverse << bounds[index];
    }
    void coverReplacesShapeAndTextPixels()
    {
        QFETCH(qreal, dpr);
        QFETCH(bool, reverse);
        QFETCH(QRect, covered);
        using namespace waibusnap;
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QImage source(160, 120, QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < source.height(); ++y)
            for (int x = 0; x < source.width(); ++x)
                source.setPixelColor(x, y, QColor(x, y, (x + y) % 256, 150 + x % 100));
        source.setDevicePixelRatio(dpr);
        const QImage original = source;
        Annotation rectangle{AnnotationType::Rectangle, {}, {5, 5}, {150, 100}, {}, {}};
        Annotation line{AnnotationType::Line, {}, {0, 30}, {160, 30}, {}, {}};
        line.style.color = Qt::green;
        Annotation text{
            AnnotationType::Text, {}, {10, 15}, {}, {}, QStringLiteral("敏感内容 ABC 123\n第二行")};
        const QVector<Annotation> underneath{rectangle, line, text};
        const QImage before = renderAnnotatedSelection(source, underneath, source.rect());
        QVERIFY(before != cropFrozenSelection(source, source.rect()));
        const QString beforePath = temporary.filePath(QStringLiteral("遮盖前.png"));
        QVERIFY(exportPngToPath(before, beforePath).success);
        const QImage savedBefore(beforePath);
        Annotation cover{AnnotationType::Cover,       {}, QPointF(13.25, 11.25) * dpr,
                         QPointF(75.75, 49.75) * dpr, {}, {}};
        if (reverse)
            std::swap(cover.first, cover.last);
        for (const QColor& color : annotationColors())
        {
            cover.style.color = color;
            // 即使模型颜色含透明度，实心遮盖仍必须替换为不透明纯色。
            cover.style.color.setAlpha(40);
            auto annotations = underneath;
            annotations.append(cover);
            const auto output = renderAnnotatedSelection(source, annotations, source.rect());
            QCOMPARE(output.devicePixelRatio(), qreal(1));
            const QString path = temporary.filePath(QStringLiteral("遮盖.png"));
            QVERIFY(exportPngToPath(output, path).success);
            const QImage loaded(path);
            QCOMPARE(loaded.size(), source.size());
            QCOMPARE(loaded.devicePixelRatio(), qreal(1));
            // 全图回读包含四边 / 四角及边界外邻接像素，检查无描边或半透明泄漏。
            for (int y = 0; y < loaded.height(); ++y)
                for (int x = 0; x < loaded.width(); ++x)
                {
                    const QColor expected =
                        covered.contains(x, y) ? color : before.pixelColor(x, y);
                    // 区域外分别比较内存合成与 PNG 基线，避免预乘转直通道的量化差异。
                    QCOMPARE(loaded.pixelColor(x, y),
                             covered.contains(x, y) ? color : savedBefore.pixelColor(x, y));
                    QCOMPARE(output.pixelColor(x, y), expected);
                }
            QImage preview = source;
            {
                QPainter painter(&preview);
                painter.scale(1 / dpr, 1 / dpr);
                paintAnnotations(painter, annotations, source);
            }
            preview.setDevicePixelRatio(1);
            QCOMPARE(preview, output);
            const QRect crop(20, 20, 80, 60);
            QCOMPARE(renderAnnotatedSelection(source, annotations, crop), output.copy(crop));
            // 后画的图形仍按原序列覆盖遮盖；不会把遮盖固定成顶层。
            annotations.append(line);
            const auto later = renderAnnotatedSelection(source, annotations, source.rect());
            QCOMPARE(later.pixelColor(50, 30), line.style.color);
            QVERIFY(later != output);
        }
        QCOMPARE(source, original);
        QCOMPARE(source.devicePixelRatio(), dpr);
    }
    void annotationPixelsAndCrop_data()
    {
        QTest::addColumn<qreal>("dpr");
        QTest::newRow("normal") << qreal(1);
        QTest::newRow("fractional") << qreal(1.5);
        QTest::newRow("retina") << qreal(2);
    }
    void annotationPixelsAndCrop()
    {
        QFETCH(qreal, dpr);
        using namespace waibusnap;
        QImage source(100, 60, QImage::Format_ARGB32_Premultiplied);
        source.fill(Qt::white);
        source.setDevicePixelRatio(dpr);
        Annotation rectangle{AnnotationType::Rectangle, {}, {10, 10}, {30, 30}, {}, {}};
        rectangle.style.color = Qt::red;
        rectangle.style.lineWidth = 2;
        Annotation line{AnnotationType::Line, {}, {-10, 20}, {110, 20}, {}, {}};
        line.style.color = Qt::blue;
        line.style.lineWidth = 4;
        Annotation outside{AnnotationType::Rectangle, {}, {70, 10}, {90, 30}, {}, {}};
        const auto output =
            renderAnnotatedSelection(source, {rectangle, line, outside}, {11, 5, 18, 30});
        QCOMPARE(output.size(), QSize(18, 30));
        QCOMPARE(output.devicePixelRatio(), qreal(1));
        QCOMPARE(source.devicePixelRatio(), dpr);
        // 裁剪窗避开矩形圆角；每个像素都有可独立计算的精确颜色。
        for (int y = 0; y < output.height(); ++y)
            for (int x = 0; x < output.width(); ++x)
            {
                const int sourceY = y + 5;
                const QColor expected =
                    sourceY == 9 || sourceY == 10 || sourceY == 29 || sourceY == 30
                        ? QColor(Qt::red)
                    : sourceY >= 18 && sourceY <= 21 ? QColor(Qt::blue)
                                                     : QColor(Qt::white);
                QCOMPARE(output.pixelColor(x, y), expected);
            }
        QCOMPARE(source.pixelColor(11, 10), QColor(Qt::white));
        QVERIFY(renderAnnotatedSelection(source, {rectangle}, {-1, 0, 5, 5}).isNull());
        // 移动裁剪窗口仅改变相对落点；源图和标注不发生缩放或平移。
        const auto moved = renderAnnotatedSelection(source, {rectangle}, {8, 8, 30, 30});
        QCOMPARE(moved.pixelColor(7, 1), QColor(Qt::red));
        QCOMPARE(moved.pixelColor(7, 2), QColor(Qt::red));
        QCOMPARE(moved.pixelColor(7, 7), QColor(Qt::white));
    }
    void sevenTypesRenderAndUndoRedoRestoresPixels()
    {
        using namespace waibusnap;
        QImage background(400, 240, QImage::Format_ARGB32_Premultiplied);
        background.fill(Qt::white);
        AnnotationHistory history;
        QVector<QImage> steps{background};
        for (int type = 0; type < 7; ++type)
        {
            Annotation annotation{
                static_cast<AnnotationType>(type), {}, {qreal(10 + type * 55), 30},
                {qreal(40 + type * 55), 60},       {}, {}};
            annotation.points = {
                annotation.first, {annotation.first.x() + 15, 45}, annotation.last};
            annotation.text = QStringLiteral("截图说明 ABC 123\n第二行");
            annotation.style.color = annotationColors()[type % 3];
            QVERIFY(history.add(annotation));
            const auto output =
                renderAnnotatedSelection(background, history.annotations(), background.rect());
            QVERIFY(output != steps.last());
            steps.append(output);
        }
        for (int index = 6; index >= 0; --index)
        {
            QVERIFY(history.undo());
            QCOMPARE(renderAnnotatedSelection(background, history.annotations(), background.rect()),
                     steps[index]);
        }
        for (int index = 1; index <= 7; ++index)
        {
            QVERIFY(history.redo());
            QCOMPARE(renderAnnotatedSelection(background, history.annotations(), background.rect()),
                     steps[index]);
        }
    }
    void sharedPainterMatchesPhysicalExport()
    {
        using namespace waibusnap;
        QImage background(300, 220, QImage::Format_ARGB32_Premultiplied);
        background.fill(Qt::white);
        QVector<Annotation> annotations;
        for (int type = 0; type < 7; ++type)
        {
            Annotation annotation{
                static_cast<AnnotationType>(type), {}, {20, qreal(20 + type * 25)},
                {80, qreal(35 + type * 25)},       {}, {}};
            annotation.points = {annotation.first, annotation.last};
            annotation.text = QStringLiteral("说明 ABC\n123");
            annotations.append(annotation);
        }
        QImage preview = background;
        preview.setDevicePixelRatio(2);
        {
            QPainter painter(&preview);
            painter.scale(0.5, 0.5);
            paintAnnotations(painter, annotations, background);
        }
        preview.setDevicePixelRatio(1);
        QCOMPARE(renderAnnotatedSelection(background, annotations, background.rect()), preview);
        QCOMPARE(renderAnnotatedSelection(background, annotations, {10, 10, 120, 180}),
                 preview.copy(10, 10, 120, 180));
    }
    void stylesChangePhysicalResults()
    {
        using namespace waibusnap;
        QImage background(220, 150, QImage::Format_ARGB32_Premultiplied);
        background.fill(Qt::white);
        QImage previousLine;
        QImage previousText;
        for (int index = 0; index < 3; ++index)
        {
            Annotation line{AnnotationType::Line, {}, {10, 30}, {210, 30}, {}, {}};
            line.style.color = annotationColors()[index];
            line.style.lineWidth = annotationLineWidths[index];
            const auto drawn = renderAnnotatedSelection(background, {line}, background.rect());
            QCOMPARE(drawn.pixelColor(100, 30), annotationColors()[index]);
            int rows = 0;
            for (int y = 0; y < drawn.height(); ++y)
                if (drawn.pixelColor(100, y) != QColor(Qt::white))
                    ++rows;
            QCOMPARE(rows, annotationLineWidths[index]);
            QVERIFY(drawn != previousLine);
            previousLine = drawn;
            Annotation text{
                AnnotationType::Text, {}, {10, 10}, {}, {}, QStringLiteral("ABC 123\n第二行")};
            text.style.textSize = annotationTextSizes[index];
            const auto textImage = renderAnnotatedSelection(background, {text}, background.rect());
            QVERIFY(textImage != background);
            QVERIFY(textImage != previousText);
            previousText = textImage;
        }
    }
    void cropKeepsPhysicalPixels()
    {
        QImage source(12, 10, QImage::Format_ARGB32);
        for (int y = 0; y < source.height(); ++y)
            for (int x = 0; x < source.width(); ++x)
                source.setPixelColor(x, y, QColor(x * 10, y * 10, 50));
        source.setDevicePixelRatio(2);
        const QImage cropped = waibusnap::cropFrozenSelection(source, {2, 3, 5, 4});
        QCOMPARE(cropped.size(), QSize(5, 4));
        QCOMPARE(cropped.devicePixelRatio(), qreal(1));
        QCOMPARE(source.devicePixelRatio(), qreal(2));
        for (int y = 0; y < cropped.height(); ++y)
            for (int x = 0; x < cropped.width(); ++x)
                QCOMPARE(cropped.pixelColor(x, y), source.pixelColor(x + 2, y + 3));
        QVERIFY(waibusnap::cropFrozenSelection(source, {}).isNull());
        QVERIFY(waibusnap::cropFrozenSelection(source, {-1, 0, 5, 4}).isNull());
        QVERIFY(waibusnap::cropFrozenSelection(source, {10, 9, 5, 4}).isNull());
    }
    void pngRoundTripSupportsChineseAndSpaces()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString directory = temporary.filePath(QStringLiteral("中文 目录"));
        QVERIFY(QDir().mkpath(directory));
        const QString path = QDir(directory).filePath(QStringLiteral("截图 文件"));
        QImage image(37, 29, QImage::Format_ARGB32);
        image.fill(QColor(20, 60, 100, 180));
        image.setPixelColor(7, 5, Qt::green);
        image.setDevicePixelRatio(2);
        const auto result = waibusnap::exportPngToPath(image, path);
        QVERIFY2(result.success, qPrintable(result.explanation));
        QImageReader reader(path + QStringLiteral(".png"));
        QCOMPARE(reader.format(), QByteArray("png"));
        QCOMPARE(reader.size(), QSize(37, 29));
        const QImage loaded = reader.read();
        QCOMPARE(loaded.devicePixelRatio(), qreal(1));
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                QCOMPARE(loaded.pixelColor(x, y), image.pixelColor(x, y));
        QCOMPARE(image.devicePixelRatio(), qreal(2));
    }
    void failuresPreserveExistingFile()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QImage image(8, 6, QImage::Format_RGB32);
        image.fill(Qt::blue);
        const QString missing = temporary.filePath(QStringLiteral("不存在/图.png"));
        const auto absent = waibusnap::exportPngToPath(image, missing);
        QVERIFY(!absent.success);
        QVERIFY(!absent.explanation.isEmpty());
        QVERIFY(!QFileInfo::exists(missing));
        const QString path = temporary.filePath(QStringLiteral("只读.png"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray original("原文件内容");
        QCOMPARE(file.write(original), qint64(original.size()));
        file.close();
        QVERIFY(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::ReadUser |
                                                QFileDevice::ReadGroup | QFileDevice::ReadOther));
        const auto denied = waibusnap::exportPngToPath(image, path);
        const bool restored =
            QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                            QFileDevice::ReadUser | QFileDevice::WriteUser);
        QVERIFY(restored);
        QVERIFY(!denied.success);
        QVERIFY(!denied.explanation.isEmpty());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), original);
        file.close();
        QVERIFY(!waibusnap::exportPngToPath({}, path).success);
        QVERIFY(!waibusnap::exportPngToPath(image, {}).success);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), original);
    }
    void suggestedNamesDoNotCollide()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QDateTime time(QDate(2026, 10, 2), QTime(12, 30, 45, 123));
        const QString first = waibusnap::suggestedPngPath(temporary.path(), time);
        QVERIFY(first.endsWith(QStringLiteral("WaibuSnap_2026-10-02_12-30-45-123.png")));
        QFile existing(first);
        QVERIFY(existing.open(QIODevice::WriteOnly));
        existing.close();
        const QString second = waibusnap::suggestedPngPath(temporary.path(), time);
        QVERIFY(second != first);
        QVERIFY(!QFileInfo::exists(second));
        QCOMPARE(waibusnap::pngFilePath(QStringLiteral("图.PNG")), QStringLiteral("图.PNG"));
        QCOMPARE(waibusnap::pngFilePath(QStringLiteral("图.jpg")), QStringLiteral("图.jpg.png"));
        QVERIFY(waibusnap::pngFilePath({}).isEmpty());
    }
};
QTEST_MAIN(ImageOutputTest)
#include "image_output_test.moc"
