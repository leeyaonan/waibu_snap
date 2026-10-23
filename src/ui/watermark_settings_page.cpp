#include "ui/watermark_settings_page.h"
#include "output/watermark_renderer.h"
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFontComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
namespace waibusnap
{
namespace
{
QImage previewBackground(QSize size)
{
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(143, 143, 143));
    QPainter painter(&image);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(130, 130, 130));
    for (int y = 0; y < size.height(); y += 16)
        for (int x = (y / 16 % 2) * 16; x < size.width(); x += 32)
            painter.drawRect(x, y, 16, 16);
    return image;
}
QImage previewFor(const WatermarkAsset& asset, const QImage& assetImage, WatermarkLayout layout,
                  int opacity, int scale, QSize size)
{
    QImage canvas = previewBackground(size);
    WatermarkInstance instance;
    instance.layout = layout;
    instance.opacity = opacity;
    instance.scale = scale;
    instance.normalizedCenter =
        watermarkInitialCenter(layout, watermarkBaseSize(asset, assetImage), canvas.size());
    return composeWatermark(canvas, asset, assetImage, instance);
}
QString imageError(const QString& error)
{
    return error.isEmpty() ? QStringLiteral("水印图片不可用。") : error;
}
class WatermarkAssetDialog final : public QDialog
{
  public:
    WatermarkAssetDialog(QWidget* parent, WatermarkAsset asset,
                         const WatermarkSettingsActions& actions, QSet<QString>* pendingNew)
        : QDialog(parent), asset_(std::move(asset)), actions_(actions), pendingNew_(pendingNew)
    {
        setWindowTitle(asset_.kind == WatermarkKind::Text ? QStringLiteral("文字水印资产")
                                                          : QStringLiteral("图片水印资产"));
        auto* layout = new QVBoxLayout(this);
        auto* form = new QFormLayout;
        name_ = new QLineEdit(asset_.name, this);
        name_->setObjectName(QStringLiteral("watermarkAssetName"));
        form->addRow(QStringLiteral("名称"), name_);
        if (asset_.kind == WatermarkKind::Text)
        {
            text_ = new QPlainTextEdit(asset_.text, this);
            text_->setObjectName(QStringLiteral("watermarkAssetText"));
            text_->setFixedHeight(74);
            form->addRow(QStringLiteral("文字"), text_);
            font_ = new QFontComboBox(this);
            font_->setObjectName(QStringLiteral("watermarkAssetFont"));
            if (!asset_.fontFamily.isEmpty())
                font_->setCurrentFont(QFont(asset_.fontFamily));
            form->addRow(QStringLiteral("字体"), font_);
            size_ = new QSpinBox(this);
            size_->setObjectName(QStringLiteral("watermarkAssetFontSize"));
            size_->setRange(watermarkMinimumFontPixelSize, watermarkMaximumFontPixelSize);
            size_->setSuffix(QStringLiteral(" px"));
            size_->setValue(asset_.fontPixelSize);
            form->addRow(QStringLiteral("字号"), size_);
            colorButton_ = new QPushButton(QStringLiteral("颜色…"), this);
            colorButton_->setObjectName(QStringLiteral("watermarkAssetColor"));
            setColor(asset_.color);
            form->addRow(QStringLiteral("颜色"), colorButton_);
            auto* styleRow = new QHBoxLayout;
            bold_ = new QCheckBox(QStringLiteral("粗体"), this);
            italic_ = new QCheckBox(QStringLiteral("斜体"), this);
            bold_->setChecked(asset_.bold);
            italic_->setChecked(asset_.italic);
            styleRow->addWidget(bold_);
            styleRow->addWidget(italic_);
            styleRow->addStretch();
            form->addRow(QStringLiteral("样式"), styleRow);
            connect(colorButton_, &QPushButton::clicked, this, [this] { chooseColor(); });
            connect(text_, &QPlainTextEdit::textChanged, this, [this] { updatePreview(); });
            connect(font_, &QFontComboBox::currentFontChanged, this, [this] { updatePreview(); });
            connect(size_, &QSpinBox::valueChanged, this, [this] { updatePreview(); });
            connect(bold_, &QCheckBox::toggled, this, [this] { updatePreview(); });
            connect(italic_, &QCheckBox::toggled, this, [this] { updatePreview(); });
        }
        else
        {
            auto* imageRow = new QHBoxLayout;
            imagePath_ = new QLineEdit(asset_.imageFileName, this);
            imagePath_->setObjectName(QStringLiteral("watermarkAssetImage"));
            imagePath_->setReadOnly(true);
            auto* choose = new QPushButton(QStringLiteral("选择…"), this);
            choose->setObjectName(QStringLiteral("watermarkAssetChooseImage"));
            imageRow->addWidget(imagePath_, 1);
            imageRow->addWidget(choose);
            form->addRow(QStringLiteral("图片"), imageRow);
            connect(choose, &QPushButton::clicked, this, [this] { chooseImage(); });
        }
        layout->addLayout(form);
        preview_ = new QLabel(this);
        preview_->setObjectName(QStringLiteral("watermarkAssetPreview"));
        preview_->setFixedSize(280, 140);
        preview_->setAlignment(Qt::AlignCenter);
        layout->addWidget(preview_);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("确定"));
        buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
        connect(buttons, &QDialogButtonBox::accepted, this, &WatermarkAssetDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        layout->addWidget(buttons);
        updatePreview();
    }
    WatermarkAsset asset() const { return asset_; }

