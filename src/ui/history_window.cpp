#include "ui/history_window.h"
#include "ui/group_settings_dialog.h"
#include <QCloseEvent>
#include <QComboBox>
#include <QDataStream>
#include <QDateTime>
#include <QDebug>
#include <QDialogButtonBox>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStandardItemModel>
#include <QStyleOptionGraphicsItem>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
namespace waibusnap
{
class HistoryImageItem final : public QGraphicsItem
{
  public:
    HistoryImageItem() { setFlag(QGraphicsItem::ItemUsesExtendedStyleOption); }
    QImage image;
    QRectF boundingRect() const override { return image.rect(); }
    void replace(QImage value)
    {
        prepareGeometryChange();
        image = std::move(value);
        update();
    }
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget*) override
    {
        // 只绘制可见源区域，不生成与整张长图等大的缩放副本。
        const QRectF visible = option->exposedRect.intersected(boundingRect());
        painter->drawImage(visible, image, visible);
    }
};
HistoryImageView::HistoryImageView(QWidget* parent) : QGraphicsView(parent)
{
    setObjectName(QStringLiteral("historyImageView"));
    setScene(new QGraphicsScene(this));
    item_ = new HistoryImageItem;
    scene()->addItem(item_);
    setDragMode(QGraphicsView::ScrollHandDrag);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    setRenderHint(QPainter::SmoothPixmapTransform);
}
void HistoryImageView::setImage(QImage image)
{
    image.setDevicePixelRatio(1);
    item_->replace(std::move(image));
    scene()->setSceneRect(item_->boundingRect());
    fitImage();
}
QImage HistoryImageView::image() const { return item_->image; }
void HistoryImageView::fitImage()
{
    fitting_ = true;
    resetTransform();
    if (!item_->image.isNull())
        fitInView(item_->boundingRect(), Qt::KeepAspectRatio);
}
void HistoryImageView::actualSize()
{
    fitting_ = false;
    resetTransform();
}
void HistoryImageView::wheelEvent(QWheelEvent* event)
{
    const int delta = event->angleDelta().y();
    if (delta)
    {
        fitting_ = false;
        const qreal next =
            std::clamp(zoom() * std::pow(1.15, delta / 120.0), qreal(0.001), qreal(8));
        scale(next / zoom(), next / zoom());
    }
    event->accept();
}
void HistoryImageView::resizeEvent(QResizeEvent* event)
{
    QGraphicsView::resizeEvent(event);
    if (fitting_)
        fitImage();
}
void HistoryImageView::drawBackground(QPainter* painter, const QRectF&)
{
    // 棋盘格保持逻辑像素大小，缩放图片时仍可辨识透明区域。
    QPixmap tile(24, 24);
    tile.fill(QColor(238, 238, 238));
    QPainter grid(&tile);
    grid.fillRect(0, 0, 12, 12, QColor(205, 205, 205));
    grid.fillRect(12, 12, 12, 12, QColor(205, 205, 205));
    grid.end();
    painter->save();
    painter->resetTransform();
    painter->fillRect(viewport()->rect(), QBrush(tile));
    painter->restore();
}
HistoryWindow::HistoryWindow(HistoryManager& manager, AppSettings settings,
                             HistoryWindowActions actions)
    : manager_(manager), settings_(std::move(settings)), actions_(std::move(actions))
{
    setObjectName(QStringLiteral("historyWindow"));
    setWindowTitle(QStringLiteral("截图历史"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(1120, 720);
    setMinimumSize(420, 360);
    auto* layout = new QVBoxLayout(this);
    auto* toolbar = new QHBoxLayout;
    collapse_ = new QToolButton(this);
    collapse_->setObjectName(QStringLiteral("historyToggleGroups"));
    collapse_->setText(QStringLiteral("分组列"));
    collapse_->setCheckable(true);
    collapse_->setChecked(true);
    groupCombo_ = new QComboBox(this);
    groupCombo_->setObjectName(QStringLiteral("historyGroupCombo"));
    groupCombo_->setMinimumContentsLength(6);
    groupCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    sortCombo_ = new QComboBox(this);
    sortCombo_->setObjectName(QStringLiteral("historySortCombo"));
    sortCombo_->addItem(QStringLiteral("新 → 旧"));
    sortCombo_->addItem(QStringLiteral("旧 → 新"));
    auto* refreshButton = new QPushButton(QStringLiteral("刷新"), this);
    refreshButton->setObjectName(QStringLiteral("historyRefresh"));
    toolbar->addWidget(collapse_);
    toolbar->addWidget(groupCombo_, 1);
    toolbar->addWidget(sortCombo_);
    toolbar->addWidget(refreshButton);
    layout->addLayout(toolbar);
    // 缩放另起一行，窄窗口下无需隐藏入口。
    auto* zoomBar = new QHBoxLayout;
    auto* fit = new QPushButton(QStringLiteral("适应窗口"), this);
    fit->setObjectName(QStringLiteral("historyFit"));
    auto* actual = new QPushButton(QStringLiteral("100%"), this);
    actual->setObjectName(QStringLiteral("historyActualSize"));
    zoomBar->addWidget(fit);
    zoomBar->addWidget(actual);
    zoomBar->addStretch();
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("historyStatus"));
    zoomBar->addWidget(status_);
    layout->addLayout(zoomBar);
    auto* actionBar = new QHBoxLayout;
    const auto actionButton =
        [this, actionBar](const char* name, const QString& text, const QString& tip)
    {
        auto* button = new QToolButton(this);
        button->setObjectName(QLatin1String(name));
        button->setText(text);
        button->setToolTip(tip);
        actionBar->addWidget(button);
        return button;
    };
    star_ = actionButton("historyStar", QStringLiteral("☆ 星标"), QStringLiteral("切换星标（S）"));
    star_->setCheckable(true);
    pin_ = actionButton("historyPin", QStringLiteral("钉住"),
                        QStringLiteral("钉住并关闭（Enter）；Shift+Enter 钉住并留窗"));
    copy_ = actionButton("historyCopy", QStringLiteral("复制"), QStringLiteral("复制原图"));
    reveal_ = actionButton("historyReveal", QStringLiteral("在文件夹中显示"),
                           QStringLiteral("在文件夹中显示当前图片"));
    delete_ = actionButton("historyDelete", QStringLiteral("删除"),
                           QStringLiteral("将当前受管图片移入系统回收站"));
    actionBar->addStretch();
    layout->addLayout(actionBar);
    connect(star_, &QToolButton::clicked, this, &HistoryWindow::toggleStar);
    connect(copy_, &QToolButton::clicked, this, [this] { outputImage(false); });
    connect(pin_, &QToolButton::clicked, this, [this] { outputImage(true, true); });
    connect(delete_, &QToolButton::clicked, this, &HistoryWindow::deleteSelection);
    connect(reveal_, &QToolButton::clicked, this,
            [this] { showActionResult(manager_.revealItem(selectedId(), actions_.reveal)); });
    notice_ = new QLabel(this);
    notice_->setObjectName(QStringLiteral("historyNotice"));
    notice_->setWordWrap(true);
    layout->addWidget(notice_);
    splitter_ = new QSplitter(Qt::Horizontal, this);
    splitter_->setObjectName(QStringLiteral("historySplitter"));
    groups_ = new QListWidget(splitter_);
    groups_->setObjectName(QStringLiteral("historyGroups"));
    groups_->setMinimumSize(80, 60);
    groups_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(groups_, &QWidget::customContextMenuRequested, this, &HistoryWindow::showGroupMenu);
    imageSplitter_ = new QSplitter(Qt::Horizontal, splitter_);
    imageSplitter_->setObjectName(QStringLiteral("historyImageSplitter"));
    thumbnails_ = new QListWidget(imageSplitter_);
    thumbnails_->setObjectName(QStringLiteral("historyThumbnails"));
    thumbnails_->setIconSize({96, 72});
    thumbnails_->setMinimumSize(100, 80);
    thumbnails_->setUniformItemSizes(true);
    auto* preview = new QWidget(imageSplitter_);
    auto* previewLayout = new QVBoxLayout(preview);
    previewLayout->setContentsMargins(0, 0, 0, 0);
    empty_ = new QLabel(preview);
    empty_->setObjectName(QStringLiteral("historyEmpty"));
    empty_->setAlignment(Qt::AlignCenter);
    empty_->setWordWrap(true);
    previewLayout->addWidget(empty_);
    imageView_ = new HistoryImageView(preview);
    previewLayout->addWidget(imageView_, 1);
    splitter_->setCollapsible(0, false);
    splitter_->setCollapsible(1, false);
    imageSplitter_->setChildrenCollapsible(false);
    splitter_->setSizes({190, 900});
    imageSplitter_->setSizes({230, 650});
    layout->addWidget(splitter_, 1);
    connect(collapse_, &QToolButton::toggled, this,
            [this](bool shown)
            {
                groups_->setVisible(shown);
                scheduleThumbnails();
            });
    connect(groups_, &QListWidget::currentRowChanged, this,
            [this](int row)
            {
                if (row >= 0)
                    selectView(groups_->item(row)->data(Qt::UserRole).toString(), true);
            });
    connect(groupCombo_, &QComboBox::currentIndexChanged, this,
            [this](int row)
            {
                if (row >= 0)
                    selectView(groupCombo_->itemData(row).toString(), true);
            });
    connect(sortCombo_, &QComboBox::currentIndexChanged, this,
            [this](int row)
            {
                order_ = row == 0 ? HistorySort::NewestFirst : HistorySort::OldestFirst;
                const auto* selected = thumbnails_->currentItem();
                rebuildItems(selected ? selected->data(Qt::UserRole).toString() : QString());
            });
    connect(refreshButton, &QPushButton::clicked, this, &HistoryWindow::refresh);
    connect(fit, &QPushButton::clicked, imageView_, &HistoryImageView::fitImage);
    connect(actual, &QPushButton::clicked, imageView_, &HistoryImageView::actualSize);
    connect(thumbnails_, &QListWidget::currentRowChanged, this, &HistoryWindow::showSelection);
    connect(thumbnails_, &QListWidget::itemChanged, this,
            [this](QListWidgetItem* entry)
            {
                const QString id = entry->data(Qt::UserRole).toString();
                const auto item = manager_.findItem(id);
                const bool starred = entry->checkState() == Qt::Checked;
                if (!item || item->starred == starred)
                    return;
                thumbnails_->setCurrentItem(entry);
                const auto result = manager_.setStarred(id, starred);
                if (result.success)
                    refresh();
                else
                {
                    const QSignalBlocker block(thumbnails_);
                    entry->setCheckState(item->starred ? Qt::Checked : Qt::Unchecked);
                }
                showActionResult(result);
            });
    connect(thumbnails_->verticalScrollBar(), &QScrollBar::valueChanged, this,
            &HistoryWindow::scheduleThumbnails);
    connect(thumbnails_->horizontalScrollBar(), &QScrollBar::valueChanged, this,
            &HistoryWindow::scheduleThumbnails);
    connect(imageSplitter_, &QSplitter::splitterMoved, this, &HistoryWindow::scheduleThumbnails);
    thumbnails_->viewport()->installEventFilter(this);
    connect(&manager_, &HistoryManager::groupsChanged, this, &HistoryWindow::refresh);
    connect(&manager_, &HistoryManager::currentGroupChanged, this,
            [this]
            {
                snapshot_.currentGroupId = manager_.currentGroupId();
                rebuildGroups(view_);
            });
    const auto saved = settings_.loadHistoryWindowState();
    view_ = saved.lastView;
    order_ = saved.sortOrder == QStringLiteral("oldest") ||
                     (saved.sortOrder.isEmpty() && view_ != QStringLiteral("all") &&
                      view_ != QStringLiteral("starred"))
                 ? HistorySort::OldestFirst
                 : HistorySort::NewestFirst;
    snapshot_ = manager_.refresh();
    rebuildGroups(view_);
    rebuildItems(saved.lastSelection);
    restoreWindowState(saved);
    // 子控件也遵循窗口级方向键 / Esc，避免列表自己再移动一遍。
    installEventFilter(this);
    for (auto* widget : findChildren<QWidget*>())
        widget->installEventFilter(this);
}
void HistoryWindow::setGroupActionsEnabled(bool enabled)
{
    groupActionsEnabled_ = enabled;
    if (groups_->count())
        groups_->item(groups_->count() - 1)
            ->setFlags(enabled ? Qt::ItemIsEnabled | Qt::ItemIsSelectable : Qt::NoItemFlags);
    if (auto* model = qobject_cast<QStandardItemModel*>(groupCombo_->model()))
        if (auto* item = model->item(groupCombo_->count() - 1))
            item->setEnabled(enabled);
}
bool HistoryWindow::dismissGroupDialogs()
{
    const auto dialogs = findChildren<QDialog*>();
    const auto menus = findChildren<QMenu*>();
    for (auto* dialog : dialogs)
        dialog->reject();
    for (auto* menu : menus)
        menu->close();
    return !dialogs.isEmpty() || !menus.isEmpty();
}
void HistoryWindow::editGroupName(const QString& groupId)
{
    if (!groupActionsEnabled_)
        return;
    QPointer<QDialog> dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("historyGroupNameDialog"));
    dialog->setWindowTitle(groupId.isEmpty() ? QStringLiteral("新建分组")
                                             : QStringLiteral("分组改名"));
    auto* layout = new QVBoxLayout(dialog);
    layout->addWidget(new QLabel(QStringLiteral("分组名称"), dialog));
    auto* edit = new QLineEdit(dialog);
    edit->setObjectName(QStringLiteral("historyGroupName"));
    for (const auto& group : manager_.groups())
        if (group.id == groupId)
            edit->setText(group.name);
    edit->selectAll();
    layout->addWidget(edit);
    auto* error = new QLabel(dialog);
    error->setObjectName(QStringLiteral("historyGroupNameError"));
    error->setWordWrap(true);
    layout->addWidget(error);
    auto* buttons = new QDialogButtonBox(dialog);
    buttons->addButton(QStringLiteral("确定"), QDialogButtonBox::AcceptRole)
        ->setObjectName(QStringLiteral("historyGroupNameSave"));
    buttons->addButton(QStringLiteral("取消"), QDialogButtonBox::RejectRole);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, dialog,
            [this, groupId, edit, error, dialog]
            {
                const auto result = groupId.isEmpty() ? manager_.createGroup(edit->text())
                                                      : manager_.renameGroup(groupId, edit->text());
                if (result.success)
                {
                    dialog->accept();
                    if (groupId.isEmpty())
                        selectView(result.group.id, true);
                }
                else
                    error->setText(result.error);
            });
    const QPointer<HistoryWindow> guard(this);
    emit groupDialogOpenChanged(true);
    dialog->exec();
    delete dialog.data();
    if (guard)
        emit groupDialogOpenChanged(false);
}
void HistoryWindow::openGroupSettings(const QString& groupId)
{
    if (!groupActionsEnabled_)
        return;
    QPointer<GroupSettingsDialog> dialog =
        new GroupSettingsDialog(manager_, groupId, actions_.trash, selectedId(), this);
    const QPointer<HistoryWindow> guard(this);
    emit groupDialogOpenChanged(true);
    dialog->exec();
    delete dialog.data();
    if (guard)
        emit groupDialogOpenChanged(false);
}
void HistoryWindow::deleteGroup(const QString& groupId)
{
    if (!groupActionsEnabled_)
        return;
    const QPointer<HistoryWindow> guard(this);
    emit groupDialogOpenChanged(true);
    const auto result = confirmHistoryGroupDeletion(manager_, groupId, actions_.trash, this);
    if (guard)
    {
        emit groupDialogOpenChanged(false);
        if (result)
            showActionResult(*result);
    }
}
void HistoryWindow::showGroupMenu(const QPoint& point)
{
    if (!groupActionsEnabled_)
        return;
    const auto* entry = groups_->itemAt(point);
    if (!entry)
        return;
    const auto id = entry->data(Qt::UserRole).toString();
    for (const auto& group : manager_.groups())
        if (group.id == id)
        {
            QPointer<QMenu> menu = new QMenu(this);
            menu->setObjectName(QStringLiteral("historyGroupMenu"));
            auto* current = menu->addAction(QStringLiteral("设为当前分组"), this,
                                            [this, id]
                                            {
                                                if (groupActionsEnabled_)
                                                    showActionResult(manager_.setCurrentGroup(id));
                                            });
            current->setObjectName(QStringLiteral("historyGroupSetCurrent"));
            current->setCheckable(true);
            current->setChecked(manager_.currentGroupId() == id);
            menu->addAction(QStringLiteral("分组设置…"), this,
                            [this, id] { openGroupSettings(id); })
                ->setObjectName(QStringLiteral("historyGroupSettings"));
            menu->addAction(QStringLiteral("改名…"), this, [this, id] { editGroupName(id); })
                ->setObjectName(QStringLiteral("historyGroupRename"));
            auto* remove =
                menu->addAction(QStringLiteral("删除…"), this, [this, id] { deleteGroup(id); });
            remove->setObjectName(QStringLiteral("historyGroupDelete"));
            remove->setEnabled(!group.isDefault && bool(actions_.trash));
            menu->exec(groups_->viewport()->mapToGlobal(point));
            delete menu.data();
            return;
        }
}
void HistoryWindow::rebuildGroups(const QString& view)
{
    const QSignalBlocker listBlock(groups_), comboBlock(groupCombo_);
    groups_->clear();
    groupCombo_->clear();
    qsizetype total = 0, starred = 0;
    for (const auto& group : snapshot_.groups)
    {
        total += group.items.size();
        starred += group.usage.starredCount;
    }
    const auto add = [this](QString name, const QString& id, qsizetype count)
    {
        const QString text = QStringLiteral("%1（%2）").arg(name).arg(count);
        auto* item = new QListWidgetItem(text, groups_);
        item->setData(Qt::UserRole, id);
        groupCombo_->addItem(text, id);
    };
    add(QStringLiteral("全部历史"), QStringLiteral("all"), total);
    add(QStringLiteral("星标"), QStringLiteral("starred"), starred);
    auto groups = snapshot_.groups;
    std::stable_sort(groups.begin(), groups.end(), [](const auto& a, const auto& b)
                     { return a.group.isDefault && !b.group.isDefault; });
    for (const auto& group : groups)
    {
        if (group.group.id.isEmpty())
            continue;
        const QString dot =
            group.group.id == snapshot_.currentGroupId ? QStringLiteral("● ") : QString();
        add(dot + group.group.name, group.group.id, group.items.size());
        groups_->item(groups_->count() - 1)
            ->setToolTip(group.group.id == snapshot_.currentGroupId
                             ? QStringLiteral("当前截图分组")
                             : QStringLiteral("仅查看此分组"));
    }
    auto* create = new QListWidgetItem(QStringLiteral("新建分组…"), groups_);
    create->setData(Qt::UserRole, QStringLiteral("new-group"));
    groupCombo_->addItem(QStringLiteral("新建分组…"), QStringLiteral("new-group"));
    setGroupActionsEnabled(groupActionsEnabled_);
    int row = groupCombo_->findData(view);
    if (row < 0)
        row = 0;
    view_ = groupCombo_->itemData(row).toString();
    groups_->setCurrentRow(row);
    groupCombo_->setCurrentIndex(row);
    notice_->setText(snapshot_.error);
    notice_->setVisible(!snapshot_.error.isEmpty());
}
void HistoryWindow::selectView(const QString& view, bool useDefaultSort)
{
    if (view == QStringLiteral("new-group"))
    {
        {
            const QSignalBlocker listBlock(groups_), comboBlock(groupCombo_);
            const int row = groupCombo_->findData(view_);
            groups_->setCurrentRow(row);
            groupCombo_->setCurrentIndex(row);
        }
        editGroupName();
        return;
    }
    if (view == view_)
        return;
    view_ = view;
    const QSignalBlocker listBlock(groups_), comboBlock(groupCombo_);
    const int row = groupCombo_->findData(view);
    groups_->setCurrentRow(row);
    groupCombo_->setCurrentIndex(row);
    if (useDefaultSort)
        order_ = view == QStringLiteral("all") || view == QStringLiteral("starred")
                     ? HistorySort::NewestFirst
                     : HistorySort::OldestFirst;
    rebuildItems({});
}
void HistoryWindow::rebuildItems(const QString& selected)
{
    items_.clear();
    for (const auto& group : snapshot_.groups)
        for (const auto& item : group.items)
            if (view_ == QStringLiteral("all") ||
                (view_ == QStringLiteral("starred") && item.starred) || view_ == item.groupId)
                items_.append(item);
    items_ = sortedHistoryItems(std::move(items_), order_);
    const QSignalBlocker sortBlock(sortCombo_), listBlock(thumbnails_);
    sortCombo_->setCurrentIndex(order_ == HistorySort::NewestFirst ? 0 : 1);
    thumbnails_->clear();
    int selectedRow = 0;
    qint64 bytes = 0;
    for (const auto& item : items_)
    {
        QString label = QDateTime::fromMSecsSinceEpoch(item.createdAtMs)
                            .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        label += item.starred ? QStringLiteral("\n★ 星标") : QStringLiteral("\n☆ 未星标");
        if (item.source == HistorySource::Scroll)
            label += QStringLiteral(" · 长图");
        if (item.source == HistorySource::External)
            label += QStringLiteral(" · 外部图片");
        auto* entry = new QListWidgetItem(label, thumbnails_);
        entry->setSizeHint({210, 92});
        entry->setData(Qt::UserRole, historyItemId(item));
        entry->setToolTip(item.fileName);
        entry->setFlags(entry->flags() | Qt::ItemIsUserCheckable);
        entry->setCheckState(item.starred ? Qt::Checked : Qt::Unchecked);
        if (historyItemId(item) == selected)
            selectedRow = thumbnails_->count() - 1;
        bytes += item.bytes;
    }
    status_->setText(
        QStringLiteral("%1 张 · %2 MiB").arg(items_.size()).arg(bytes / 1048576.0, 0, 'f', 1));
    thumbnails_->setCurrentRow(items_.isEmpty() ? -1 : selectedRow);
    if (thumbnails_->currentItem())
        thumbnails_->scrollToItem(thumbnails_->currentItem());
    showSelection();
    scheduleThumbnails();
}
void HistoryWindow::showSelection()
{
    updateActions();
    const auto* entry = thumbnails_->currentItem();
    if (!entry)
    {
        imageView_->setImage({});
        imageView_->hide();
        empty_->setText(view_ == QStringLiteral("all")
                            ? QStringLiteral("还没有截图历史。完成截图后可在这里查看。")
                        : view_ == QStringLiteral("starred") ? QStringLiteral("还没有星标图片。")
                                                             : QStringLiteral("此分组暂无记录。"));
        empty_->show();
        return;
    }
    // 切图前释放上一张主图，避免两个大图的解码内存长期重叠。
    imageView_->setImage({});
    const auto result = manager_.loadImage(entry->data(Qt::UserRole).toString());
    imageView_->setImage(result.image);
    imageView_->setVisible(result.success);
    empty_->setText(result.error);
    empty_->setVisible(!result.success);
}
QString HistoryWindow::selectedId() const
{
    const auto* entry = thumbnails_->currentItem();
    return entry ? entry->data(Qt::UserRole).toString() : QString();
}
void HistoryWindow::updateActions()
{
    const auto item = manager_.findItem(selectedId());
    star_->setEnabled(bool(item));
    star_->setChecked(item && item->starred);
    star_->setText(item && item->starred ? QStringLiteral("★ 星标") : QStringLiteral("☆ 星标"));
    copy_->setEnabled(item && bool(actions_.copyImage));
    pin_->setEnabled(item && bool(actions_.pinImage));
    reveal_->setEnabled(item && bool(actions_.reveal));
    delete_->setEnabled(item && item->managed && bool(actions_.trash));
    delete_->setToolTip(item && !item->managed ? QStringLiteral("请在文件管理器中操作")
                                               : QStringLiteral("将当前受管图片移入系统回收站"));
}
void HistoryWindow::showActionResult(const HistoryStatus& result)
{
    notice_->setText(result.success ? snapshot_.error : result.error);
    notice_->setVisible(!notice_->text().isEmpty());
    updateActions();
}
void HistoryWindow::toggleStar()
{
    const auto item = manager_.findItem(selectedId());
    if (!item)
        return;
    const auto result = manager_.setStarred(historyItemId(*item), !item->starred);
    if (result.success)
        refresh();
    showActionResult(result);
}
void HistoryWindow::outputImage(bool pin, bool closeAfter)
{
    if (selectedId().isEmpty())
        return;
    const auto loaded = manager_.loadImage(selectedId());
    if (!loaded.success)
    {
        showActionResult(loaded);
        return;
    }
    const auto& callback = pin ? actions_.pinImage : actions_.copyImage;
    const auto result = callback ? callback(loaded.image)
                                 : ImageOutputResult{false, QStringLiteral("图片输出服务不可用。")};
    showActionResult({result.success, result.explanation});
    if (result.success && pin && closeAfter)
        close();
}
void HistoryWindow::deleteSelection()
{
    const auto result = manager_.deleteItem(selectedId(), actions_.trash);
    if (result.success)
        refresh();
    showActionResult(result);
}
void HistoryWindow::refresh()
{
    const auto* entry = thumbnails_->currentItem();
    const QString selected = entry ? entry->data(Qt::UserRole).toString() : QString();
    snapshot_ = manager_.refresh();
    rebuildGroups(view_);
    rebuildItems(selected);
}
void HistoryWindow::scheduleThumbnails()
{
    if (thumbnailsPending_)
        return;
    thumbnailsPending_ = true;
    QTimer::singleShot(0, this,
                       [this]
                       {
                           thumbnailsPending_ = false;
                           loadVisibleThumbnails();
                       });
}
void HistoryWindow::loadVisibleThumbnails()
{
    // 仅保留可见图标，避免长时间浏览最终把整库缩略图留在列表中。
    int decoded = 0;
    bool more = false;
    const QRect viewport = thumbnails_->viewport()->rect();
    for (int row = 0; row < thumbnails_->count(); ++row)
    {
        auto* entry = thumbnails_->item(row);
        if (!thumbnails_->visualItemRect(entry).intersects(viewport))
        {
            if (entry->data(Qt::UserRole + 1).toBool())
            {
                entry->setIcon(QIcon());
                entry->setData(Qt::UserRole + 1, false);
            }
            continue;
        }
        if (entry->data(Qt::UserRole + 1).toBool())
            continue;
        if (decoded >= 3)
        {
            more = true;
            continue;
        }
        ++decoded;
        const qreal dpr = thumbnails_->devicePixelRatioF();
        const auto result = manager_.loadImage(entry->data(Qt::UserRole).toString(),
                                               QSize(qRound(96 * dpr), qRound(72 * dpr)));
        if (result.success)
        {
            auto pixmap = QPixmap::fromImage(result.image);
            pixmap.setDevicePixelRatio(dpr);
            entry->setIcon(QIcon(pixmap));
        }
        entry->setData(Qt::UserRole + 1, true);
    }
    if (more)
        scheduleThumbnails();
}
void HistoryWindow::updateLayout()
{
    const bool narrow = width() < 900;
    // splitter 恢复也会恢复方向；列表流向始终从当前宽度推导。
    groups_->setFlow(narrow ? QListView::LeftToRight : QListView::TopToBottom);
    thumbnails_->setFlow(narrow ? QListView::LeftToRight : QListView::TopToBottom);
    if (narrow != narrow_)
    {
        narrow_ = narrow;
        splitter_->setOrientation(narrow ? Qt::Vertical : Qt::Horizontal);
        imageSplitter_->setOrientation(narrow ? Qt::Vertical : Qt::Horizontal);
        if (narrow)
        {
            splitter_->setSizes({70, 600});
            imageSplitter_->setSizes({115, 420});
        }
        else
        {
            splitter_->setSizes({190, 900});
            imageSplitter_->setSizes({230, 650});
        }
    }
    scheduleThumbnails();
}
void HistoryWindow::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    updateLayout();
}
bool HistoryWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == thumbnails_->viewport() && event->type() == QEvent::Resize)
        scheduleThumbnails();
    if (event->type() == QEvent::KeyPress)
    {
        const auto* key = static_cast<QKeyEvent*>(event);
        if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) &&
            (key->modifiers() == Qt::NoModifier || key->modifiers() == Qt::ShiftModifier))
        {
            if (!key->isAutoRepeat())
                outputImage(true, key->modifiers() == Qt::NoModifier);
            return true;
        }
        if (key->key() == Qt::Key_S && key->modifiers() == Qt::NoModifier)
        {
            if (!key->isAutoRepeat())
                toggleStar();
            return true;
        }
        if (key->key() == Qt::Key_Escape)
        {
            close();
            return true;
        }
        if (key->modifiers() == Qt::NoModifier &&
            (key->key() == Qt::Key_Up || key->key() == Qt::Key_Down || key->key() == Qt::Key_Left ||
             key->key() == Qt::Key_Right))
        {
            const int step = key->key() == Qt::Key_Up || key->key() == Qt::Key_Left ? -1 : 1;
            if (thumbnails_->count())
                thumbnails_->setCurrentRow(
                    std::clamp(thumbnails_->currentRow() + step, 0, thumbnails_->count() - 1));
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}
void HistoryWindow::restoreWindowState(const HistoryWindowState& state)
{
    if (!state.geometry.isEmpty())
        restoreGeometry(state.geometry);
    bool visible = false;
    for (auto* screen : QGuiApplication::screens())
        visible |= screen->availableGeometry().intersects(frameGeometry());
    if (!visible && QGuiApplication::primaryScreen())
    {
        const QRect available = QGuiApplication::primaryScreen()->availableGeometry();
        resize(size().boundedTo(available.size()));
        move(available.center() - rect().center());
    }
    QByteArray outer, inner;
    bool shown = true;
    QDataStream stream(state.splitterState);
    stream >> outer >> inner >> shown;
    if (stream.status() == QDataStream::Ok)
    {
        collapse_->setChecked(shown);
        layout()->activate();
        splitter_->restoreState(outer);
        imageSplitter_->restoreState(inner);
    }
    // restoreState 内含方向；先读取恢复方向，再按实际宽度校正。
    narrow_ = splitter_->orientation() == Qt::Vertical;
    updateLayout();
}
void HistoryWindow::saveWindowState()
{
    HistoryWindowState state;
    state.geometry = saveGeometry();
    QDataStream stream(&state.splitterState, QIODevice::WriteOnly);
    stream << splitter_->saveState() << imageSplitter_->saveState() << collapse_->isChecked();
    state.lastView = view_;
    if (const auto* selected = thumbnails_->currentItem())
        state.lastSelection = selected->data(Qt::UserRole).toString();
    state.sortOrder =
        order_ == HistorySort::NewestFirst ? QStringLiteral("newest") : QStringLiteral("oldest");
    const QString error = settings_.saveHistoryWindowState(state);
    if (!error.isEmpty())
        qWarning().noquote() << error;
}
void HistoryWindow::closeEvent(QCloseEvent* event)
{
    saveWindowState();
    event->accept();
}
}
