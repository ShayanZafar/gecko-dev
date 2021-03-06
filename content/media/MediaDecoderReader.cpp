/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "MediaDecoderReader.h"
#include "AbstractMediaDecoder.h"
#include "VideoUtils.h"
#include "ImageContainer.h"

#include "mozilla/mozalloc.h"
#include <stdint.h>
#include <algorithm>

namespace mozilla {

// Un-comment to enable logging of seek bisections.
//#define SEEK_LOGGING

#ifdef PR_LOGGING
extern PRLogModuleInfo* gMediaDecoderLog;
#define DECODER_LOG(type, msg) PR_LOG(gMediaDecoderLog, type, msg)
#ifdef SEEK_LOGGING
#define SEEK_LOG(type, msg) PR_LOG(gMediaDecoderLog, type, msg)
#else
#define SEEK_LOG(type, msg)
#endif
#else
#define DECODER_LOG(type, msg)
#define SEEK_LOG(type, msg)
#endif

class VideoQueueMemoryFunctor : public nsDequeFunctor {
public:
  VideoQueueMemoryFunctor() : mSize(0) {}

  MOZ_DEFINE_MALLOC_SIZE_OF(MallocSizeOf);

  virtual void* operator()(void* aObject) {
    const VideoData* v = static_cast<const VideoData*>(aObject);
    mSize += v->SizeOfIncludingThis(MallocSizeOf);
    return nullptr;
  }

  size_t mSize;
};


class AudioQueueMemoryFunctor : public nsDequeFunctor {
public:
  AudioQueueMemoryFunctor() : mSize(0) {}

  MOZ_DEFINE_MALLOC_SIZE_OF(MallocSizeOf);

  virtual void* operator()(void* aObject) {
    const AudioData* audioData = static_cast<const AudioData*>(aObject);
    mSize += audioData->SizeOfIncludingThis(MallocSizeOf);
    return nullptr;
  }

