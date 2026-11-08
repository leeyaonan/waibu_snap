#include "interfaces/qr_recognizer.h"
#include "core/quirc_qr_recognizer.h"
namespace waibusnap
{
std::unique_ptr<QrRecognizer> createQrRecognizer() { return createQuircQrRecognizer(); }
} // namespace waibusnap
