#include "ocr_engine_languages.h"
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Media.Ocr.h>
namespace
{
struct Mta
{
    Mta() { winrt::init_apartment(winrt::apartment_type::multi_threaded); }
    ~Mta()
    {
        winrt::clear_factory_cache();
        winrt::uninit_apartment();
    }
};
}
QStringList ocrEngineLanguages()
{
    QStringList languages;
    try
    {
        Mta apartment;
        const auto available =
            winrt::Windows::Media::Ocr::OcrEngine::AvailableRecognizerLanguages();
        if (!available)
            return languages;
        for (const auto& language : available)
        {
            const auto tag = language.LanguageTag();
            languages.append(QString::fromWCharArray(tag.c_str(), int(tag.size())));
        }
    }
    catch (const winrt::hresult_error&)
    {
        // 无系统引擎的机器仍由样本用例明确断言生产错误路径。
    }
    return languages;
}
bool ocrRequiresChinese() { return false; }
bool ocrEngineAvailable()
{
    try
    {
        Mta apartment;
        auto engine = winrt::Windows::Media::Ocr::OcrEngine::TryCreateFromUserProfileLanguages();
        if (!engine)
            engine = winrt::Windows::Media::Ocr::OcrEngine::TryCreateFromLanguage(
                winrt::Windows::Globalization::Language(L"zh-Hans"));
        return bool(engine);
    }
    catch (const winrt::hresult_error&)
    {
        return false;
    }
}
