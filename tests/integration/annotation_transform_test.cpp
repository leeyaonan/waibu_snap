#include "core/annotation.h"
#include "interfaces/capture_provider.h"
#include "output/annotation_renderer.h"
#include "ui/annotation_text_edit.h"
#include "ui/selection_overlay.h"
#include "ui/sticker_window.h"
#include <QApplication>
#include <QPushButton>
#include <QTest>
#include <cmath>

namespace
{
using namespace waibusnap;

CaptureFrame frame(qreal dpr = 1)
{
    CaptureFrame result;
    result.display.logicalGeometry = QRect(0, 0, 800, 600);
    result.display.availableLogicalGeometry = result.display.logicalGeometry;
    result.display.devicePixelRatio = dpr;
    result.pixels = QImage(QSize(800, 600) * dpr, QImage::Format_ARGB32_Premultiplied);
    result.pixels.fill(Qt::white);
    result.pixels.setDevicePixelRatio(dpr);
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

QPointF centerOf(const Annotation& annotation) { return annotationBaseBounds(annotation).center(); }

QRect inkBounds(const QImage& image)
{
    int left = image.width();
    int top = image.height();
    int right = -1;
    int bottom = -1;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if (image.pixelColor(x, y) != QColor(Qt::white))
            {
                left = std::min(left, x);
                top = std::min(top, y);
                right = std::max(right, x);
                bottom = std::max(bottom, y);
            }
    return right < left ? QRect() : QRect(QPoint(left, top), QPoint(right, bottom));
}
}

class AnnotationTransformTest final : public QObject
{
    Q_OBJECT
  private slots:
    void rendererAppliesRotation()
    {
        QImage source(160, 120, QImage::Format_ARGB32_Premultiplied);
        source.fill(Qt::white);
        Annotation rectangle;
        rectangle.type = AnnotationType::Rectangle;
        rectangle.style.color = Qt::red;
        rectangle.style.lineWidth = 4;
        rectangle.first = {30, 50};
        rectangle.last = {130, 70};
        const QImage horizontal =
            renderAnnotatedSelection(source, QVector<Annotation>{rectangle}, source.rect());
        setAnnotationRotation(rectangle, 90);
        const QImage vertical =
            renderAnnotatedSelection(source, QVector<Annotation>{rectangle}, source.rect());
        QVERIFY(horizontal != vertical);

        Annotation text;
        text.type = AnnotationType::Text;
        text.style.color = Qt::black;
        text.style.textSize = 20;
        text.text = QStringLiteral("旋转文本");
        text.first = {30, 30};
        const QImage textHorizontal =
            renderAnnotatedSelection(source, QVector<Annotation>{text}, source.rect());
        setAnnotationRotation(text, 90);
        const QImage textRotated =
            renderAnnotatedSelection(source, QVector<Annotation>{text}, source.rect());
        QVERIFY(textHorizontal != textRotated);
        const QRect horizontalInk = inkBounds(textHorizontal);
        const QRect rotatedInk = inkBounds(textRotated);
        QVERIFY(horizontalInk.width() > horizontalInk.height());
        QVERIFY(rotatedInk.height() > rotatedInk.width());
    }
    void overlaySelectMoveRotate()
    {
        SelectionOverlay overlay(frame());
        overlay.show();
        overlay.activateWindow();
        QTest::qWait(1);
        drag(overlay, {0, 0}, {800, 600});
        QVERIFY(!overlay.selection().isEmpty());

        QTest::mouseClick(button(overlay, "rectangleToolButton"), Qt::LeftButton);
        drag(overlay, {100, 100}, {300, 200});
        QCOMPARE(overlay.annotations().size(), qsizetype(1));
        const Annotation original = overlay.annotations().first();

        QTest::mouseClick(button(overlay, "selectToolButton"), Qt::LeftButton);
        QVERIFY(overlay.isSelectMode());
        QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, {200, 150});
        QCOMPARE(overlay.selectedAnnotation(), 0);

        drag(overlay, {200, 150}, {240, 180});
        const Annotation moved = overlay.annotations().first();
        QCOMPARE(moved.first, original.first + QPointF(40, 30));
        QCOMPARE(moved.last, original.last + QPointF(40, 30));

        QTest::mouseClick(button(overlay, "undoAnnotationButton"), Qt::LeftButton);
        QCOMPARE(overlay.annotations().first().first, original.first);
        QTest::mouseClick(button(overlay, "redoAnnotationButton"), Qt::LeftButton);
        QCOMPARE(overlay.annotations().first().first, moved.first);

