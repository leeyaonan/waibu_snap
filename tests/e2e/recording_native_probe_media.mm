#include "recording_native_probe_media.h"
#import <AVFoundation/AVFoundation.h>
#include <memory>

RecordingProbeAudioInfo recordingProbeAudioInfo(const QString& path)
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
            return {};
        RecordingProbeAudioInfo info;
        info.valid = result->tracks != nil;
        info.tracks = int(result->tracks.count);
        if (info.tracks > 0)
        {
            AVAssetTrack* track = result->tracks.firstObject;
            info.durationMs = qRound64(CMTimeGetSeconds(track.timeRange.duration) * 1000);
            for (id value in track.formatDescriptions)
            {
                CMAudioFormatDescriptionRef description =
                    (__bridge CMAudioFormatDescriptionRef)value;
                const auto* asbd = CMAudioFormatDescriptionGetStreamBasicDescription(description);
                if (asbd)
                {
                    info.aac = asbd->mFormatID == kAudioFormatMPEG4AAC;
                    info.sampleRate = asbd->mSampleRate;
                    info.channels = int(asbd->mChannelsPerFrame);
                    break;
                }
            }
        }
        return info;
    }
}