  size_t mSize;
};

MediaDecoderReader::MediaDecoderReader(AbstractMediaDecoder* aDecoder)
  : mAudioCompactor(mAudioQueue),
    mDecoder(aDecoder),
    mIgnoreAudioOutputFormat(false)
{
  MOZ_COUNT_CTOR(MediaDecoderReader);
}

MediaDecoderReader::~MediaDecoderReader()
{
  ResetDecode();
  MOZ_COUNT_DTOR(MediaDecoderReader);
}

size_t MediaDecoderReader::SizeOfVideoQueueInBytes() const
{
  VideoQueueMemoryFunctor functor;
  mVideoQueue.LockedForEach(functor);
  return functor.mSize;
}

size_t MediaDecoderReader::SizeOfAudioQueueInBytes() const
{
  AudioQueueMemoryFunctor functor;
  mAudioQueue.LockedForEach(functor);
  return functor.mSize;
}

nsresult MediaDecoderReader::ResetDecode()
{
  nsresult res = NS_OK;

  VideoQueue().Reset();
  AudioQueue().Reset();

  return res;
}

VideoData* MediaDecoderReader::DecodeToFirstVideoData()
{
  bool eof = false;
  while (!eof && VideoQueue().GetSize() == 0) {
    {
      ReentrantMonitorAutoEnter decoderMon(mDecoder->GetReentrantMonitor());
      if (mDecoder->IsShutdown()) {
        return nullptr;
      }
    }
    bool keyframeSkip = false;
    eof = !DecodeVideoFrame(keyframeSkip, 0);
  }
  if (eof) {
    VideoQueue().Finish();
  }
  VideoData* d = nullptr;
  return (d = VideoQueue().PeekFront()) ? d : nullptr;
}

AudioData* MediaDecoderReader::DecodeToFirstAudioData()
{
  bool eof = false;
  while (!eof && AudioQueue().GetSize() == 0) {
    {
      ReentrantMonitorAutoEnter decoderMon(mDecoder->GetReentrantMonitor());
      if (mDecoder->IsShutdown()) {
        return nullptr;
      }
    }
    eof = !DecodeAudioData();
  }
  if (eof) {
    AudioQueue().Finish();
  }
  AudioData* d = nullptr;
  return (d = AudioQueue().PeekFront()) ? d : nullptr;
}

VideoData* MediaDecoderReader::FindStartTime(int64_t& aOutStartTime)
{
  NS_ASSERTION(mDecoder->OnStateMachineThread() || mDecoder->OnDecodeThread(),
               "Should be on state machine or decode thread.");

  // Extract the start times of the bitstreams in order to calculate
  // the duration.
  int64_t videoStartTime = INT64_MAX;
  int64_t audioStartTime = INT64_MAX;
  VideoData* videoData = nullptr;

  if (HasVideo()) {
    videoData = DecodeToFirstVideoData();
    if (videoData) {
      videoStartTime = videoData->mTime;
      DECODER_LOG(PR_LOG_DEBUG, ("MediaDecoderReader::FindStartTime() video=%lld", videoStartTime));
    }
  }
  if (HasAudio()) {
    AudioData* audioData = DecodeToFirstAudioData();
    if (audioData) {
      audioStartTime = audioData->mTime;
      DECODER_LOG(PR_LOG_DEBUG, ("MediaDecoderReader::FindStartTime() audio=%lld", audioStartTime));
    }
  }

  int64_t startTime = std::min(videoStartTime, audioStartTime);
  if (startTime != INT64_MAX) {
    aOutStartTime = startTime;
  }

  return videoData;
}

nsresult MediaDecoderReader::DecodeToTarget(int64_t aTarget)
{
  DECODER_LOG(PR_LOG_DEBUG, ("MediaDecoderReader::DecodeToTarget(%lld) Begin", aTarget));

  // Decode forward to the target frame. Start with video, if we have it.
  if (HasVideo()) {
    // Note: when decoding hits the end of stream we must keep the last frame
    // in the video queue so that we'll have something to display after the
    // seek completes. This makes our logic a bit messy.
    bool eof = false;
    nsAutoPtr<VideoData> video;
    while (HasVideo() && !eof) {
      while (VideoQueue().GetSize() == 0 && !eof) {
        bool skip = false;
        eof = !DecodeVideoFrame(skip, 0);
        {
          ReentrantMonitorAutoEnter decoderMon(mDecoder->GetReentrantMonitor());
          if (mDecoder->IsShutdown()) {
            return NS_ERROR_FAILURE;
          }
        }
      }
      if (eof) {
        // Hit end of file, we want to display the last frame of the video.
        if (video) {
          DECODER_LOG(PR_LOG_DEBUG,
            ("MediaDecoderReader::DecodeToTarget(%lld) repushing video frame [%lld, %lld] at EOF",
            aTarget, video->mTime, video->GetEndTime()));
          VideoQueue().PushFront(video.forget());
        }
        VideoQueue().Finish();
        break;
      }
      video = VideoQueue().PeekFront();
      // If the frame end time is less than the seek target, we won't want
      // to display this frame after the seek, so discard it.
      if (video && video->GetEndTime() <= aTarget) {
        DECODER_LOG(PR_LOG_DEBUG,
                    ("MediaDecoderReader::DecodeToTarget(%lld) pop video frame [%lld, %lld]",
                     aTarget, video->mTime, video->GetEndTime()));
        VideoQueue().PopFront();
      } else {
        // Found a frame after or encompasing the seek target.
        if (aTarget >= video->mTime && video->GetEndTime() >= aTarget) {
          // The seek target lies inside this frame's time slice. Adjust the frame's
          // start time to match the seek target. We do this by replacing the
          // first frame with a shallow copy which has the new timestamp.
          VideoQueue().PopFront();
          VideoData* temp = VideoData::ShallowCopyUpdateTimestamp(video, aTarget);
          video = temp;
          VideoQueue().PushFront(video);
        }
        DECODER_LOG(PR_LOG_DEBUG,
                    ("MediaDecoderReader::DecodeToTarget(%lld) found target video frame [%lld,%lld]",
                     aTarget, video->mTime, video->GetEndTime()));

        video.forget();
        break;
      }
    }
    {
      ReentrantMonitorAutoEnter decoderMon(mDecoder->GetReentrantMonitor());
      if (mDecoder->IsShutdown()) {
        return NS_ERROR_FAILURE;
      }
    }
#ifdef PR_LOGGING
    const VideoData* front =  VideoQueue().PeekFront();
    DECODER_LOG(PR_LOG_DEBUG, ("First video frame after decode is %lld",
                front ? front->mTime : -1));
#endif
  }

  if (HasAudio()) {
    // Decode audio forward to the seek target.
    bool eof = false;
    while (HasAudio() && !eof) {
      while (!eof && AudioQueue().GetSize() == 0) {
        eof = !DecodeAudioData();
        {
          ReentrantMonitorAutoEnter decoderMon(mDecoder->GetReentrantMonitor());
          if (mDecoder->IsShutdown()) {
            return NS_ERROR_FAILURE;
          }
        }
      }
      const AudioData* audio = AudioQueue().PeekFront();
      if (!audio || eof) {
        AudioQueue().Finish();
        break;
      }
      CheckedInt64 startFrame = UsecsToFrames(audio->mTime, mInfo.mAudio.mRate);
      CheckedInt64 targetFrame = UsecsToFrames(aTarget, mInfo.mAudio.mRate);
      if (!startFrame.isValid() || !targetFrame.isValid()) {
        return NS_ERROR_FAILURE;
      }
      if (startFrame.value() + audio->mFrames <= targetFrame.value()) {
        // Our seek target lies after the frames in this AudioData. Pop it
        // off the queue, and keep decoding forwards.
        delete AudioQueue().PopFront();
        audio = nullptr;
        continue;
      }
      if (startFrame.value() > targetFrame.value()) {
        // The seek target doesn't lie in the audio block just after the last
        // audio frames we've seen which were before the seek target. This
        // could have been the first audio data we've seen after seek, i.e. the
        // seek terminated after the seek target in the audio stream. Just
        // abort the audio decode-to-target, the state machine will play
        // silence to cover the gap. Typically this happens in poorly muxed
        // files.
        NS_WARNING("Audio not synced after seek, maybe a poorly muxed file?");
        break;
      }

      // The seek target lies somewhere in this AudioData's frames, strip off
      // any frames which lie before the seek target, so we'll begin playback
      // exactly at the seek target.
      NS_ASSERTION(targetFrame.value() >= startFrame.value(),
                   "Target must at or be after data start.");
      NS_ASSERTION(targetFrame.value() < startFrame.value() + audio->mFrames,
                   "Data must end after target.");

      int64_t framesToPrune = targetFrame.value() - startFrame.value();
      if (framesToPrune > audio->mFrames) {
        // We've messed up somehow. Don't try to trim frames, the |frames|
        // variable below will overflow.
        NS_WARNING("Can't prune more frames that we have!");
        break;
      }
      uint32_t frames = audio->mFrames - static_cast<uint32_t>(framesToPrune);
      uint32_t channels = audio->mChannels;
      nsAutoArrayPtr<AudioDataValue> audioData(new AudioDataValue[frames * channels]);
      memcpy(audioData.get(),
             audio->mAudioData.get() + (framesToPrune * channels),
             frames * channels * sizeof(AudioDataValue));
      CheckedInt64 duration = FramesToUsecs(frames, mInfo.mAudio.mRate);
      if (!duration.isValid()) {
        return NS_ERROR_FAILURE;
      }
      nsAutoPtr<AudioData> data(new AudioData(audio->mOffset,
                                              aTarget,
                                              duration.value(),
                                              frames,
                                              audioData.forget(),
                                              channels));
      delete AudioQueue().PopFront();
      AudioQueue().PushFront(data.forget());
      break;
    }
  }

#ifdef PR_LOGGING
  const VideoData* v = VideoQueue().PeekFront();
  const AudioData* a = AudioQueue().PeekFront();
  DECODER_LOG(PR_LOG_DEBUG,
              ("MediaDecoderReader::DecodeToTarget(%lld) finished v=%lld a=%lld",
              aTarget, v ? v->mTime : -1, a ? a->mTime : -1));
#endif

  return NS_OK;
}

nsresult
MediaDecoderReader::GetBuffered(mozilla::dom::TimeRanges* aBuffered,
                                int64_t aStartTime)
{
  MediaResource* stream = mDecoder->GetResource();
  int64_t durationUs = 0;
  {
    ReentrantMonitorAutoEnter mon(mDecoder->GetReentrantMonitor());
    durationUs = mDecoder->GetMediaDuration();
  }
  GetEstimatedBufferedTimeRanges(stream, durationUs, aBuffered);
  return NS_OK;
}

} // namespace mozilla