  protected:
    void reject() override { QDialog::reject(); }
    void accept() override
    {
        asset_.name = name_->text().trimmed();
        if (asset_.name.isEmpty())
        {
            QMessageBox::warning(this, QStringLiteral("水印资产"),
                                 QStringLiteral("名称不能为空。"));
            return;
        }
        if (asset_.kind == WatermarkKind::Text)
        {
            asset_.text = text_->toPlainText();
            if (asset_.text.trimmed().isEmpty())
            {
                QMessageBox::warning(this, QStringLiteral("水印资产"),
                                     QStringLiteral("文字不能为空。"));
                return;
            }
            asset_.fontFamily = font_->currentFont().family();
            asset_.fontPixelSize = size_->value();
            asset_.bold = bold_->isChecked();
            asset_.italic = italic_->isChecked();
        }
        else if (asset_.imageFileName.isEmpty())
        {
            QMessageBox::warning(this, QStringLiteral("水印资产"),
                                 QStringLiteral("请先选择水印图片。"));
            return;
        }
        QDialog::accept();
    }

  private:
    void setColor(QColor color)
    {
        color_ = color;
        asset_.color = color;
        colorButton_->setStyleSheet(QStringLiteral("background:%1; color:%2;")
                                        .arg(color.name(), color.lightness() < 128
                                                               ? QStringLiteral("white")
                                                               : QStringLiteral("black")));
        colorButton_->setText(color.name());
    }
    void chooseColor()
    {
        const QColor selected = QColorDialog::getColor(color_, this, QStringLiteral("水印颜色"));
        if (selected.isValid())
        {
            setColor(selected);
            updatePreview();
        }
    }
    void chooseImage()
    {
        QString path;
        if (actions_.chooseImage)
            path = actions_.chooseImage(this, {});
        if (path.isEmpty())
            path = QFileDialog::getOpenFileName(this, QStringLiteral("选择水印图片"), {},
                                                QStringLiteral("图片 (*.png *.jpg *.jpeg *.webp)"));
        if (path.isEmpty())
            return;
        if (!actions_.importImage)
        {
            QMessageBox::warning(this, QStringLiteral("水印资产"),
                                 QStringLiteral("当前无法导入水印图片。"));
            return;
        }
        QString managed;
        QString error;
        const QString result = actions_.importImage(path, &managed, &error);
        if (!result.isEmpty() || managed.isEmpty())
        {
            QMessageBox::warning(this, QStringLiteral("导入失败"), imageError(error));
            return;
        }
        asset_.imageFileName = managed;
        imagePath_->setText(managed);
        if (pendingNew_)
            pendingNew_->insert(managed);
        updatePreview();
    }
    QImage currentImage(QString* error = nullptr) const
    {
        if (asset_.kind != WatermarkKind::Image || asset_.imageFileName.isEmpty())
            return {};
        if (actions_.loadManagedImage)
            return actions_.loadManagedImage(asset_.imageFileName, error);
        return {};
    }
    void updatePreview()
    {
        if (!preview_)
            return;
        WatermarkAsset previewAsset = asset_;
        if (text_)
        {
            previewAsset.name = name_->text();
            previewAsset.text = text_->toPlainText();
            previewAsset.fontFamily = font_->currentFont().family();
            previewAsset.fontPixelSize = size_->value();
            previewAsset.bold = bold_->isChecked();
            previewAsset.italic = italic_->isChecked();
        }
        QString error;
        const QImage image = currentImage(&error);
        const QImage preview =
            previewFor(previewAsset, image, WatermarkLayout::Center, 100, 100, preview_->size());
        preview_->setPixmap(QPixmap::fromImage(preview));
    }
    WatermarkAsset asset_;
    WatermarkSettingsActions actions_;
    QSet<QString>* pendingNew_ = nullptr;
    QLineEdit* name_ = nullptr;
    QPlainTextEdit* text_ = nullptr;
    QFontComboBox* font_ = nullptr;
    QSpinBox* size_ = nullptr;
    QPushButton* colorButton_ = nullptr;
    QColor color_ = Qt::white;
    QCheckBox* bold_ = nullptr;
    QCheckBox* italic_ = nullptr;
    QLineEdit* imagePath_ = nullptr;
    QLabel* preview_ = nullptr;
};
class WatermarkPresetDialog final : public QDialog
{
  public:
    WatermarkPresetDialog(QWidget* parent, WatermarkPreset preset,
                          const QVector<WatermarkAsset>& assets,
                          const WatermarkSettingsActions& actions)
        : QDialog(parent), preset_(std::move(preset)), assets_(assets), actions_(actions)
    {
        setWindowTitle(QStringLiteral("水印预设"));
        auto* layout = new QVBoxLayout(this);
        auto* form = new QFormLayout;
        name_ = new QLineEdit(preset_.name, this);
        name_->setObjectName(QStringLiteral("watermarkPresetName"));
        form->addRow(QStringLiteral("名称"), name_);
        asset_ = new QComboBox(this);
        asset_->setObjectName(QStringLiteral("watermarkPresetAsset"));
        for (const auto& asset : assets_)
            asset_->addItem(asset.name, asset.id);
        const int assetIndex = asset_->findData(preset_.assetId);
        if (assetIndex >= 0)
            asset_->setCurrentIndex(assetIndex);
        form->addRow(QStringLiteral("水印"), asset_);
        layout_ = new QComboBox(this);
        layout_->setObjectName(QStringLiteral("watermarkPresetLayout"));
        for (auto value : watermarkLayouts())
            layout_->addItem(watermarkLayoutName(value), int(value));
        layout_->setCurrentIndex(layout_->findData(int(preset_.layout)));
        form->addRow(QStringLiteral("排布"), layout_);
        opacity_ = new QSpinBox(this);
        opacity_->setObjectName(QStringLiteral("watermarkPresetOpacity"));
        opacity_->setRange(watermarkMinimumOpacity, watermarkMaximumOpacity);
        opacity_->setSuffix(QStringLiteral(" %"));
        opacity_->setValue(preset_.opacity);
        form->addRow(QStringLiteral("透明度"), opacity_);
        scale_ = new QSpinBox(this);
        scale_->setObjectName(QStringLiteral("watermarkPresetScale"));
        scale_->setRange(watermarkMinimumScale, watermarkMaximumScale);
        scale_->setSuffix(QStringLiteral(" %"));
        scale_->setValue(preset_.scale);
        form->addRow(QStringLiteral("缩放"), scale_);
        layout->addLayout(form);
        preview_ = new QLabel(this);
        preview_->setObjectName(QStringLiteral("watermarkPresetPreview"));
        preview_->setFixedSize(320, 180);
        preview_->setAlignment(Qt::AlignCenter);
        layout->addWidget(preview_);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("确定"));
        buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
        connect(buttons, &QDialogButtonBox::accepted, this, &WatermarkPresetDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        layout->addWidget(buttons);
        connect(asset_, &QComboBox::currentIndexChanged, this, [this] { updatePreview(); });
        connect(layout_, &QComboBox::currentIndexChanged, this, [this] { updatePreview(); });
        connect(opacity_, &QSpinBox::valueChanged, this, [this] { updatePreview(); });
        connect(scale_, &QSpinBox::valueChanged, this, [this] { updatePreview(); });
        updatePreview();
    }
    WatermarkPreset preset() const { return preset_; }

