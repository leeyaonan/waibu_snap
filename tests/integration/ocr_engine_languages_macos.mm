#include "ocr_engine_languages.h"
#import <Vision/Vision.h>
QStringList ocrEngineLanguages()
{
    @autoreleasepool
    {
        VNRecognizeTextRequest* request = [[VNRecognizeTextRequest alloc] init];
        request.recognitionLevel = VNRequestTextRecognitionLevelAccurate;
        NSError* error = nil;
        QStringList languages;
        for (NSString* language in [request supportedRecognitionLanguagesAndReturnError:&error])
            languages.append(QString::fromNSString(language));
        return languages;
    }
}
bool ocrRequiresChinese() { return true; }

bool ocrEngineAvailable() { return true; }
