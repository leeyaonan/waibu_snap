#pragma once
#include <QColor>
#include <QJsonObject>
#include <QPointF>
#include <QSizeF>
#include <QString>
#include <QVector>
#include <array>
namespace waibusnap
{
enum class WatermarkKind
{
    Text,
    Image
};
enum class WatermarkLayout
{
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
    Center,
    DiagonalForward,
    DiagonalReverse
};
inline constexpr int watermarkMaximumPresets = 10;
inline constexpr int watermarkMinimumOpacity = 1;
inline constexpr int watermarkMaximumOpacity = 100;
inline constexpr int watermarkMinimumScale = 10;
inline constexpr int watermarkMaximumScale = 400;
inline constexpr int watermarkMinimumFontPixelSize = 9;
inline constexpr int watermarkMaximumFontPixelSize = 96;
struct WatermarkAsset
{
    QString id;
    QString name;
    WatermarkKind kind = WatermarkKind::Text;
    QString text;
    QString imageFileName;
    QString fontFamily;
    int fontPixelSize = 32;
    QColor color = Qt::white;
    bool bold = false;
    bool italic = false;
};
struct WatermarkPreset
{
    QString id;
    QString name;
    QString assetId;
    WatermarkLayout layout = WatermarkLayout::BottomRight;
    int opacity = 40;
    int scale = 100;
};
struct WatermarkPreferences
{
    QVector<WatermarkAsset> assets;
    QVector<WatermarkPreset> presets;
    QString defaultPresetId;
    bool autoEnabled = false;
    bool retainOriginal = false;
};
// 单张截图上的水印状态；中心使用输出图的归一化坐标，0..1。
struct WatermarkInstance
{
    QString presetId;
    WatermarkLayout layout = WatermarkLayout::BottomRight;
    bool visible = true;
    int opacity = 40;
    int scale = 100;
    QPointF normalizedCenter = {0.9, 0.9};
    qreal rotation = 0;
};
const std::array<WatermarkLayout, 7>& watermarkLayouts();
QString watermarkKindKey(WatermarkKind kind);
QString watermarkLayoutKey(WatermarkLayout layout);
QString watermarkLayoutName(WatermarkLayout layout);
bool watermarkLayoutFromKey(const QString& key, WatermarkLayout& layout);
const WatermarkAsset* findWatermarkAsset(const WatermarkPreferences& preferences,
                                         const QString& id);
const WatermarkPreset* findWatermarkPreset(const WatermarkPreferences& preferences,
                                           const QString& id);
const WatermarkPreset* defaultWatermarkPreset(const WatermarkPreferences& preferences);
// 空字符串表示通过；错误信息可直接展示给用户。
QString watermarkPreferencesError(const WatermarkPreferences& preferences);
QJsonObject watermarkPreferencesToJson(const WatermarkPreferences& preferences);
bool watermarkPreferencesFromJson(const QJsonObject& json, WatermarkPreferences& preferences,
                                  QString* error = nullptr);
}
