#include "app/sticker_manager.h"
#include "core/sticker_geometry.h"
#include "interfaces/sticker_window_behavior.h"
#include <QGuiApplication>
#include <QMessageBox>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QScreen>
#include <QTimer>
#include <QWindow>
namespace waibusnap
{
StickerManager::StickerManager(QObject* parent, StickerActions actions)
    : QObject(parent), actions_(std::move(actions))
{
    if (!actions_.confirmQuit)
        actions_.confirmQuit = [](int count)
        {
            QMessageBox dialog(
                QMessageBox::Question, QStringLiteral("退出 WaibuSnap"),
                QStringLiteral(
                    "还有 %1 张未保存贴图。请选择保存或放弃。未保存图片不会在重启后恢复。")
                    .arg(count),
                QMessageBox::NoButton);
            auto* cancel = dialog.addButton(QStringLiteral("取消退出"), QMessageBox::RejectRole);
            auto* save = dialog.addButton(QStringLiteral("逐张保存"), QMessageBox::AcceptRole);
            dialog.addButton(QStringLiteral("全部放弃"), QMessageBox::DestructiveRole);
            dialog.setDefaultButton(cancel);
            dialog.setEscapeButton(cancel);
            dialog.exec();
            return dialog.clickedButton() == save ? StickerQuitDecision::SaveAll
                   : dialog.clickedButton() == cancel || !dialog.clickedButton()
                       ? StickerQuitDecision::Cancel
                       : StickerQuitDecision::DiscardAll;
        };
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
    if (closingAll_ || resolvingQuit_ || image.isNull() ||
        stickerWindowSize(image.size(), 1, 1).isEmpty())
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
void StickerManager::hideAll()
{
    if (closingAll_ || resolvingQuit_)
        return;
    const auto windows = windows_;
    for (const auto& window : windows)
        if (window && window->isVisible())
        {
            // 与保存前相同地提交文本；隐藏不关闭，也不修改保存状态。
            window->setEditing(false);
            if (window)
                window->hide();
        }
}
void StickerManager::restoreAll()
{
    if (closingAll_ || resolvingQuit_)
        return;
    const auto windows = windows_;
    for (const auto& window : windows)
        if (window && !window->isVisible())
            window->show();
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
            window->forceClose();
            if (window)
                delete window.data();
        }
    windows_.clear();
    ownedWindows_.clear();
    closingAll_ = false;
}
bool StickerManager::resolveUnsavedForQuit()
{
    if (resolvingQuit_ || closingAll_)
        return false;
    // 关闭确认 / 保存面板嵌套循环中再退出，明确中止且不创建第二个对话框。
    for (const auto& window : windows_)
        if (window && window->hasPendingInteraction())
            return false;
    QScopedValueRollback<bool> resolving(resolvingQuit_, true);
    const auto snapshot = windows_;
    QVector<QPointer<StickerWindow>> unsaved;
    for (const auto& window : snapshot)
        if (window)
        {
            window->setEditing(false);
            if (!window->isSaved())
                unsaved.append(window);
            window->setQuitInteraction(true);
        }
    bool resolved = true;
    if (!unsaved.isEmpty())
    {
        const auto confirm = actions_.confirmQuit;
        const auto decision = confirm(int(unsaved.size()));
        resolved = decision != StickerQuitDecision::Cancel;
        if (decision == StickerQuitDecision::SaveAll)
            for (const auto& window : unsaved)
                if (window && !window->isSaved() && !window->saveImage())
                {
                    resolved = false;
                    break;
                }
    }
    for (const auto& window : snapshot)
        if (window)
            window->setQuitInteraction(false);
    return resolved;
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
