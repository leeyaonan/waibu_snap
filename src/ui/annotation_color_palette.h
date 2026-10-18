#pragma once
#include <QVector>
#include <QWidget>
#include <functional>
class QColor;
class QPushButton;
namespace waibusnap
{
// 两编辑面使用相同尺寸、颜色顺序、可访问元数据和选中态。
class AnnotationColorPalette final : public QWidget
{
  public:
    AnnotationColorPalette(QWidget* parent, const QString& prefix, std::function<void(int)> choose);
    void setColor(const QColor& color);

  private:
    QVector<QPushButton*> buttons_;
};
}
