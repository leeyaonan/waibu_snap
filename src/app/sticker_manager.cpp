#include "app/sticker_manager.h"
#include "core/sticker_geometry.h"
#include "interfaces/sticker_window_behavior.h"
#include <QGuiApplication>
#include <QScreen>
#include <QTimer>
#include <QWindow>
namespace waibusnap
{
StickerManager::StickerManager(QObject* parent, StickerActions actions)
    : QObject(parent), actions_(std::move(actions))
{
    for (QScreen* screen : QGuiApplication::screens())
        watchScreen(screen);
    connect(qGuiApp, &QGuiApplication::screenAdded, this,
            [this](QScreen* screen)
            {
                watchScreen(screen);
                QTimer::singleShot(0, this, &StickerManager::recoverWindows);
            });
    connect(qGuiApp, &QGuiApplication::screenRemoved, this,
            [this]
            {
                // Qt 完成原生窗口的屏幕迁移后再找回，不保留已移除的屏幕指针。
                QTimer::singleShot(0, this, &StickerManager::recoverWindows);
            });
}
StickerManager::~StickerManager() { closeAll(); }
void StickerManager::watchScreen(QScreen* screen)
{
    connect(screen, &QScreen::geometryChanged, this,
            [this] { QTimer::singleShot(0, this, &StickerManager::recoverWindows); });
    connect(screen, &QScreen::availableGeometryChanged, this,
            [this] { QTimer::singleShot(0, this, &StickerManager::recoverWindows); });
    connect(screen, &QScreen::logicalDotsPerInchChanged, this,
            [this] { QTimer::singleShot(0, this, &StickerManager::recoverWindows); });
}
ImageOutputResult StickerManager::create(const QImage& image, QPoint position, bool saved)
{
    if (closingAll_ || image.isNull() || stickerWindowSize(image.size(), 1, 1).isEmpty())
        return {false, QStringLiteral("贴图图像或窗口尺寸无效。")};
    auto* window = new StickerWindow(image, position, saved, actions_);
    if (!configureStickerWindowBehavior(window->windowHandle()))
    {
        delete window;
        return {false, QStringLiteral("无法配置不抢焦点的贴图窗口，请重试。")};
    }
    windows_.append(window);
    ownedWindows_.append(window);
    connect(window, &StickerWindow::closed, this, [this, window] { windows_.removeAll(window); });
    connect(window, &QObject::destroyed, this,
            [this]
            {
                windows_.removeAll(nullptr);
                ownedWindows_.removeAll(nullptr);
            });
    window->show();
    if (!window->isVisible())
    {
        delete window;
        return {false, QStringLiteral("无法显示贴图窗口，请重试。")};
    }
    return {true, {}};
}
void StickerManager::closeAll()
{
    if (closingAll_)
        return;
    closingAll_ = true;
    const auto windows = ownedWindows_;
    for (const auto& window : windows)
        if (window)
        {
            window->close();
            if (window)
                delete window.data();
        }
    windows_.clear();
    ownedWindows_.clear();
    closingAll_ = false;
}
void StickerManager::recoverWindows()
{
    QScreen* primary = QGuiApplication::primaryScreen();
    if (!primary)
        return;
    QVector<QRect> screens;
    for (QScreen* screen : QGuiApplication::screens())
        screens.append(screen->availableGeometry());
    for (const auto& window : windows_)
        if (window)
        {
            const QPoint recovered =
                recoveredStickerPosition(window->geometry(), screens, primary->availableGeometry());
            if (recovered != window->pos())
            {
                window->windowHandle()->setScreen(primary);
                window->refreshScreenGeometry();
                window->move(recoveredStickerPosition(QRect(recovered, window->size()), {},
                                                      primary->availableGeometry()));
            }
            else
                window->refreshScreenGeometry();
        }
}
}
