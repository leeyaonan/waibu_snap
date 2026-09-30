#pragma once

#include <QWidget>

namespace waibusnap
{
class StartupWindow final : public QWidget
{
  public:
    explicit StartupWindow(QWidget* parent = nullptr);
};
}