        // 选择状态在撤销 / 重做后清除；重新点选后拖动旋转手柄。
        QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, {240, 180});
        QCOMPARE(overlay.selectedAnnotation(), 0);
        const Annotation beforeRotate = overlay.annotations().first();
        const QPointF center = centerOf(beforeRotate);
        const QPointF handle = annotationRotationHandle(beforeRotate, 24);
        QTest::mousePress(&overlay, Qt::LeftButton, Qt::NoModifier, handle.toPoint());
        QTest::mouseMove(&overlay, (center + QPointF(100, 0)).toPoint());
        QTest::mouseRelease(&overlay, Qt::LeftButton, Qt::NoModifier,
                            (center + QPointF(100, 0)).toPoint());
        const Annotation rotated = overlay.annotations().first();
        QVERIFY(std::abs(rotated.rotation - 90) < 2);
        QCOMPARE(rotated.first, beforeRotate.first);
        QTest::mouseClick(button(overlay, "undoAnnotationButton"), Qt::LeftButton);
        QCOMPARE(overlay.annotations().first().rotation, beforeRotate.rotation);
        QTest::mouseClick(button(overlay, "redoAnnotationButton"), Qt::LeftButton);
        QVERIFY(std::abs(overlay.annotations().first().rotation - 90) < 2);

        QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, {240, 180});
        QCOMPARE(overlay.selectedAnnotation(), 0);
        QTest::keyClick(&overlay, Qt::Key_Escape);
        QCOMPARE(overlay.selectedAnnotation(), -1);
        QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, {700, 500});
        QCOMPARE(overlay.selectedAnnotation(), -1);
    }

    void overlayHighDpiSelectMove()
    {
        SelectionOverlay overlay(frame(2));
        overlay.show();
        overlay.activateWindow();
        QTest::qWait(1);
        drag(overlay, {0, 0}, {800, 600});
        QTest::mouseClick(button(overlay, "rectangleToolButton"), Qt::LeftButton);
        drag(overlay, {100, 100}, {300, 200});
        QCOMPARE(overlay.annotations().size(), qsizetype(1));
        const Annotation original = overlay.annotations().first();
        QCOMPARE(original.first, QPointF(200, 200));
        QCOMPARE(original.last, QPointF(600, 400));

        QTest::mouseClick(button(overlay, "selectToolButton"), Qt::LeftButton);
        QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, {200, 150});
        QCOMPARE(overlay.selectedAnnotation(), 0);
        drag(overlay, {200, 150}, {240, 180});
        QCOMPARE(overlay.annotations().first().first, original.first + QPointF(80, 60));
        QCOMPARE(overlay.annotations().first().last, original.last + QPointF(80, 60));

        const Annotation beforeRotate = overlay.annotations().first();
        const QPointF handle = annotationRotationHandle(beforeRotate, 48) / 2;
        const QPointF center = centerOf(beforeRotate) / 2;
        QTest::mousePress(&overlay, Qt::LeftButton, Qt::NoModifier, handle.toPoint());
        QTest::mouseMove(&overlay, (center + QPointF(100, 0)).toPoint());
        QTest::mouseRelease(&overlay, Qt::LeftButton, Qt::NoModifier,
                            (center + QPointF(100, 0)).toPoint());
        QVERIFY(std::abs(overlay.annotations().first().rotation - 90) < 2);
    }
    void overlayTextSelectMove()
    {
        SelectionOverlay overlay(frame());
        overlay.show();
        overlay.activateWindow();
        QTest::qWait(1);
        drag(overlay, {0, 0}, {800, 600});
        QTest::mouseClick(button(overlay, "textToolButton"), Qt::LeftButton);
        QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, {400, 300});
        auto* editor = overlay.findChild<AnnotationTextEdit*>();
        QVERIFY(editor && editor->isVisible());
        editor->setPlainText(QStringLiteral("可拖动文本 ABC"));
        QTest::keyClick(editor, Qt::Key_Return, Qt::ControlModifier);
        QCOMPARE(overlay.annotations().size(), qsizetype(1));
        QCOMPARE(overlay.activeTool(), std::optional<AnnotationType>(AnnotationType::Text));
        QCOMPARE(overlay.selectedAnnotation(), 0);
        const Annotation original = overlay.annotations().first();
        const QRect frameArea = annotationBaseBounds(original).adjusted(-8, -8, 8, 8).toRect();
        const QImage screenshot = overlay.grab().toImage();
        int framePixels = 0;
        for (int y = frameArea.top(); y <= frameArea.bottom(); ++y)
            for (int x = frameArea.left(); x <= frameArea.right(); ++x)
                if (screenshot.rect().contains(x, y))
                {
                    const QColor color = screenshot.pixelColor(x, y);
                    if (color.red() > 200 && color.green() > 70 && color.green() < 180 &&
                        color.blue() < 90)
                        ++framePixels;
                }
        QVERIFY2(framePixels > 0, "文本框提交后应显示橙色选框");
        const QPointF center = centerOf(original);
        drag(overlay, center.toPoint(), (center + QPointF(30, 20)).toPoint());
        QCOMPARE(overlay.annotations().first().first, original.first + QPointF(30, 20));
    }
    void overlayTextCommitOutsideKeepsSelection()
    {
        SelectionOverlay overlay(frame());
        overlay.show();
        overlay.activateWindow();
        QTest::qWait(1);
        drag(overlay, {0, 0}, {800, 600});
        QTest::mouseClick(button(overlay, "textToolButton"), Qt::LeftButton);
        QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, {400, 300});
        auto* editor = overlay.findChild<AnnotationTextEdit*>();
        QVERIFY(editor && editor->isVisible());
        editor->setPlainText(QStringLiteral("点外提交后保留选框"));
        // 点画布空白处提交：应当只提交并保留当前文本框选中，不再顺手开新文本。
        QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, {700, 500});
        QCOMPARE(overlay.annotations().size(), qsizetype(1));
        QCOMPARE(overlay.selectedAnnotation(), 0);
        QCOMPARE(overlay.activeTool(), std::optional<AnnotationType>(AnnotationType::Text));
        QVERIFY(!editor->isVisible());
        const Annotation committed = overlay.annotations().first();
        const QRect frameArea = annotationBaseBounds(committed).adjusted(-8, -8, 8, 8).toRect();
        const QImage screenshot = overlay.grab().toImage();
        int framePixels = 0;
        for (int y = frameArea.top(); y <= frameArea.bottom(); ++y)
            for (int x = frameArea.left(); x <= frameArea.right(); ++x)
                if (screenshot.rect().contains(x, y))
                {
                    const QColor color = screenshot.pixelColor(x, y);
                    if (color.red() > 200 && color.green() > 70 && color.green() < 180 &&
                        color.blue() < 90)
                        ++framePixels;
                }
        QVERIFY2(framePixels > 0, "点外提交后应保留文本框橙色选框");
        const QPointF center = centerOf(committed);
        drag(overlay, center.toPoint(), (center + QPointF(30, 20)).toPoint());
        QCOMPARE(overlay.annotations().first().first, committed.first + QPointF(30, 20));
    }
    void overlayCurrentObjectContinuous()
    {
        SelectionOverlay overlay(frame());
        overlay.show();
        overlay.activateWindow();
        QTest::qWait(1);
        drag(overlay, {0, 0}, {800, 600});
        QTest::mouseClick(button(overlay, "rectangleToolButton"), Qt::LeftButton);
        drag(overlay, {100, 100}, {300, 200});
        QCOMPARE(overlay.annotations().size(), qsizetype(1));
        QCOMPARE(overlay.selectedAnnotation(), 0);
        QCOMPARE(overlay.activeTool(), std::optional<AnnotationType>(AnnotationType::Rectangle));
        const Annotation original = overlay.annotations().first();

        // 不切工具：直接拖当前对象。
        drag(overlay, {200, 150}, {240, 180});
        QCOMPARE(overlay.annotations().first().first, original.first + QPointF(40, 30));
        QCOMPARE(overlay.activeTool(), std::optional<AnnotationType>(AnnotationType::Rectangle));
        QCOMPARE(overlay.selectedAnnotation(), 0);

        // 不切工具：直接拖旋转手柄。
        const Annotation beforeRotate = overlay.annotations().first();
        const QPointF center = centerOf(beforeRotate);
        const QPointF handle = annotationRotationHandle(beforeRotate, 24);
        QTest::mousePress(&overlay, Qt::LeftButton, Qt::NoModifier, handle.toPoint());
        QTest::mouseMove(&overlay, (center + QPointF(100, 0)).toPoint());
        QTest::mouseRelease(&overlay, Qt::LeftButton, Qt::NoModifier,
                            (center + QPointF(100, 0)).toPoint());
        QVERIFY(std::abs(overlay.annotations().first().rotation - 90) < 2);
        QCOMPARE(overlay.activeTool(), std::optional<AnnotationType>(AnnotationType::Rectangle));

        // 点外侧继续画新对象，当前对象自动切换为新的最后一个。
        drag(overlay, {500, 300}, {600, 400});
        QCOMPARE(overlay.annotations().size(), qsizetype(2));
        QCOMPARE(overlay.selectedAnnotation(), 1);
    }
    void stickerTextCommitOutsideKeepsSelection()
    {
        QImage image(800, 600, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::white);
        StickerWindow sticker(image, {40, 50}, true);
        sticker.show();
        sticker.setEditing(true);
        QTest::qWait(1);
        QTest::mouseClick(button(sticker, "stickerEditTextToolButton"), Qt::LeftButton);
        QTest::mouseClick(&sticker, Qt::LeftButton, Qt::NoModifier, {200, 200});
        auto* editor = sticker.findChild<AnnotationTextEdit*>();
        QVERIFY(editor && editor->isVisible());
        editor->setPlainText(QStringLiteral("贴图文本"));
        QTest::mouseClick(&sticker, Qt::LeftButton, Qt::NoModifier, {700, 500});
        QCOMPARE(sticker.annotations().size(), qsizetype(1));
        QCOMPARE(sticker.selectedAnnotation(), 0);
        QCOMPARE(sticker.activeTool(), std::optional<AnnotationType>(AnnotationType::Text));
        QVERIFY(!editor->isVisible());
        const Annotation committed = sticker.annotations().first();
        const QPointF center = centerOf(committed);
        drag(sticker, center.toPoint(), (center + QPointF(30, 20)).toPoint());
        QCOMPARE(sticker.annotations().first().first, committed.first + QPointF(30, 20));
    }
    void stickerCurrentObjectContinuous()
    {
        QImage image(800, 600, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::white);
        StickerWindow sticker(image, {40, 50}, true);
        sticker.show();
        sticker.setEditing(true);
        QTest::qWait(1);
        QTest::mouseClick(button(sticker, "stickerEditRectangleToolButton"), Qt::LeftButton);
        drag(sticker, {100, 100}, {300, 200});
        QCOMPARE(sticker.annotations().size(), qsizetype(1));
        QCOMPARE(sticker.selectedAnnotation(), 0);
        QCOMPARE(sticker.activeTool(), std::optional<AnnotationType>(AnnotationType::Rectangle));
        const Annotation original = sticker.annotations().first();

        drag(sticker, {200, 150}, {240, 180});
        QCOMPARE(sticker.annotations().first().first, original.first + QPointF(40, 30));
        QCOMPARE(sticker.activeTool(), std::optional<AnnotationType>(AnnotationType::Rectangle));
        QCOMPARE(sticker.selectedAnnotation(), 0);

        const Annotation beforeRotate = sticker.annotations().first();
        const QPointF center = centerOf(beforeRotate);
        const QPointF handle = annotationRotationHandle(beforeRotate, 24);
        QTest::mousePress(&sticker, Qt::LeftButton, Qt::NoModifier, handle.toPoint());
        QTest::mouseMove(&sticker, (center + QPointF(100, 0)).toPoint());
        QTest::mouseRelease(&sticker, Qt::LeftButton, Qt::NoModifier,
                            (center + QPointF(100, 0)).toPoint());
        QVERIFY(std::abs(sticker.annotations().first().rotation - 90) < 2);
        QCOMPARE(sticker.activeTool(), std::optional<AnnotationType>(AnnotationType::Rectangle));

        drag(sticker, {500, 300}, {600, 400});
        QCOMPARE(sticker.annotations().size(), qsizetype(2));
        QCOMPARE(sticker.selectedAnnotation(), 1);
    }
    void stickerScaledSelectMove()
    {
        QImage image(800, 600, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::white);
        StickerWindow sticker(image, {40, 50}, true, {}, 0.5);
        sticker.show();
        sticker.setEditing(true);
        QTest::qWait(1);
        QVERIFY(sticker.isEditing());
        const qreal factor = sticker.width() / qreal(image.width());
        QVERIFY(std::abs(factor - 0.5) < 0.01);

        QTest::mouseClick(button(sticker, "stickerEditRectangleToolButton"), Qt::LeftButton);
        drag(sticker, {50, 50}, {150, 100});
        QCOMPARE(sticker.annotations().size(), qsizetype(1));
        const Annotation original = sticker.annotations().first();
        QCOMPARE(original.first, QPointF(100, 100));
        QCOMPARE(original.last, QPointF(300, 200));

        QTest::mouseClick(button(sticker, "stickerEditSelectToolButton"), Qt::LeftButton);
        QTest::mouseClick(&sticker, Qt::LeftButton, Qt::NoModifier, {100, 75});
        QCOMPARE(sticker.selectedAnnotation(), 0);
        drag(sticker, {100, 75}, {120, 90});
        QCOMPARE(sticker.annotations().first().first, original.first + QPointF(40, 30));

        const Annotation beforeRotate = sticker.annotations().first();
        const QPointF handle = annotationRotationHandle(beforeRotate, 24 / factor) * factor;
        const QPointF center = centerOf(beforeRotate) * factor;
        QTest::mousePress(&sticker, Qt::LeftButton, Qt::NoModifier, handle.toPoint());
        QTest::mouseMove(&sticker, (center + QPointF(60, 0)).toPoint());
        QTest::mouseRelease(&sticker, Qt::LeftButton, Qt::NoModifier,
                            (center + QPointF(60, 0)).toPoint());
        QVERIFY(std::abs(sticker.annotations().first().rotation - 90) < 2);
    }
    void stickerSelectMoveRotate()
    {
        QImage image(800, 600, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::white);
        StickerWindow sticker(image, {40, 50}, true);
        sticker.show();
        sticker.setEditing(true);
        QTest::qWait(1);
        QVERIFY(sticker.isEditing());

        QTest::mouseClick(button(sticker, "stickerEditRectangleToolButton"), Qt::LeftButton);
        drag(sticker, {100, 100}, {300, 200});
        QCOMPARE(sticker.annotations().size(), qsizetype(1));
        const Annotation original = sticker.annotations().first();

        QTest::mouseClick(button(sticker, "stickerEditSelectToolButton"), Qt::LeftButton);
        QVERIFY(sticker.isSelectMode());
        QTest::mouseClick(&sticker, Qt::LeftButton, Qt::NoModifier, {200, 150});
        QCOMPARE(sticker.selectedAnnotation(), 0);

        drag(sticker, {200, 150}, {240, 180});
        const Annotation moved = sticker.annotations().first();
        QCOMPARE(moved.first, original.first + QPointF(40, 30));
        QCOMPARE(moved.last, original.last + QPointF(40, 30));

        QTest::mouseClick(button(sticker, "stickerEditUndoAnnotationButton"), Qt::LeftButton);
        QCOMPARE(sticker.annotations().first().first, original.first);
        QTest::mouseClick(button(sticker, "stickerEditRedoAnnotationButton"), Qt::LeftButton);
        QCOMPARE(sticker.annotations().first().first, moved.first);

        QTest::mouseClick(&sticker, Qt::LeftButton, Qt::NoModifier, {240, 180});
        QCOMPARE(sticker.selectedAnnotation(), 0);
        const Annotation beforeRotate = sticker.annotations().first();
        const QPointF center = centerOf(beforeRotate);
        const QPointF handle = annotationRotationHandle(beforeRotate, 24);
        QTest::mousePress(&sticker, Qt::LeftButton, Qt::NoModifier, handle.toPoint());
        QTest::mouseMove(&sticker, (center + QPointF(100, 0)).toPoint());
        QTest::mouseRelease(&sticker, Qt::LeftButton, Qt::NoModifier,
                            (center + QPointF(100, 0)).toPoint());
        const Annotation rotated = sticker.annotations().first();
        QVERIFY(std::abs(rotated.rotation - 90) < 2);
        QCOMPARE(rotated.first, beforeRotate.first);
        QTest::mouseClick(&sticker, Qt::LeftButton, Qt::NoModifier, {240, 180});
        QCOMPARE(sticker.selectedAnnotation(), 0);
        QTest::keyClick(&sticker, Qt::Key_Escape);
        QCOMPARE(sticker.selectedAnnotation(), -1);
    }
};

QTEST_MAIN(AnnotationTransformTest)
#include "annotation_transform_test.moc"
