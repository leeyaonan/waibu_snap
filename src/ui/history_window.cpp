#include "ui/history_window.h"
#include <QCloseEvent>
#include <QComboBox>
#include <QDataStream>
#include <QDateTime>
#include <QDebug>
#include <QDialog>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QScrollBar>
#include <QShowEvent>
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
void HistoryImageView::setImage(QImage image, bool preserveView)
{
    const auto previousTransform = transform();
    const QPoint scroll(horizontalScrollBar()->value(), verticalScrollBar()->value());
    image.setDevicePixelRatio(1);
    item_->replace(std::move(image));
    scene()->setSceneRect(item_->boundingRect());
    if (preserveView)
    {
        setTransform(previousTransform);
        horizontalScrollBar()->setValue(scroll.x());
        verticalScrollBar()->setValue(scroll.y());
    }
    else
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
                             HistoryWindowActions actions, ResourceSource* additionalSource,
                             bool historyEnabled)
    : historySource_(new HistoryResourceSource(manager, std::move(actions), this)),
      source_(historySource_), settings_(std::move(settings))
{
    historySource_->setEnabled(historyEnabled);
    sources_.append(historySource_);
    if (additionalSource)
        sources_.append(additionalSource);
    setObjectName(QStringLiteral("historyWindow"));
    setWindowTitle(QStringLiteral("资源浏览器"));
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
    status_->setWordWrap(true);
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
    copy_ = actionButton("historyCopy", QStringLiteral("复制"), QStringLiteral("复制当前版本"));
    original_ = actionButton("historyOriginalToggle", QStringLiteral("原图"),
                             QStringLiteral("切换水印版 / 原图"));
    original_->setCheckable(true);
    reveal_ = actionButton("historyReveal", QStringLiteral("在文件夹中显示"),
                           QStringLiteral("在文件夹中显示当前图片"));
    delete_ = actionButton("historyDelete", QStringLiteral("删除"),
                           QStringLiteral("将当前受管图片移入系统回收站"));
    clear_ = actionButton("resourceClearAll", QStringLiteral("清空全部"),
                          QStringLiteral("将此来源全部图片移入系统回收站（包含星标）"));
    configure_ = actionButton("resourceChooseDirectory", QStringLiteral("选择目录…"),
                              QStringLiteral("配置此来源的图片目录"));
    connect(clear_, &QToolButton::clicked, this, &HistoryWindow::clearSource);
    connect(configure_, &QToolButton::clicked, this,
            [this] { emit directoryRequested(source_->id()); });
    actionBar->addStretch();
    layout->addLayout(actionBar);
    connect(star_, &QToolButton::clicked, this, &HistoryWindow::toggleStar);
    connect(copy_, &QToolButton::clicked, this, [this] { outputImage(false); });
    connect(original_, &QToolButton::toggled, this,
            [this](bool original)
            {
                showOriginal_ = original;
                showSelection();
            });
    connect(pin_, &QToolButton::clicked, this, [this] { outputImage(true, true); });
    connect(delete_, &QToolButton::clicked, this, &HistoryWindow::deleteSelection);
    connect(reveal_, &QToolButton::clicked, this,
            [this] { showActionResult(source_->revealItem(selectedId())); });
    notice_ = new QLabel(this);
    notice_->setObjectName(QStringLiteral("historyNotice"));
    notice_->setWordWrap(true);
    layout->addWidget(notice_);
    splitter_ = new QSplitter(Qt::Horizontal, this);
    splitter_->setObjectName(QStringLiteral("historySplitter"));
    auto* sources = new QWidget(splitter_);
    auto* sourcesLayout = new QVBoxLayout(sources);
    sourcesLayout->setContentsMargins(0, 0, 0, 0);
    sourceCombo_ = new QComboBox(sources);
    sourceCombo_->setObjectName(QStringLiteral("resourceSourceCombo"));

    // 来源入口始终可达，分组列折叠与窄窗口不会把来源选择器藏起来。
    sourceCombo_->setParent(this);
    toolbar->insertWidget(0, sourceCombo_);
    connect(sourceCombo_, &QComboBox::currentIndexChanged, this,
            [this](int row)
            {
                if (row >= 0)
                    selectSource(sourceCombo_->itemData(row).toString());
            });
    groups_ = new QListWidget(sources);
    sourcesLayout->addWidget(groups_, 1);
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
            [this, sources](bool shown)
            {
                sources->setVisible(shown);
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
                order_ = row == 0 ? ResourceSort::NewestFirst : ResourceSort::OldestFirst;
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
                const auto item = findItem(id);
                const bool starred = entry->checkState() == Qt::Checked;
                if (!item || item->starred == starred)
                    return;
                thumbnails_->setCurrentItem(entry);
                const auto result = source_->setStarred(id, starred);
                if (!result.success)
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
    for (auto* source : sources_)
        connect(source, &ResourceSource::changed, this,
                [this, source]
                {
                    if (source == source_ || !source_->visible())
                        refreshFromSource();
                    else
                        rebuildSources();
                });
    connect(historySource_, &HistoryResourceSource::groupDialogOpenChanged, this,
            &HistoryWindow::groupDialogOpenChanged);
    connect(historySource_, &HistoryResourceSource::groupViewRequested, this,
            [this](const QString& id) { selectView(id, true); });
    connect(historySource_, &HistoryResourceSource::actionFinished, this,
            &HistoryWindow::showActionResult);
    const auto saved = settings_.loadHistoryWindowState();
    view_ = saved.lastView;
    order_ = saved.sortOrder == QStringLiteral("oldest") ||
                     (saved.sortOrder.isEmpty() && view_ != QStringLiteral("all") &&
                      view_ != QStringLiteral("starred"))
                 ? ResourceSort::OldestFirst
                 : ResourceSort::NewestFirst;
    sourceViews_.insert(saved.lastSource, {view_, saved.lastSelection, order_});
    source_ = nullptr;
    selectSource(saved.lastSource);
    restoreWindowState(saved);
    // 子控件也遵循窗口级方向键 / Esc，避免列表自己再移动一遍。
    installEventFilter(this);
    for (auto* widget : findChildren<QWidget*>())
        widget->installEventFilter(this);
}
void HistoryWindow::rebuildSources()
{
    const QSignalBlocker block(sourceCombo_);
    sourceCombo_->clear();
    for (auto* candidate : sources_)
        if (candidate->visible() ||
            (candidate == source_ && candidate->capabilities().configureDirectory))
            sourceCombo_->addItem(candidate->displayName(), candidate->id());
    sourceCombo_->setCurrentIndex(sourceCombo_->findData(source_->id()));
}
void HistoryWindow::selectSource(const QString& sourceId)
{
    ResourceSource* next = nullptr;
    for (auto* candidate : sources_)
        if (candidate->id() == sourceId &&
            (candidate->visible() || candidate->capabilities().configureDirectory))
            next = candidate;
    if (!next)
        for (auto* candidate : sources_)
            if (candidate->visible())
            {
                next = candidate;
                break;
            }
    if (!next)
        for (auto* candidate : sources_)
            if (candidate->capabilities().configureDirectory)
            {
                next = candidate;
                break;
            }
    if (!next)
        next = historySource_;
    if (next == source_)
    {
        rebuildSources();
        return;
    }
    if (source_)
    {
        sourceViews_.insert(source_->id(), {view_, selectedId(), order_});
        source_->setProtectedItem({});
        source_->setActive(false);
    }
    source_ = next;
    const auto state = sourceViews_.value(source_->id());
    view_ = state.view;
    order_ = state.order;
    showOriginal_ = false;
    displayedId_.clear();
    source_->refresh();
    snapshot_ = source_->snapshot();
    rebuildSources();
    rebuildGroups(view_);
    rebuildItems(state.selected);
    source_->setActive(isVisible());
    emit sourceChanged(source_->id());
}
void HistoryWindow::setHistoryEnabled(bool enabled)
{
    historySource_->setEnabled(enabled);
    refreshFromSource();
}
void HistoryWindow::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    source_->setActive(true);
}
void HistoryWindow::hideEvent(QHideEvent* event)
{
    source_->setActive(false);
    QWidget::hideEvent(event);
}
void HistoryWindow::clearSource()
{
    if (!source_->capabilities().clearAll)
        return;
    const QPointer<ResourceSource> target(source_);
    const QPointer<HistoryWindow> guard(this);
    QMessageBox confirm(
        QMessageBox::Warning, QStringLiteral("清空全部"),
        QStringLiteral("将“%1”中的全部图片移入系统回收站？\n包含星标图片及当前筛选外图片。")
            .arg(target->displayName()),
        QMessageBox::Yes | QMessageBox::Cancel, this);
    confirm.setObjectName(QStringLiteral("resourceClearConfirm"));
    confirm.setDefaultButton(QMessageBox::Cancel);
    confirm.button(QMessageBox::Yes)->setText(QStringLiteral("移入回收站"));
    confirm.button(QMessageBox::Cancel)->setText(QStringLiteral("取消"));
    // 复用已有模态状态通知，托盘目录 / 设置入口在确认期间也受控。
    emit groupDialogOpenChanged(true);
    const int answer = confirm.exec();
    if (!guard || !target)
        return;
    emit groupDialogOpenChanged(false);
    if (answer == QMessageBox::Yes && target == source_)
    {
        const auto result = target->clearAll();
        refreshFromSource();
        showActionResult(result);
        if (result.success)
        {
            notice_->setText(QStringLiteral("已移入回收站 %1 张。").arg(result.processedCount));
            notice_->show();
        }
    }
}
void HistoryWindow::setGroupActionsEnabled(bool enabled)
{
    groupActionsEnabled_ = enabled;
    historySource_->setGroupActionsEnabled(enabled);
    if (!source_->capabilities().groups)
        return;
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
    if (source_->capabilities().groups)
        historySource_->editGroupName(groupId, this);
}
void HistoryWindow::showGroupMenu(const QPoint& point)
{
    if (!source_->capabilities().groups)
        return;
    if (const auto* entry = groups_->itemAt(point))
        historySource_->showGroupMenu(entry->data(Qt::UserRole).toString(),
                                      groups_->viewport()->mapToGlobal(point), this);
}
void HistoryWindow::rebuildGroups(const QString& view)
{
    const QSignalBlocker listBlock(groups_), comboBlock(groupCombo_);
    groups_->clear();
    groupCombo_->clear();
    const auto total = snapshot_.items.size();
    const auto starred = snapshot_.usage.starredCount;
    const auto add = [this](QString name, const QString& id, qsizetype count)
    {
        const QString text = QStringLiteral("%1（%2）").arg(name).arg(count);
        auto* item = new QListWidgetItem(text, groups_);
        item->setData(Qt::UserRole, id);
        groupCombo_->addItem(text, id);
    };
    const bool grouped = source_->capabilities().groups;
    add(grouped ? QStringLiteral("全部历史") : QStringLiteral("全部"), QStringLiteral("all"),
        total);
    add(QStringLiteral("星标"), QStringLiteral("starred"), starred);
    auto groups = snapshot_.groups;
    std::stable_sort(groups.begin(), groups.end(),
                     [](const auto& a, const auto& b) { return a.isDefault && !b.isDefault; });
    for (const auto& group : groups)
    {
        if (!grouped || group.id.isEmpty())
            continue;
        const QString dot = group.isCurrent ? QStringLiteral("● ") : QString();
        add(dot + group.name, group.id,
            snapshot_.groupUsage.value(group.id).managedCount +
                snapshot_.groupUsage.value(group.id).externalCount);
        groups_->item(groups_->count() - 1)
            ->setToolTip((group.isCurrent ? QStringLiteral("当前截图分组\n")
                                          : QStringLiteral("仅查看此分组\n")) +
                         QStringLiteral("目录：") + group.absolutePath);
    }
    if (grouped)
    {
        auto* create = new QListWidgetItem(QStringLiteral("新建分组…"), groups_);
        create->setData(Qt::UserRole, QStringLiteral("new-group"));
        groupCombo_->addItem(QStringLiteral("新建分组…"), QStringLiteral("new-group"));
    }
    setGroupActionsEnabled(groupActionsEnabled_);
    int row = groupCombo_->findData(view);
    if (row < 0)
        row = 0;
    view_ = groupCombo_->itemData(row).toString();
    groups_->setCurrentRow(row);
    groupCombo_->setCurrentIndex(row);
    updateNotice();
}
void HistoryWindow::navigate(int step, bool allHistory)
{
    if (allHistory)
    {
        // 上次记录可能属于另一视图，必须在全部历史中重新定位后再应用首步。
        const auto selected = settings_.loadHistoryWindowState().lastSelection;
        view_ = QStringLiteral("all");
        order_ = ResourceSort::NewestFirst;
        rebuildGroups(view_);
        rebuildItems(selected);
    }
    if (thumbnails_->count())
    {
        // 全局前后按时间方向移动；窗口方向键仍由事件过滤器按列表行序处理。
        const int rowStep = order_ == ResourceSort::NewestFirst ? -step : step;
        thumbnails_->setCurrentRow(
            std::clamp(thumbnails_->currentRow() + rowStep, 0, thumbnails_->count() - 1));
        thumbnails_->scrollToItem(thumbnails_->currentItem());
    }
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
                     ? ResourceSort::NewestFirst
                     : ResourceSort::OldestFirst;
    rebuildItems({});
}
void HistoryWindow::rebuildItems(const QString& selected, int fallbackRow)
{
    items_.clear();
    for (const auto& item : snapshot_.items)
        if (view_ == QStringLiteral("all") ||
            (view_ == QStringLiteral("starred") && item.starred) || view_ == item.groupId)
            items_.append(item);
    items_ = sortedResourceItems(std::move(items_), order_);
    const QSignalBlocker sortBlock(sortCombo_), listBlock(thumbnails_);
    sortCombo_->setCurrentIndex(order_ == ResourceSort::NewestFirst ? 0 : 1);
    thumbnails_->clear();
    int selectedRow = std::clamp(fallbackRow, 0, std::max(0, int(items_.size()) - 1));
    for (const auto& item : items_)
    {
        QString label = QDateTime::fromMSecsSinceEpoch(item.createdAtMs)
                            .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        label += item.starred ? QStringLiteral("\n★ 星标") : QStringLiteral("\n☆ 未星标");
        for (const auto& badge : item.badges)
            label += QStringLiteral(" · ") + badge;
        auto* entry = new QListWidgetItem(label, thumbnails_);
        entry->setSizeHint({210, 92});
        entry->setData(Qt::UserRole, item.id);
        entry->setToolTip(item.fileName);
        entry->setFlags(entry->flags() | Qt::ItemIsUserCheckable);
        entry->setCheckState(item.starred ? Qt::Checked : Qt::Unchecked);
        if (item.id == selected)
            selectedRow = thumbnails_->count() - 1;
    }
    thumbnails_->setCurrentRow(items_.isEmpty() ? -1 : selectedRow);
    if (thumbnails_->currentItem())
        thumbnails_->scrollToItem(thumbnails_->currentItem());
    showSelection();
    scheduleThumbnails();
}
HistoryWindow::~HistoryWindow()
{
    source_->setProtectedItem({});
    source_->setActive(false);
}
void HistoryWindow::showSelection()
{
    source_->setProtectedItem(selectedId());
    snapshot_ = source_->snapshot();
    const auto selected = findItem(selectedId());
    if (!selected || !selected->hasOriginal)
        showOriginal_ = false;
    updateStatus();
    updateActions();
    const auto* entry = thumbnails_->currentItem();
    if (!entry)
    {
        imageView_->setImage({});
        imageView_->hide();
        empty_->setText(!snapshot_.error.isEmpty() ? snapshot_.error
                        : view_ == QStringLiteral("starred") ? QStringLiteral("还没有星标图片。")
                        : !source_->capabilities().groups
                            ? QStringLiteral("此来源暂无图片。可选择目录，或添加图片后查看。")
                        : view_ == QStringLiteral("all")
                            ? QStringLiteral("还没有截图历史。完成截图后可在这里查看。")
                        : view_ == QStringLiteral("starred") ? QStringLiteral("还没有星标图片。")
                                                             : QStringLiteral("此分组暂无记录。"));
        empty_->show();
        return;
    }
    const bool preserveView = displayedId_ == selectedId() && displayedOriginal_ == showOriginal_ &&
                              !imageView_->image().isNull();
    // 切图前释放上一张主图；同条目刷新保留缩放和平移。
    if (!preserveView)
        imageView_->setImage({});
    const auto result = source_->loadImage(selectedId(), {}, showOriginal_);
    if (!preserveView || imageView_->image() != result.image)
        imageView_->setImage(result.image, preserveView);
    displayedId_ = selectedId();
    displayedOriginal_ = showOriginal_;
    imageView_->setVisible(result.success);
    empty_->setText(result.error);
    empty_->setVisible(!result.success);
}
QString HistoryWindow::selectedId() const
{
    const auto* entry = thumbnails_->currentItem();
    return entry ? entry->data(Qt::UserRole).toString() : QString();
}
std::optional<ResourceItem> HistoryWindow::findItem(const QString& id) const
{
    for (const auto& item : snapshot_.items)
        if (item.id == id)
            return item;
    return {};
}
void HistoryWindow::updateActions()
{
    const auto item = findItem(selectedId());
    const auto capabilities = source_->capabilities();
    star_->setEnabled(item && capabilities.star);
    star_->setChecked(item && item->starred);
    star_->setText(item && item->starred ? QStringLiteral("★ 星标") : QStringLiteral("☆ 星标"));
    copy_->setEnabled(item && source_->canCopy());
    if (original_)
    {
        const QSignalBlocker blocker(original_);
        original_->setVisible(item && item->hasOriginal && capabilities.originalVariant);
        original_->setChecked(showOriginal_ && item && item->hasOriginal &&
                              capabilities.originalVariant);
        original_->setText(showOriginal_ ? QStringLiteral("水印版") : QStringLiteral("原图"));
        original_->setToolTip(item && item->hasOriginal ? QStringLiteral("切换水印版 / 原图")
                                                        : QStringLiteral("这条记录没有保留原图"));
    }
    pin_->setEnabled(item && source_->canPin());
    reveal_->setEnabled(item && capabilities.reveal);
    delete_->setEnabled(item && item->deletable && capabilities.deleteItem);
    delete_->setToolTip(delete_->isEnabled() ? QStringLiteral("移入系统回收站")
                                             : QStringLiteral("请在文件管理器中操作"));
    clear_->setVisible(capabilities.clearAll);
    clear_->setEnabled(!snapshot_.items.isEmpty());
    configure_->setVisible(capabilities.configureDirectory);
}
void HistoryWindow::updateNotice()
{
    QStringList lines;
    if (!snapshot_.error.isEmpty())
        lines.append(snapshot_.error);
    const auto cleanup = snapshot_.notice;
    if (!cleanup.isEmpty())
        lines.append(cleanup);
    notice_->setText(lines.join(QLatin1Char('\n')));
    notice_->setVisible(!notice_->text().isEmpty());
}
void HistoryWindow::updateStatus()
{
    qint64 bytes = 0, protectedBytes = 0;
    qsizetype protectedCount = 0;
    const auto snapshot = source_->snapshot();
    for (const auto& group : snapshot.groups)
        if (view_ == QStringLiteral("all") || view_ == QStringLiteral("starred") ||
            view_ == group.id)
        {
            const auto usage = snapshot.groupUsage.value(group.id);
            protectedCount += usage.protectedCount;
            protectedBytes += usage.protectedBytes;
        }
    for (const auto& item : items_)
        bytes += item.bytes + item.originalBytes;
    if (!source_->capabilities().cleanup)
    {
        status_->setText(QStringLiteral("%1 · %2 张 · %3 MiB")
                             .arg(source_->displayName())
                             .arg(items_.size())
                             .arg(bytes / 1048576.0, 0, 'f', 2));
        return;
    }
    status_->setText(
        QStringLiteral("%1 张 · %2 MiB · 受保护 %3 张 / %4 MiB%5")
            .arg(items_.size())
            .arg(bytes / 1048576.0, 0, 'f', 2)
            .arg(protectedCount)
            .arg(protectedBytes / 1048576.0, 0, 'f', 2)
            .arg(view_ == QStringLiteral("starred") ? QStringLiteral("（全部分组）") : QString()));
}
void HistoryWindow::showActionResult(const ResourceStatus& result)
{
    updateNotice();
    if (!result.success)
    {
        notice_->setText(result.error);
        notice_->show();
    }
    updateActions();
}
void HistoryWindow::toggleStar()
{
    const auto item = findItem(selectedId());
    if (!item)
        return;
    const auto result = source_->setStarred(item->id, !item->starred);
    showActionResult(result);
}
void HistoryWindow::outputImage(bool pin, bool closeAfter)
{
    if (selectedId().isEmpty())
        return;
    const auto result = source_->outputImage(selectedId(), showOriginal_, pin);
    showActionResult(result);
    if (result.success && pin && closeAfter)
        close();
}
void HistoryWindow::deleteSelection()
{
    const auto result = source_->deleteItem(selectedId());
    showActionResult(result);
}
void HistoryWindow::refresh()
{
    source_->refresh();
    refreshFromSource();
}
void HistoryWindow::refreshFromSource()
{
    if (!source_->visible() && !source_->capabilities().configureDirectory)
    {
        selectSource({});
        return;
    }
    rebuildSources();
    const QString selected = selectedId();
    const int row = thumbnails_->currentRow();
    snapshot_ = source_->snapshot();
    rebuildGroups(view_);
    rebuildItems(selected, row);
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
        const auto result = source_->loadImage(entry->data(Qt::UserRole).toString(),
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
    state.lastSource = source_->id();
    state.lastView = view_;
    if (const auto* selected = thumbnails_->currentItem())
        state.lastSelection = selected->data(Qt::UserRole).toString();
    state.sortOrder =
        order_ == ResourceSort::NewestFirst ? QStringLiteral("newest") : QStringLiteral("oldest");
    const QString error = settings_.saveHistoryWindowState(state);
    if (!error.isEmpty())
        qWarning().noquote() << error;
}
void HistoryWindow::closeEvent(QCloseEvent* event)
{
    source_->setProtectedItem({});
    source_->setActive(false);
    saveWindowState();
    event->accept();
}
}
