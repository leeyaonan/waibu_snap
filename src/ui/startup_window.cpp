#include "ui/startup_window.h"

namespace waibusnap
{
StartupWindow::StartupWindow(QWidget* parent) : QWidget(parent)
{
    setWindowTitle(QStringLiteral("WaibuSnap"));
    resize(640, 400);
}
}