  protected:
    void accept() override
    {
        preset_.name = name_->text().trimmed();
        if (preset_.name.isEmpty())
        {
            QMessageBox::warning(this, QStringLiteral("水印预设"),
                                 QStringLiteral("名称不能为空。"));
            return;
        }
        preset_.assetId = asset_->currentData().toString();
        preset_.layout = WatermarkLayout(layout_->currentData().toInt());
        preset_.opacity = opacity_->value();
        preset_.scale = scale_->value();
        QDialog::accept();
    }

  private:
    void updatePreview()
    {
        if (!preview_)
            return;
        const QString assetId = asset_->currentData().toString();
        const auto found = std::find_if(assets_.begin(), assets_.end(),
                                        [&](const auto& asset) { return asset.id == assetId; });
        if (found == assets_.end())
        {
            preview_->clear();
            return;
        }
        QImage image;
        QString error;
        if (found->kind == WatermarkKind::Image && actions_.loadManagedImage)
            image = actions_.loadManagedImage(found->imageFileName, &error);
        const QImage preview =
            previewFor(*found, image, WatermarkLayout(layout_->currentData().toInt()),
                       opacity_->value(), scale_->value(), preview_->size());
        preview_->setPixmap(QPixmap::fromImage(preview));
    }
    WatermarkPreset preset_;
    QVector<WatermarkAsset> assets_;
    WatermarkSettingsActions actions_;
    QLineEdit* name_ = nullptr;
    QComboBox* asset_ = nullptr;
    QComboBox* layout_ = nullptr;
    QSpinBox* opacity_ = nullptr;
    QSpinBox* scale_ = nullptr;
    QLabel* preview_ = nullptr;
};
}
WatermarkSettingsPage::WatermarkSettingsPage(WatermarkSettingsActions actions, QWidget* parent)
    : QWidget(parent), actions_(std::move(actions)), draft_(actions_.preferences)
{
    setObjectName(QStringLiteral("settingsWatermarkPage"));
    auto* layout = new QVBoxLayout(this);
    auto* help = new QLabel(
        QStringLiteral("先创建水印资产，再用水印资产 + 排布 + 透明度 + 缩放组合成预设。"
                       "最多 10 套预设；当前预设由工具栏 / 托盘切换，重启回到默认预设。"),
        this);
    help->setObjectName(QStringLiteral("watermarkHelp"));
    help->setWordWrap(true);
    layout->addWidget(help);

    auto* assetsGroup = new QGroupBox(QStringLiteral("水印资产"), this);
    auto* assetsLayout = new QVBoxLayout(assetsGroup);
    assets_ = new QListWidget(assetsGroup);
    assets_->setObjectName(QStringLiteral("watermarkAssetsList"));
    assets_->setMaximumHeight(78);
    assetsLayout->addWidget(assets_);
    auto* assetButtons = new QHBoxLayout;
    addTextButton_ = new QPushButton(QStringLiteral("新增文字"), assetsGroup);
    addTextButton_->setObjectName(QStringLiteral("watermarkAddTextButton"));
    addImageButton_ = new QPushButton(QStringLiteral("新增图片"), assetsGroup);
    addImageButton_->setObjectName(QStringLiteral("watermarkAddImageButton"));
    editAssetButton_ = new QPushButton(QStringLiteral("编辑…"), assetsGroup);
    editAssetButton_->setObjectName(QStringLiteral("watermarkEditAssetButton"));
    deleteAssetButton_ = new QPushButton(QStringLiteral("删除"), assetsGroup);
    deleteAssetButton_->setObjectName(QStringLiteral("watermarkDeleteAssetButton"));
    assetButtons->addWidget(addTextButton_);
    assetButtons->addWidget(addImageButton_);
    assetButtons->addWidget(editAssetButton_);
    assetButtons->addWidget(deleteAssetButton_);
    assetButtons->addStretch();
    assetsLayout->addLayout(assetButtons);
    assetPreview_ = new QLabel(assetsGroup);
    assetPreview_->setObjectName(QStringLiteral("watermarkAssetPreview"));
    assetPreview_->setFixedSize(200, 80);
    assetPreview_->setAlignment(Qt::AlignCenter);
    assetsLayout->addWidget(assetPreview_, 0, Qt::AlignHCenter);
    layout->addWidget(assetsGroup);

    auto* presetsGroup = new QGroupBox(QStringLiteral("水印预设"), this);
    auto* presetsLayout = new QVBoxLayout(presetsGroup);
    presets_ = new QListWidget(presetsGroup);
    presets_->setObjectName(QStringLiteral("watermarkPresetsList"));
    presets_->setMaximumHeight(78);
    presetsLayout->addWidget(presets_);
    auto* presetButtons = new QHBoxLayout;
    addPresetButton_ = new QPushButton(QStringLiteral("新增预设"), presetsGroup);
    addPresetButton_->setObjectName(QStringLiteral("watermarkAddPresetButton"));
    editPresetButton_ = new QPushButton(QStringLiteral("编辑…"), presetsGroup);
    editPresetButton_->setObjectName(QStringLiteral("watermarkEditPresetButton"));
    duplicatePresetButton_ = new QPushButton(QStringLiteral("复制"), presetsGroup);
    duplicatePresetButton_->setObjectName(QStringLiteral("watermarkDuplicatePresetButton"));
    deletePresetButton_ = new QPushButton(QStringLiteral("删除"), presetsGroup);
    deletePresetButton_->setObjectName(QStringLiteral("watermarkDeletePresetButton"));
    defaultPresetButton_ = new QPushButton(QStringLiteral("设为默认"), presetsGroup);
    defaultPresetButton_->setObjectName(QStringLiteral("watermarkSetDefaultButton"));
    presetButtons->addWidget(addPresetButton_);
    presetButtons->addWidget(editPresetButton_);
    presetButtons->addWidget(duplicatePresetButton_);
    presetButtons->addWidget(deletePresetButton_);
    presetButtons->addWidget(defaultPresetButton_);
    presetButtons->addStretch();
    presetsLayout->addLayout(presetButtons);
    presetPreview_ = new QLabel(presetsGroup);
    presetPreview_->setObjectName(QStringLiteral("watermarkPresetPreview"));
    presetPreview_->setFixedSize(240, 96);
    presetPreview_->setAlignment(Qt::AlignCenter);
    presetsLayout->addWidget(presetPreview_, 0, Qt::AlignHCenter);
    layout->addWidget(presetsGroup);

    autoCheck_ = new QCheckBox(QStringLiteral("常驻水印（后续截图自动使用当前预设）"), this);
    autoCheck_->setObjectName(QStringLiteral("watermarkAutoCheck"));
    autoCheck_->setChecked(draft_.autoEnabled);
    layout->addWidget(autoCheck_);
    retainOriginalCheck_ = new QCheckBox(QStringLiteral("保留原始图片（需开启历史）"), this);
    retainOriginalCheck_->setObjectName(QStringLiteral("watermarkRetainOriginalCheck"));
    retainOriginalCheck_->setChecked(draft_.retainOriginal);
    layout->addWidget(retainOriginalCheck_);
    notice_ = new QLabel(this);
    notice_->setObjectName(QStringLiteral("watermarkPageNotice"));
    notice_->setWordWrap(true);
    notice_->setTextFormat(Qt::PlainText);
    layout->addWidget(notice_);
    layout->addStretch();

    connect(assets_, &QListWidget::currentRowChanged, this, [this] { updatePreviews(); });
    connect(presets_, &QListWidget::currentRowChanged, this, [this] { updatePreviews(); });
    connect(addTextButton_, &QPushButton::clicked, this, [this] { addAsset(WatermarkKind::Text); });
    connect(addImageButton_, &QPushButton::clicked, this,
            [this] { addAsset(WatermarkKind::Image); });
    connect(editAssetButton_, &QPushButton::clicked, this, [this] { editAsset(); });
    connect(deleteAssetButton_, &QPushButton::clicked, this, [this] { deleteAsset(); });
    connect(addPresetButton_, &QPushButton::clicked, this, [this] { addPreset(); });
    connect(editPresetButton_, &QPushButton::clicked, this, [this] { editPreset(); });
    connect(duplicatePresetButton_, &QPushButton::clicked, this, [this] { duplicatePreset(); });
    connect(deletePresetButton_, &QPushButton::clicked, this, [this] { deletePreset(); });
    connect(defaultPresetButton_, &QPushButton::clicked, this, [this] { setDefaultPreset(); });
    connect(autoCheck_, &QCheckBox::toggled, this, [this] { updatePreviews(); });
    connect(retainOriginalCheck_, &QCheckBox::toggled, this, [this] { updatePreviews(); });
    setHistoryEnabled(actions_.historyEnabled);
    rebuild();
}
WatermarkSettingsPage::~WatermarkSettingsPage()
{
    if (!saved_)
        discardPendingAssets();
}
WatermarkPreferences WatermarkSettingsPage::collect() const
{
    WatermarkPreferences result = draft_;
    result.autoEnabled = autoCheck_ && autoCheck_->isChecked() && !result.presets.isEmpty();
    result.retainOriginal = retainOriginalCheck_ && retainOriginalCheck_->isChecked();
    if (result.presets.isEmpty())
    {
        result.defaultPresetId.clear();
        result.autoEnabled = false;
    }
    return result;
}
QString WatermarkSettingsPage::validationError(bool historyEnabled) const
{
    const auto preferences = collect();
    const QString error = watermarkPreferencesError(preferences);
    if (!error.isEmpty())
        return error;
    if (preferences.retainOriginal && !historyEnabled)
        return QStringLiteral("保留原始图片需要先开启历史。");
    for (const auto& asset : preferences.assets)
        if (asset.kind == WatermarkKind::Image && asset.imageFileName.isEmpty())
            return QStringLiteral("水印图片资产「%1」缺少图片。").arg(asset.name);
    return {};
}
void WatermarkSettingsPage::setHistoryEnabled(bool enabled)
{
    if (!retainOriginalCheck_)
        return;
    retainOriginalCheck_->setEnabled(enabled);
    if (!enabled)
        retainOriginalCheck_->setChecked(false);
}
QString WatermarkSettingsPage::commitPendingAssets()
{
    QStringList referenced;
    for (const auto& asset : draft_.assets)
        if (asset.kind == WatermarkKind::Image && !asset.imageFileName.isEmpty())
            referenced.append(asset.imageFileName);
    QStringList errors;
    const auto removeIfUnreferenced = [&](const QString& fileName)
    {
        if (fileName.isEmpty() || referenced.contains(fileName))
            return;
        if (actions_.removeAssetImage)
        {
            const QString error = actions_.removeAssetImage(fileName);
            if (!error.isEmpty())
                errors.append(error);
        }
    };
    for (const auto& fileName : pendingNewImages_)
        removeIfUnreferenced(fileName);
    for (const auto& fileName : pendingOldImages_)
        removeIfUnreferenced(fileName);
    pendingNewImages_.clear();
    pendingOldImages_.clear();
    saved_ = true;
    return errors.join(QLatin1Char('\n'));
}
void WatermarkSettingsPage::discardPendingAssets()
{
    if (actions_.removeAssetImage)
        for (const auto& fileName : pendingNewImages_)
            actions_.removeAssetImage(fileName);
    pendingNewImages_.clear();
    pendingOldImages_.clear();
}
void WatermarkSettingsPage::rebuild()
{
    rebuildAssets(selectedAssetId());
    rebuildPresets(selectedPresetId());
    updatePreviews();
}
void WatermarkSettingsPage::rebuildAssets(const QString& selected)
{
    if (!assets_)
        return;
    const QSignalBlocker blocker(assets_);
    assets_->clear();
    for (const auto& asset : draft_.assets)
    {
        const QString type =
            asset.kind == WatermarkKind::Text ? QStringLiteral("文字") : QStringLiteral("图片");
        auto* item = new QListWidgetItem(QStringLiteral("%1 · %2 · 引用 %3 套")
                                             .arg(asset.name, type)
                                             .arg(presetReferenceCount(asset.id)),
                                         assets_);
        item->setData(Qt::UserRole, asset.id);
    }
    for (int index = 0; index < assets_->count(); ++index)
        if (assets_->item(index)->data(Qt::UserRole).toString() == selected)
            assets_->setCurrentRow(index);
    if (assets_->currentRow() < 0 && assets_->count() > 0)
        assets_->setCurrentRow(0);
    const bool hasSelection = assets_->currentRow() >= 0;
    editAssetButton_->setEnabled(hasSelection);
    deleteAssetButton_->setEnabled(hasSelection);
    if (addPresetButton_)
        addPresetButton_->setEnabled(!draft_.assets.isEmpty() &&
                                     draft_.presets.size() < watermarkMaximumPresets);
    updatePreviews();
}
void WatermarkSettingsPage::rebuildPresets(const QString& selected)
{
    if (!presets_)
        return;
    const QSignalBlocker blocker(presets_);
    presets_->clear();
    for (const auto& preset : draft_.presets)
    {
        const auto* asset = findWatermarkAsset(draft_, preset.assetId);
        const QString name = asset ? asset->name : QStringLiteral("资产缺失");
        const QString prefix =
            preset.id == draft_.defaultPresetId ? QStringLiteral("默认 · ") : QString();
        auto* item = new QListWidgetItem(
            QStringLiteral("%1%2 · %3 · %4 · %5% · %6%")
                .arg(prefix, preset.name, name, watermarkLayoutName(preset.layout))
                .arg(preset.opacity)
                .arg(preset.scale),
            presets_);
        item->setData(Qt::UserRole, preset.id);
    }
    for (int index = 0; index < presets_->count(); ++index)
        if (presets_->item(index)->data(Qt::UserRole).toString() == selected)
            presets_->setCurrentRow(index);
    if (presets_->currentRow() < 0 && presets_->count() > 0)
        presets_->setCurrentRow(0);
    const bool hasSelection = presets_->currentRow() >= 0;
    editPresetButton_->setEnabled(hasSelection);
    duplicatePresetButton_->setEnabled(hasSelection &&
                                       draft_.presets.size() < watermarkMaximumPresets);
    deletePresetButton_->setEnabled(hasSelection);
    defaultPresetButton_->setEnabled(hasSelection && !selectedPresetId().isEmpty() &&
                                     selectedPresetId() != draft_.defaultPresetId);
    addPresetButton_->setEnabled(!draft_.assets.isEmpty() &&
                                 draft_.presets.size() < watermarkMaximumPresets);
    autoCheck_->setEnabled(!draft_.presets.isEmpty());
    if (draft_.presets.isEmpty())
        autoCheck_->setChecked(false);
}
QString WatermarkSettingsPage::selectedAssetId() const
{
    const auto* item = assets_ ? assets_->currentItem() : nullptr;
    return item ? item->data(Qt::UserRole).toString() : QString();
}
QString WatermarkSettingsPage::selectedPresetId() const
{
    const auto* item = presets_ ? presets_->currentItem() : nullptr;
    return item ? item->data(Qt::UserRole).toString() : QString();
}
WatermarkAsset* WatermarkSettingsPage::assetById(const QString& id)
{
    for (auto& asset : draft_.assets)
        if (asset.id == id)
            return &asset;
    return nullptr;
}
WatermarkPreset* WatermarkSettingsPage::presetById(const QString& id)
{
    for (auto& preset : draft_.presets)
        if (preset.id == id)
            return &preset;
    return nullptr;
}
int WatermarkSettingsPage::presetReferenceCount(const QString& assetId) const
{
    return int(std::count_if(draft_.presets.begin(), draft_.presets.end(),
                             [&](const auto& preset) { return preset.assetId == assetId; }));
}
void WatermarkSettingsPage::addAsset(WatermarkKind kind)
{
    WatermarkAsset asset;
    asset.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    asset.kind = kind;
    asset.name =
        kind == WatermarkKind::Text ? QStringLiteral("文字水印") : QStringLiteral("图片水印");
    if (kind == WatermarkKind::Text)
        asset.text = QStringLiteral("WaibuSnap");
    WatermarkAssetDialog dialog(this, asset, actions_, &pendingNewImages_);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const auto result = dialog.asset();
    if (std::any_of(draft_.assets.begin(), draft_.assets.end(),
                    [&](const auto& existing) { return existing.name == result.name; }))
    {
        showNotice(QStringLiteral("水印资产名称已存在。"));
        return;
    }
    draft_.assets.append(result);
    rebuildAssets(result.id);
}
void WatermarkSettingsPage::editAsset()
{
    auto* asset = assetById(selectedAssetId());
    if (!asset)
        return;
    const WatermarkAsset before = *asset;
    WatermarkAssetDialog dialog(this, before, actions_, &pendingNewImages_);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const auto result = dialog.asset();
    if (std::any_of(draft_.assets.begin(), draft_.assets.end(), [&](const auto& existing)
                    { return existing.id != result.id && existing.name == result.name; }))
    {
        showNotice(QStringLiteral("水印资产名称已存在。"));
        return;
    }
    if (before.kind == WatermarkKind::Image && before.imageFileName != result.imageFileName &&
        !before.imageFileName.isEmpty())
        pendingOldImages_.insert(before.imageFileName);
    *asset = result;
    rebuildAssets(result.id);
}
void WatermarkSettingsPage::deleteAsset()
{
    auto* asset = assetById(selectedAssetId());
    if (!asset)
        return;
    const int references = presetReferenceCount(asset->id);
    if (references > 0)
    {
        showNotice(QStringLiteral("该资产正被 %1 套预设使用，请先解除引用。").arg(references));
        return;
    }
    if (QMessageBox::question(this, QStringLiteral("删除水印资产"),
                              QStringLiteral("删除「%1」？").arg(asset->name)) != QMessageBox::Yes)
        return;
    if (asset->kind == WatermarkKind::Image && !asset->imageFileName.isEmpty())
        pendingOldImages_.insert(asset->imageFileName);
    const QString id = asset->id;
    draft_.assets.erase(std::remove_if(draft_.assets.begin(), draft_.assets.end(),
                                       [&](const auto& value) { return value.id == id; }),
                        draft_.assets.end());
    rebuildAssets();
}
void WatermarkSettingsPage::addPreset()
{
    if (draft_.assets.isEmpty())
    {
        showNotice(QStringLiteral("请先创建至少一个水印资产。"));
        return;
    }
    if (draft_.presets.size() >= watermarkMaximumPresets)
    {
        showNotice(QStringLiteral("最多 10 套水印预设，请先删除已有预设。"));
        return;
    }
    WatermarkPreset preset;
    preset.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    preset.name = QStringLiteral("水印预设 %1").arg(draft_.presets.size() + 1);
    preset.assetId = draft_.assets.first().id;
    preset.layout = WatermarkLayout::BottomRight;
    WatermarkPresetDialog dialog(this, preset, draft_.assets, actions_);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const auto result = dialog.preset();
    if (std::any_of(draft_.presets.begin(), draft_.presets.end(),
                    [&](const auto& existing) { return existing.name == result.name; }))
    {
        showNotice(QStringLiteral("水印预设名称已存在。"));
        return;
    }
    draft_.presets.append(result);
    if (draft_.defaultPresetId.isEmpty())
        draft_.defaultPresetId = result.id;
    rebuildPresets(result.id);
}
void WatermarkSettingsPage::editPreset()
{
    auto* preset = presetById(selectedPresetId());
    if (!preset || draft_.assets.isEmpty())
        return;
    WatermarkPresetDialog dialog(this, *preset, draft_.assets, actions_);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const auto result = dialog.preset();
    if (std::any_of(draft_.presets.begin(), draft_.presets.end(), [&](const auto& existing)
                    { return existing.id != result.id && existing.name == result.name; }))
    {
        showNotice(QStringLiteral("水印预设名称已存在。"));
        return;
    }
    *preset = result;
    rebuildPresets(result.id);
}
void WatermarkSettingsPage::duplicatePreset()
{
    const auto* source = presetById(selectedPresetId());
    if (!source || draft_.presets.size() >= watermarkMaximumPresets)
        return;
    WatermarkPreset copy = *source;
    copy.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QString name = copy.name + QStringLiteral(" 副本");
    int suffix = 2;
    while (std::any_of(draft_.presets.begin(), draft_.presets.end(),
                       [&](const auto& preset) { return preset.name == name; }))
        name = copy.name + QStringLiteral(" 副本 %1").arg(suffix++);
    copy.name = name;
    draft_.presets.append(copy);
    rebuildPresets(copy.id);
}
void WatermarkSettingsPage::deletePreset()
{
    auto* preset = presetById(selectedPresetId());
    if (!preset)
        return;
    if (QMessageBox::question(this, QStringLiteral("删除水印预设"),
                              QStringLiteral("删除「%1」？").arg(preset->name)) != QMessageBox::Yes)
        return;
    const QString id = preset->id;
    const bool wasDefault = draft_.defaultPresetId == id;
    draft_.presets.erase(std::remove_if(draft_.presets.begin(), draft_.presets.end(),
                                        [&](const auto& value) { return value.id == id; }),
                         draft_.presets.end());
    if (wasDefault)
        draft_.defaultPresetId = draft_.presets.isEmpty() ? QString() : draft_.presets.first().id;
    rebuildPresets();
}
void WatermarkSettingsPage::setDefaultPreset()
{
    const QString id = selectedPresetId();
    if (id.isEmpty())
        return;
    draft_.defaultPresetId = id;
    rebuildPresets(id);
}
void WatermarkSettingsPage::updatePreviews()
{
    if (!assets_ || !presets_)
        return;
    const auto* asset = findWatermarkAsset(draft_, selectedAssetId());
    if (!asset)
    {
        assetPreview_->clear();
        assetPreview_->setText(QStringLiteral("尚无资产"));
    }
    else
    {
        QString error;
        const QImage image = assetImage(*asset, &error);
        const QImage preview =
            previewFor(*asset, image, WatermarkLayout::Center, 100, 100, assetPreview_->size());
        assetPreview_->setPixmap(QPixmap::fromImage(preview));
        assetPreview_->setToolTip(error);
    }
    const auto* preset = findWatermarkPreset(draft_, selectedPresetId());
    if (!preset || !asset)
    {
        presetPreview_->clear();
        presetPreview_->setText(QStringLiteral("尚无预设"));
        return;
    }
    const auto* presetAsset = findWatermarkAsset(draft_, preset->assetId);
    if (!presetAsset)
    {
        presetPreview_->clear();
        presetPreview_->setText(QStringLiteral("预设资产不可用"));
        return;
    }
    QString error;
    const QImage image = assetImage(*presetAsset, &error);
    const QImage preview = previewFor(*presetAsset, image, preset->layout, preset->opacity,
                                      preset->scale, presetPreview_->size());
    presetPreview_->setPixmap(QPixmap::fromImage(preview));
    presetPreview_->setToolTip(error);
}
QImage WatermarkSettingsPage::assetImage(const WatermarkAsset& asset, QString* error) const
{
    if (error)
        error->clear();
    if (asset.kind != WatermarkKind::Image)
        return {};
    if (actions_.loadManagedImage && !asset.imageFileName.isEmpty())
        return actions_.loadManagedImage(asset.imageFileName, error);
    return {};
}
void WatermarkSettingsPage::showNotice(const QString& text) { notice_->setText(text); }
}
