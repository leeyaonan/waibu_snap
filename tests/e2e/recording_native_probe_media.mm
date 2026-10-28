#include "recording_native_probe_media.h"
#import <AVFoundation/AVFoundation.h>
#include <memory>

bool recordingProbeHasAudioTrack(const QString& path)
{
    @autoreleasepool
    {
        AVURLAsset* asset = [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:path.toNSString()]
                                                options:nil];
        struct TrackLoadResult
        {
            NSArray<AVAssetTrack*>* __strong tracks = nil;
        };
        const auto result = std::make_shared<TrackLoadResult>();
        dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
        [asset loadTracksWithMediaType:AVMediaTypeAudio
                     completionHandler:^(NSArray<AVAssetTrack*>* loaded, NSError* error) {
                       (void)error;
                       result->tracks = loaded;
                       dispatch_semaphore_signal(semaphore);
                     }];
        if (dispatch_semaphore_wait(semaphore,
                                    dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) != 0)
            return false;
        return result->tracks.count > 0;
    }
}
