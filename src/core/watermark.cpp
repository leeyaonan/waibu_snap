#include "core/watermark.h"
#include <QFileInfo>
#include <QJsonArray>
#include <QSet>
#include <algorithm>
namespace waibusnap
{
namespace
{
const std::array<WatermarkLayout, 7> layouts = {
    WatermarkLayout::TopLeft,        WatermarkLayout::TopRight, WatermarkLayout::BottomLeft,
    WatermarkLayout::BottomRight,    WatermarkLayout::Center,   WatermarkLayout::DiagonalForward,
    WatermarkLayout::DiagonalReverse};
bool validKind(WatermarkKind kind)
{
    return kind == WatermarkKind::Text || kind == WatermarkKind::Image;
}
bool validLayout(WatermarkLayout layout)
{
    return std::find(layouts.begin(), layouts.end(), layout) != layouts.end();
}
QString assetError(const WatermarkAsset& asset)
{
    if (asset.id.trimmed().isEmpty())
        return QStringLiteral("水印资产缺少 ID。");
    if (asset.name.trimmed().isEmpty())
        return QStringLiteral("水印资产名称不能为空。");
    if (!validKind(asset.kind))
        return QStringLiteral("水印资产类型无效。");
    if (asset.kind == WatermarkKind::Text)
    {
        if (asset.text.trimmed().isEmpty())
            return QStringLiteral("水印文字不能为空。");
        if (asset.fontPixelSize < watermarkMinimumFontPixelSize ||
            asset.fontPixelSize > watermarkMaximumFontPixelSize)
            return QStringLiteral("水印字号必须在 %1–%2 px 之间。")
                .arg(watermarkMinimumFontPixelSize)
                .arg(watermarkMaximumFontPixelSize);
        if (!asset.color.isValid())
            return QStringLiteral("水印颜色无效。");
    }
    else
    {
        if (asset.imageFileName.trimmed().isEmpty())
            return QStringLiteral("水印图片文件不能为空。");
        const QFileInfo file(asset.imageFileName);
        if (file.isAbsolute() || asset.imageFileName.contains(QLatin1Char('/')) ||
            asset.imageFileName.contains(QLatin1Char('\\')))
            return QStringLiteral("水印图片必须使用受管相对文件名。");
    }
    return {};
}
QString presetError(const WatermarkPreset& preset, const WatermarkPreferences& preferences)
{
    if (preset.id.trimmed().isEmpty())
        return QStringLiteral("水印预设缺少 ID。");
    if (preset.name.trimmed().isEmpty())
        return QStringLiteral("水印预设名称不能为空。");
    if (!findWatermarkAsset(preferences, preset.assetId))
        return QStringLiteral("水印预设「%1」引用了不存在的水印资产。").arg(preset.name);
    if (!validLayout(preset.layout))
        return QStringLiteral("水印预设「%1」的排布无效。").arg(preset.name);
    if (preset.opacity < watermarkMinimumOpacity || preset.opacity > watermarkMaximumOpacity)
        return QStringLiteral("水印预设「%1」的透明度必须在 %2–%3%% 之间。")
            .arg(preset.name)
            .arg(watermarkMinimumOpacity)
            .arg(watermarkMaximumOpacity);
    if (preset.scale < watermarkMinimumScale || preset.scale > watermarkMaximumScale)
        return QStringLiteral("水印预设「%1」的缩放必须在 %2–%3%% 之间。")
            .arg(preset.name)
            .arg(watermarkMinimumScale)
            .arg(watermarkMaximumScale);
    return {};
}
bool readString(const QJsonObject& object, const QString& key, QString& value, QString* error)
{
    const auto json = object.value(key);
    if (!json.isString())
    {
        if (error)
            *error = QStringLiteral("字段 %1 必须是字符串。").arg(key);
        return false;
    }
    value = json.toString();
    return true;
}
bool readInt(const QJsonObject& object, const QString& key, int& value, QString* error)
{
    const auto json = object.value(key);
    if (!json.isDouble())
    {
        if (error)
            *error = QStringLiteral("字段 %1 必须是整数。").arg(key);
        return false;
    }
    value = json.toInt();
    return true;
}
bool readBool(const QJsonObject& object, const QString& key, bool& value, QString* error)
{
    const auto json = object.value(key);
    if (!json.isBool())
    {
        if (error)
            *error = QStringLiteral("字段 %1 必须是布尔值。").arg(key);
        return false;
    }
    value = json.toBool();
    return true;
}
}
const std::array<WatermarkLayout, 7>& watermarkLayouts() { return layouts; }
QString watermarkKindKey(WatermarkKind kind)
{
    return kind == WatermarkKind::Image ? QStringLiteral("image") : QStringLiteral("text");
}
QString watermarkLayoutKey(WatermarkLayout layout)
{
    switch (layout)
    {
    case WatermarkLayout::TopLeft:
        return QStringLiteral("topLeft");
    case WatermarkLayout::TopRight:
        return QStringLiteral("topRight");
    case WatermarkLayout::BottomLeft:
        return QStringLiteral("bottomLeft");
    case WatermarkLayout::BottomRight:
        return QStringLiteral("bottomRight");
    case WatermarkLayout::Center:
        return QStringLiteral("center");
    case WatermarkLayout::DiagonalForward:
        return QStringLiteral("diagonalForward");
    case WatermarkLayout::DiagonalReverse:
        return QStringLiteral("diagonalReverse");
    }
    return QStringLiteral("bottomRight");
}
QString watermarkLayoutName(WatermarkLayout layout)
{
    switch (layout)
    {
    case WatermarkLayout::TopLeft:
        return QStringLiteral("左上");
    case WatermarkLayout::TopRight:
        return QStringLiteral("右上");
    case WatermarkLayout::BottomLeft:
        return QStringLiteral("左下");
    case WatermarkLayout::BottomRight:
        return QStringLiteral("右下");
    case WatermarkLayout::Center:
        return QStringLiteral("居中");
    case WatermarkLayout::DiagonalForward:
        return QStringLiteral("正向斜铺");
    case WatermarkLayout::DiagonalReverse:
        return QStringLiteral("反向斜铺");
    }
    return {};
}
bool watermarkLayoutFromKey(const QString& key, WatermarkLayout& layout)
{
    for (auto candidate : layouts)
        if (watermarkLayoutKey(candidate) == key)
        {
            layout = candidate;
            return true;
        }
    return false;
}
const WatermarkAsset* findWatermarkAsset(const WatermarkPreferences& preferences, const QString& id)
{
    for (const auto& asset : preferences.assets)
        if (asset.id == id)
            return &asset;
    return nullptr;
}
const WatermarkPreset* findWatermarkPreset(const WatermarkPreferences& preferences,
                                           const QString& id)
{
    for (const auto& preset : preferences.presets)
        if (preset.id == id)
            return &preset;
    return nullptr;
}
const WatermarkPreset* defaultWatermarkPreset(const WatermarkPreferences& preferences)
{
    return findWatermarkPreset(preferences, preferences.defaultPresetId);
}
QString watermarkPreferencesError(const WatermarkPreferences& preferences)
{
    QSet<QString> assetIds;
    QSet<QString> assetNames;
    for (const auto& asset : preferences.assets)
    {
        const QString error = assetError(asset);
        if (!error.isEmpty())
            return error;
        if (assetIds.contains(asset.id))
            return QStringLiteral("水印资产 ID 重复：%1").arg(asset.id);
        if (assetNames.contains(asset.name))
            return QStringLiteral("水印资产名称重复：%1").arg(asset.name);
        assetIds.insert(asset.id);
        assetNames.insert(asset.name);
    }
    if (preferences.presets.size() > watermarkMaximumPresets)
        return QStringLiteral("水印预设最多 %1 套。").arg(watermarkMaximumPresets);
    QSet<QString> presetIds;
    QSet<QString> presetNames;
    for (const auto& preset : preferences.presets)
    {
        const QString error = presetError(preset, preferences);
        if (!error.isEmpty())
            return error;
        if (presetIds.contains(preset.id))
            return QStringLiteral("水印预设 ID 重复：%1").arg(preset.id);
        if (presetNames.contains(preset.name))
            return QStringLiteral("水印预设名称重复：%1").arg(preset.name);
        presetIds.insert(preset.id);
        presetNames.insert(preset.name);
    }
    if (preferences.presets.isEmpty())
    {
        if (!preferences.defaultPresetId.isEmpty())
            return QStringLiteral("没有水印预设时不能指定默认预设。");
    }
    else if (!presetIds.contains(preferences.defaultPresetId))
        return QStringLiteral("默认水印预设不存在。");
    return {};
}
QJsonObject watermarkPreferencesToJson(const WatermarkPreferences& preferences)
{
    QJsonArray assets;
    for (const auto& asset : preferences.assets)
    {
        QJsonObject object;
        object.insert(QStringLiteral("id"), asset.id);
        object.insert(QStringLiteral("name"), asset.name);
        object.insert(QStringLiteral("kind"), watermarkKindKey(asset.kind));
        object.insert(QStringLiteral("text"), asset.text);
        object.insert(QStringLiteral("imageFileName"), asset.imageFileName);
        object.insert(QStringLiteral("fontFamily"), asset.fontFamily);
        object.insert(QStringLiteral("fontPixelSize"), asset.fontPixelSize);
        object.insert(QStringLiteral("color"), asset.color.name(QColor::HexArgb));
        object.insert(QStringLiteral("bold"), asset.bold);
        object.insert(QStringLiteral("italic"), asset.italic);
        assets.append(object);
    }
    QJsonArray presets;
    for (const auto& preset : preferences.presets)
    {
        QJsonObject object;
        object.insert(QStringLiteral("id"), preset.id);
        object.insert(QStringLiteral("name"), preset.name);
        object.insert(QStringLiteral("assetId"), preset.assetId);
        object.insert(QStringLiteral("layout"), watermarkLayoutKey(preset.layout));
        object.insert(QStringLiteral("opacity"), preset.opacity);
        object.insert(QStringLiteral("scale"), preset.scale);
        presets.append(object);
    }
    QJsonObject result;
    result.insert(QStringLiteral("version"), 1);
    result.insert(QStringLiteral("assets"), assets);
    result.insert(QStringLiteral("presets"), presets);
    result.insert(QStringLiteral("defaultPresetId"), preferences.defaultPresetId);
    result.insert(QStringLiteral("autoEnabled"), preferences.autoEnabled);
    result.insert(QStringLiteral("retainOriginal"), preferences.retainOriginal);
    return result;
}
bool watermarkPreferencesFromJson(const QJsonObject& json, WatermarkPreferences& preferences,
                                  QString* error)
{
    WatermarkPreferences parsed;
    const auto assets = json.value(QStringLiteral("assets"));
    const auto presets = json.value(QStringLiteral("presets"));
    if (!assets.isArray() || !presets.isArray())
    {
        if (error)
            *error = QStringLiteral("水印设置缺少 assets 或 presets 数组。");
        return false;
    }
    for (const auto& value : assets.toArray())
    {
        if (!value.isObject())
        {
            if (error)
                *error = QStringLiteral("水印资产条目必须是对象。");
            return false;
        }
        const auto object = value.toObject();
        WatermarkAsset asset;
        QString kind;
        if (!readString(object, QStringLiteral("id"), asset.id, error) ||
            !readString(object, QStringLiteral("name"), asset.name, error) ||
            !readString(object, QStringLiteral("kind"), kind, error) ||
            !readString(object, QStringLiteral("text"), asset.text, error) ||
            !readString(object, QStringLiteral("imageFileName"), asset.imageFileName, error) ||
            !readString(object, QStringLiteral("fontFamily"), asset.fontFamily, error))
            return false;
        if (kind == QStringLiteral("text"))
            asset.kind = WatermarkKind::Text;
        else if (kind == QStringLiteral("image"))
            asset.kind = WatermarkKind::Image;
        else
        {
            if (error)
                *error = QStringLiteral("水印资产类型无效：%1").arg(kind);
            return false;
        }
        if (!readInt(object, QStringLiteral("fontPixelSize"), asset.fontPixelSize, error) ||
            !readBool(object, QStringLiteral("bold"), asset.bold, error) ||
            !readBool(object, QStringLiteral("italic"), asset.italic, error))
            return false;
        QString color;
        if (!readString(object, QStringLiteral("color"), color, error))
            return false;
        asset.color = QColor(color);
        parsed.assets.append(asset);
    }
    for (const auto& value : presets.toArray())
    {
        if (!value.isObject())
        {
            if (error)
                *error = QStringLiteral("水印预设条目必须是对象。");
            return false;
        }
        const auto object = value.toObject();
        WatermarkPreset preset;
        QString layout;
        if (!readString(object, QStringLiteral("id"), preset.id, error) ||
            !readString(object, QStringLiteral("name"), preset.name, error) ||
            !readString(object, QStringLiteral("assetId"), preset.assetId, error) ||
            !readString(object, QStringLiteral("layout"), layout, error) ||
            !readInt(object, QStringLiteral("opacity"), preset.opacity, error) ||
            !readInt(object, QStringLiteral("scale"), preset.scale, error))
            return false;
        if (!watermarkLayoutFromKey(layout, preset.layout))
        {
            if (error)
                *error = QStringLiteral("水印排布无效：%1").arg(layout);
            return false;
        }
        parsed.presets.append(preset);
    }
    if (!readString(json, QStringLiteral("defaultPresetId"), parsed.defaultPresetId, error))
        return false;
    const auto autoEnabled = json.value(QStringLiteral("autoEnabled"));
    if (autoEnabled.isBool())
        parsed.autoEnabled = autoEnabled.toBool();
    else if (!autoEnabled.isUndefined() && !autoEnabled.isNull())
    {
        if (error)
            *error = QStringLiteral("autoEnabled 必须是布尔值。");
        return false;
    }
    const auto retainOriginal = json.value(QStringLiteral("retainOriginal"));
    if (retainOriginal.isBool())
        parsed.retainOriginal = retainOriginal.toBool();
    else if (!retainOriginal.isUndefined() && !retainOriginal.isNull())
    {
        if (error)
            *error = QStringLiteral("retainOriginal 必须是布尔值。");
        return false;
    }
    const QString validation = watermarkPreferencesError(parsed);
    if (!validation.isEmpty())
    {
        if (error)
            *error = validation;
        return false;
    }
    preferences = std::move(parsed);
    return true;
}
}
