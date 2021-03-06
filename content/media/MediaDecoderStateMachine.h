/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
/*
Each video element for a media file has two threads:

  1) The Audio thread writes the decoded audio data to the audio
     hardware. This is done in a separate thread to ensure that the
     audio hardware gets a constant stream of data without
     interruption due to decoding or display. At some point
     AudioStream will be refactored to have a callback interface
     where it asks for data and an extra thread will no longer be
     needed.

  2) The decode thread. This thread reads from the media stream and
     decodes the Theora and Vorbis data. It places the decoded data into
     queues for the other threads to pull from.

All file reads, seeks, and all decoding must occur on the decode thread.
Synchronisation of state between the thread is done via a monitor owned
by MediaDecoder.

The lifetime of the decode and audio threads is controlled by the state
machine when it runs on the shared state machine thread. When playback
needs to occur they are created and events dispatched to them to run
them. These events exit when decoding/audio playback is completed or
no longer required.

A/V synchronisation is handled by the state machine. It examines the audio
playback time and compares this to the next frame in the queue of video
frames. If it is time to play the video frame it is then displayed, otherwise
it schedules the state machine to run again at the time of the next frame.

Frame skipping is done in the following ways:

  1) The state machine will skip all frames in the video queue whose
     display time is less than the current audio time. This ensures
     the correct frame for the current time is always displayed.

  2) The decode thread will stop decoding interframes and read to the
     next keyframe if it determines that decoding the remaining
     interframes will cause playback issues. It detects this by:
       a) If the amount of audio data in the audio queue drops
          below a threshold whereby audio may start to skip.
       b) If the video queue drops below a threshold where it
          will be decoding video data that won't be displayed due
          to the decode thread dropping the frame immediately.

When hardware accelerated graphics is not available, YCbCr conversion
is done on the decode thread when video frames are decoded.

The decode thread pushes decoded audio and videos frames into two
separate queues - one for audio and one for video. These are kept
separate to make it easy to constantly feed audio data to the audio
hardware while allowing frame skipping of video data. These queues are
threadsafe, and neither the decode, audio, or state machine should
be able to monopolize them, and cause starvation of the other threads.

Both queues are bounded by a maximum size. When this size is reached
the decode thread will no longer decode video or audio depending on the
queue that has reached the threshold. If both queues are full, the decode
thread will wait on the decoder monitor.

When the decode queues are full (they've reaced their maximum size) and
the decoder is not in PLAYING play state, the state machine may opt
to shut down the decode thread in order to conserve resources.

During playback the audio thread will be idle (via a Wait() on the
monitor) if the audio queue is empty. Otherwise it constantly pops
audio data off the queue and plays it with a blocking write to the audio
hardware (via AudioStream).

*/
#if !defined(MediaDecoderStateMachine_h__)
#define MediaDecoderStateMachine_h__

#include "mozilla/Attributes.h"
#include "nsThreadUtils.h"
#include "MediaDecoder.h"
#include "mozilla/ReentrantMonitor.h"
#include "MediaDecoderReader.h"
#include "MediaDecoderOwner.h"
#include "MediaMetadataManager.h"

class nsITimer;

namespace mozilla {

class AudioSegment;
class VideoSegment;
class MediaTaskQueue;
class SharedThreadPool;

// GetCurrentTime is defined in winbase.h as zero argument macro forwarding to
// GetTickCount() and conflicts with MediaDecoderStateMachine::GetCurrentTime
// implementation.
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

/*
  The state machine class. This manages the decoding and seeking in the
  MediaDecoderReader on the decode thread, and A/V sync on the shared
  state machine thread, and controls the audio "push" thread.

  All internal state is synchronised via the decoder monitor. State changes
  are either propagated by NotifyAll on the monitor (typically when state
  changes need to be propagated to non-state machine threads) or by scheduling
  the state machine to run another cycle on the shared state machine thread.

  See MediaDecoder.h for more details.
*/
class MediaDecoderStateMachine
{
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(MediaDecoderStateMachine)
public:
  typedef MediaDecoder::DecodedStreamData DecodedStreamData;
  MediaDecoderStateMachine(MediaDecoder* aDecoder,
                               MediaDecoderReader* aReader,
                               bool aRealTime = false);

  nsresult Init(MediaDecoderStateMachine* aCloneDonor);

  // Enumeration for the valid decoding states
  enum State {
    DECODER_STATE_DECODING_METADATA,
    DECODER_STATE_WAIT_FOR_RESOURCES,
    DECODER_STATE_DORMANT,
    DECODER_STATE_DECODING,
    DECODER_STATE_SEEKING,
    DECODER_STATE_BUFFERING,
    DECODER_STATE_COMPLETED,
    DECODER_STATE_SHUTDOWN
  };

  State GetState() {
    AssertCurrentThreadInMonitor();
    return mState;
  }

  // Set the audio volume. The decoder monitor must be obtained before
  // calling this.
  void SetVolume(double aVolume);
  void SetAudioCaptured(bool aCapture);

  // Check if the decoder needs to become dormant state.
  bool IsDormantNeeded();
  // Set/Unset dormant state.
  void SetDormant(bool aDormant);
  void Shutdown();

  // Called from the main thread to get the duration. The decoder monitor
  // must be obtained before calling this. It is in units of microseconds.
  int64_t GetDuration();

  // Called from the main thread to set the duration of the media resource
  // if it is able to be obtained via HTTP headers. Called from the
  // state machine thread to set the duration if it is obtained from the
  // media metadata. The decoder monitor must be obtained before calling this.
  // aDuration is in microseconds.
  void SetDuration(int64_t aDuration);

  // Called while decoding metadata to set the end time of the media
  // resource. The decoder monitor must be obtained before calling this.
  // aEndTime is in microseconds.
  void SetMediaEndTime(int64_t aEndTime);

  // Called from main thread to update the duration with an estimated value.
  // The duration is only changed if its significantly different than the
  // the current duration, as the incoming duration is an estimate and so
  // often is unstable as more data is read and the estimate is updated.
  // Can result in a durationchangeevent. aDuration is in microseconds.
  void UpdateEstimatedDuration(int64_t aDuration);

  // Functions used by assertions to ensure we're calling things
  // on the appropriate threads.
  bool OnDecodeThread() const;
  bool OnStateMachineThread() const;
  bool OnAudioThread() const {
    return IsCurrentThread(mAudioThread);
  }

  MediaDecoderOwner::NextFrameStatus GetNextFrameStatus();

  // Cause state transitions. These methods obtain the decoder monitor
  // to synchronise the change of state, and to notify other threads
  // that the state has changed.
  void Play();

  // Seeks to the decoder to aTarget asynchronously.
  void Seek(const SeekTarget& aTarget);

  // Returns the current playback position in seconds.
  // Called from the main thread to get the current frame time. The decoder
  // monitor must be obtained before calling this.
  double GetCurrentTime() const;

  // Clear the flag indicating that a playback position change event
  // is currently queued. This is called from the main thread and must
  // be called with the decode monitor held.
  void ClearPositionChangeFlag();

  // Called from the main thread or the decoder thread to set whether the media
  // resource can seek into unbuffered ranges. The decoder monitor must be
  // obtained before calling this.
  void SetTransportSeekable(bool aSeekable);

  // Called from the main thread or the decoder thread to set whether the media
  // can seek to random location. This is not true for chained ogg and WebM
  // media without index. The decoder monitor must be obtained before calling
  // this.
  void SetMediaSeekable(bool aSeekable);

  // Update the playback position. This can result in a timeupdate event
  // and an invalidate of the frame being dispatched asynchronously if
  // there is no such event currently queued.
  // Only called on the decoder thread. Must be called with
  // the decode monitor held.
  void UpdatePlaybackPosition(int64_t aTime);

  // Causes the state machine to switch to buffering state, and to
  // immediately stop playback and buffer downloaded data. Must be called
  // with the decode monitor held. Called on the state machine thread and
  // the main thread.
  void StartBuffering();

  // This is called on the state machine thread and audio thread.
  // The decoder monitor must be obtained before calling this.
  bool HasAudio() const {
    AssertCurrentThreadInMonitor();
    return mInfo.HasAudio();
  }

  // This is called on the state machine thread and audio thread.
  // The decoder monitor must be obtained before calling this.
  bool HasVideo() const {
    AssertCurrentThreadInMonitor();
    return mInfo.HasVideo();
  }

  // Should be called by main thread.
  bool HaveNextFrameData();

  // Must be called with the decode monitor held.
  bool IsBuffering() const {
    AssertCurrentThreadInMonitor();

    return mState == DECODER_STATE_BUFFERING;
  }

  // Must be called with the decode monitor held.
  bool IsSeeking() const {
    AssertCurrentThreadInMonitor();

    return mState == DECODER_STATE_SEEKING;
  }

  nsresult GetBuffered(dom::TimeRanges* aBuffered);

  void SetPlaybackRate(double aPlaybackRate);
  void SetPreservesPitch(bool aPreservesPitch);

  size_t SizeOfVideoQueue() {
    if (mReader) {
      return mReader->SizeOfVideoQueueInBytes();
    }
    return 0;
  }

  size_t SizeOfAudioQueue() {
    if (mReader) {
      return mReader->SizeOfAudioQueueInBytes();
    }
    return 0;
  }

  void NotifyDataArrived(const char* aBuffer, uint32_t aLength, int64_t aOffset);

  int64_t GetEndMediaTime() const {
    AssertCurrentThreadInMonitor();
    return mEndTime;
  }

  bool IsTransportSeekable() {
    AssertCurrentThreadInMonitor();
    return mTransportSeekable;
  }

  bool IsMediaSeekable() {
    AssertCurrentThreadInMonitor();
    return mMediaSeekable;
  }

  // Returns the shared state machine thread.
  nsIEventTarget* GetStateMachineThread();

  // Calls ScheduleStateMachine() after taking the decoder lock. Also
  // notifies the decoder thread in case it's waiting on the decoder lock.
  void ScheduleStateMachineWithLockAndWakeDecoder();

  // Schedules the shared state machine thread to run the state machine
  // in aUsecs microseconds from now, if it's not already scheduled to run
  // earlier, in which case the request is discarded.
  nsresult ScheduleStateMachine(int64_t aUsecs = 0);

  // Timer function to implement ScheduleStateMachine(aUsecs).
  nsresult TimeoutExpired(int aGeneration);

  // Set the media fragment end time. aEndTime is in microseconds.
  void SetFragmentEndTime(int64_t aEndTime);

  // Drop reference to decoder.  Only called during shutdown dance.
  void ReleaseDecoder() {
    MOZ_ASSERT(mReader);
    if (mReader) {
      mReader->ReleaseDecoder();
    }
    mDecoder = nullptr;
  }

  // If we're playing into a MediaStream, record the current point in the
  // MediaStream and the current point in our media resource so later we can
  // convert MediaStream playback positions to media resource positions. Best to
  // call this while we're not playing (while the MediaStream is blocked). Can
  // be called on any thread with the decoder monitor held.
  void SetSyncPointForMediaStream();
  int64_t GetCurrentTimeViaMediaStreamSync();

  // Copy queued audio/video data in the reader to any output MediaStreams that
  // need it.
  void SendStreamData();
  void FinishStreamData();
  bool HaveEnoughDecodedAudio(int64_t aAmpleAudioUSecs);
  bool HaveEnoughDecodedVideo();

  // Returns true if the state machine has shutdown or is in the process of
  // shutting down. The decoder monitor must be held while calling this.
  bool IsShutdown();

  void QueueMetadata(int64_t aPublishTime, int aChannels, int aRate, bool aHasAudio, bool aHasVideo, MetadataTags* aTags);

  // Returns true if we're currently playing. The decoder monitor must
  // be held.
  bool IsPlaying();

  // Called when the reader may have acquired the hardware resources required
  // to begin decoding. The state machine may move into DECODING_METADATA if
  // appropriate. The decoder monitor must be held while calling this.
  void NotifyWaitingForResourcesStatusChanged();

  // Notifies the state machine that should minimize the number of samples
  // decoded we preroll, until playback starts. The first time playback starts
  // the state machine is free to return to prerolling normally. Note
  // "prerolling" in this context refers to when we decode and buffer decoded
  // samples in advance of when they're needed for playback.
  void SetMinimizePrerollUntilPlaybackStarts();

protected:
  virtual ~MediaDecoderStateMachine();

  void AssertCurrentThreadInMonitor() const { mDecoder->GetReentrantMonitor().AssertCurrentThreadIn(); }

  class WakeDecoderRunnable : public nsRunnable {
  public:
    WakeDecoderRunnable(MediaDecoderStateMachine* aSM)
      : mMutex("WakeDecoderRunnable"), mStateMachine(aSM) {}
    NS_IMETHOD Run() MOZ_OVERRIDE
    {
      nsRefPtr<MediaDecoderStateMachine> stateMachine;
      {
        // Don't let Run() (called by media stream graph thread) race with
        // Revoke() (called by decoder state machine thread)
        MutexAutoLock lock(mMutex);
        if (!mStateMachine)
          return NS_OK;
        stateMachine = mStateMachine;
      }
      stateMachine->ScheduleStateMachineWithLockAndWakeDecoder();
      return NS_OK;
    }
    void Revoke()
    {
      MutexAutoLock lock(mMutex);
      mStateMachine = nullptr;
    }

    Mutex mMutex;
    // Protected by mMutex.
    // We don't use an owning pointer here, because keeping mStateMachine alive
    // would mean in some cases we'd have to destroy mStateMachine from this
    // object, which would be problematic since MediaDecoderStateMachine can
    // only be destroyed on the main thread whereas this object can be destroyed
    // on the media stream graph thread.
    MediaDecoderStateMachine* mStateMachine;
  };
  WakeDecoderRunnable* GetWakeDecoderRunnable();

  MediaQueue<AudioData>& AudioQueue() { return mReader->AudioQueue(); }
  MediaQueue<VideoData>& VideoQueue() { return mReader->VideoQueue(); }

  // True if our buffers of decoded audio are not full, and we should
  // decode more.
  bool NeedToDecodeAudio();

  // Decodes some audio. This should be run on the decode task queue.
  void DecodeAudio();

  // True if our buffers of decoded video are not full, and we should
  // decode more.
  bool NeedToDecodeVideo();

  // Decodes some video. This should be run on the decode task queue.
  void DecodeVideo();

  // Returns true if we've got less than aAudioUsecs microseconds of decoded
  // and playable data. The decoder monitor must be held.
  bool HasLowDecodedData(int64_t aAudioUsecs);

  // Returns true if we're running low on data which is not yet decoded.
  // The decoder monitor must be held.
  bool HasLowUndecodedData();

  // Returns true if we have less than aUsecs of undecoded data available.
  bool HasLowUndecodedData(double aUsecs);

  // Returns the number of unplayed usecs of audio we've got decoded and/or
  // pushed to the hardware waiting to play. This is how much audio we can
  // play without having to run the audio decoder. The decoder monitor
  // must be held.
  int64_t AudioDecodedUsecs();

  // Returns true when there's decoded audio waiting to play.
  // The decoder monitor must be held.
  bool HasFutureAudio();

  // Returns true if we recently exited "quick buffering" mode.
  bool JustExitedQuickBuffering();

  // Waits on the decoder ReentrantMonitor for aUsecs microseconds. If the decoder
  // monitor is awoken by a Notify() call, we'll continue waiting, unless
  // we've moved into shutdown state. This enables us to ensure that we
  // wait for a specified time, and that the myriad of Notify()s we do on
  // the decoder monitor don't cause the audio thread to be starved. aUsecs
  // values of less than 1 millisecond are rounded up to 1 millisecond
  // (see bug 651023). The decoder monitor must be held. Called only on the
  // audio thread.
  void Wait(int64_t aUsecs);

  // Dispatches an asynchronous event to update the media element's ready state.
  void UpdateReadyState();

  // Resets playback timing data. Called when we seek, on the decode thread.
  void ResetPlayback();

  // Returns the audio clock, if we have audio, or -1 if we don't.
  // Called on the state machine thread.
  int64_t GetAudioClock();

  // Get the video stream position, taking the |playbackRate| change into
  // account. This is a position in the media, not the duration of the playback
  // so far.
  int64_t GetVideoStreamPosition();

  // Return the current time, either the audio clock if available (if the media
  // has audio, and the playback is possible), or a clock for the video.
  // Called on the state machine thread.
  int64_t GetClock();

  // Returns the presentation time of the first audio or video frame in the
  // media.  If the media has video, it returns the first video frame. The
  // decoder monitor must be held with exactly one lock count. Called on the
  // state machine thread.
  VideoData* FindStartTime();

  // Update only the state machine's current playback position (and duration,
  // if unknown).  Does not update the playback position on the decoder or
  // media element -- use UpdatePlaybackPosition for that.  Called on the state
  // machine thread, caller must hold the decoder lock.
  void UpdatePlaybackPositionInternal(int64_t aTime);

  // Pushes the image down the rendering pipeline. Called on the shared state
  // machine thread. The decoder monitor must *not* be held when calling this.
  void RenderVideoFrame(VideoData* aData, TimeStamp aTarget);

  // If we have video, display a video frame if it's time for display has
  // arrived, otherwise sleep until it's time for the next frame. Update the
  // current frame time as appropriate, and trigger ready state update.  The
  // decoder monitor must be held with exactly one lock count. Called on the
  // state machine thread.
  void AdvanceFrame();

  // Write aFrames of audio frames of silence to the audio hardware. Returns
  // the number of frames actually written. The write size is capped at
  // SILENCE_BYTES_CHUNK (32kB), so must be called in a loop to write the
  // desired number of frames. This ensures that the playback position
  // advances smoothly, and guarantees that we don't try to allocate an
  // impossibly large chunk of memory in order to play back silence. Called
  // on the audio thread.
  uint32_t PlaySilence(uint32_t aFrames,
                       uint32_t aChannels,
                       uint64_t aFrameOffset);

  // Pops an audio chunk from the front of the audio queue, and pushes its
  // audio data to the audio hardware.
  uint32_t PlayFromAudioQueue(uint64_t aFrameOffset, uint32_t aChannels);

  // Stops the audio thread. The decoder monitor must be held with exactly
  // one lock count. Called on the state machine thread.
  void StopAudioThread();

  // Starts the audio thread. The decoder monitor must be held with exactly
  // one lock count. Called on the state machine thread.
  nsresult StartAudioThread();

  // The main loop for the audio thread. Sent to the thread as
  // an nsRunnableMethod. This continually does blocking writes to
  // to audio stream to play audio data.
  void AudioLoop();

  // Sets internal state which causes playback of media to pause.
  // The decoder monitor must be held.
  void StopPlayback();

  // Sets internal state which causes playback of media to begin or resume.
  // Must be called with the decode monitor held.
  void StartPlayback();

  // Moves the decoder into decoding state. Called on the state machine
  // thread. The decoder monitor must be held.
  void StartDecoding();

  // Moves the decoder into the shutdown state, and dispatches an error
  // event to the media element. This begins shutting down the decoder.
  // The decoder monitor must be held. This is only called on the
  // decode thread.
  void DecodeError();

  void StartWaitForResources();

  // Dispatches a task to the decode task queue to begin decoding metadata.
  // This is threadsafe and can be called on any thread.
  // The decoder monitor must be held.
  nsresult EnqueueDecodeMetadataTask();

  nsresult DispatchAudioDecodeTaskIfNeeded();

  // Ensures a to decode audio has been dispatched to the decode task queue.
  // If a task to decode has already been dispatched, this does nothing,
  // otherwise this dispatches a task to do the decode.
  // This is called on the state machine or decode threads.
  // The decoder monitor must be held.
  nsresult EnsureAudioDecodeTaskQueued();

  nsresult DispatchVideoDecodeTaskIfNeeded();

  // Ensures a to decode video has been dispatched to the decode task queue.
  // If a task to decode has already been dispatched, this does nothing,
  // otherwise this dispatches a task to do the decode.
  // The decoder monitor must be held.
  nsresult EnsureVideoDecodeTaskQueued();

  // Dispatches a task to the decode task queue to seek the decoder.
  // The decoder monitor must be held.
  nsresult EnqueueDecodeSeekTask();

  // Calls the reader's SetIdle(), with aIsIdle as parameter. This is only
  // called in a task dispatched to the decode task queue, don't call it
  // directly.
  void SetReaderIdle();
  void SetReaderActive();

  // Re-evaluates the state and determines whether we need to dispatch
  // events to run the decode, or if not whether we should set the reader
  // to idle mode. This is threadsafe, and can be called from any thread.
  // The decoder monitor must be held.
  void DispatchDecodeTasksIfNeeded();

  // Called before we do anything on the decode task queue to set the reader
  // as not idle if it was idle. This is called before we decode, seek, or
  // decode metadata (in case we were dormant or awaiting resources).
  void EnsureActive();

  // Queries our state to see whether the decode has finished for all streams.
  // If so, we move into DECODER_STATE_COMPLETED and schedule the state machine
  // to run.
  // The decoder monitor must be held.
  void CheckIfDecodeComplete();

  // Returns the "media time". This is the absolute time which the media
  // playback has reached. i.e. this returns values in the range
  // [mStartTime, mEndTime], and mStartTime will not be 0 if the media does
  // not start at 0. Note this is different to the value returned
  // by GetCurrentTime(), which is in the range [0,duration].
  int64_t GetMediaTime() const {
    AssertCurrentThreadInMonitor();
    return mStartTime + mCurrentFrameTime;
  }

  // Returns an upper bound on the number of microseconds of audio that is
  // decoded and playable. This is the sum of the number of usecs of audio which
  // is decoded and in the reader's audio queue, and the usecs of unplayed audio
  // which has been pushed to the audio hardware for playback. Note that after
  // calling this, the audio hardware may play some of the audio pushed to
  // hardware, so this can only be used as a upper bound. The decoder monitor
  // must be held when calling this. Called on the decode thread.
  int64_t GetDecodedAudioDuration();

  // Load metadata. Called on the decode thread. The decoder monitor
  // must be held with exactly one lock count.
  nsresult DecodeMetadata();

  // Seeks to mSeekTarget. Called on the decode thread. The decoder monitor
  // must be held with exactly one lock count.
  void DecodeSeek();

  // Decode loop, decodes data until EOF or shutdown.
  // Called on the decode thread.
  void DecodeLoop();

  void CallDecodeMetadata();

  // Copy audio from an AudioData packet to aOutput. This may require
  // inserting silence depending on the timing of the audio packet.
  void SendStreamAudio(AudioData* aAudio, DecodedStreamData* aStream,
                       AudioSegment* aOutput);

  // State machine thread run function. Defers to RunStateMachine().
  nsresult CallRunStateMachine();

  // Performs one "cycle" of the state machine. Polls the state, and may send
  // a video frame to be displayed, and generally manages the decode. Called
  // periodically via timer to ensure the video stays in sync.
  nsresult RunStateMachine();

  bool IsStateMachineScheduled() const {
    AssertCurrentThreadInMonitor();
    return !mTimeout.IsNull();
  }

  // Returns true if we're not playing and the decode thread has filled its
  // decode buffers and is waiting. We can shut the decode thread down in this
  // case as it may not be needed again.
  bool IsPausedAndDecoderWaiting();

  // The decoder object that created this state machine. The state machine
  // holds a strong reference to the decoder to ensure that the decoder stays
  // alive once media element has started the decoder shutdown process, and has
  // dropped its reference to the decoder. This enables the state machine to
  // keep using the decoder's monitor until the state machine has finished
  // shutting down, without fear of the monitor being destroyed. After
  // shutting down, the state machine will then release this reference,
  // causing the decoder to be destroyed. This is accessed on the decode,
  // state machine, audio and main threads.
  nsRefPtr<MediaDecoder> mDecoder;

  // The decoder monitor must be obtained before modifying this state.
  // NotifyAll on the monitor must be called when the state is changed so
  // that interested threads can wake up and alter behaviour if appropriate
  // Accessed on state machine, audio, main, and AV thread.
  State mState;

  // Thread for pushing audio onto the audio hardware.
  // The "audio push thread".
  nsCOMPtr<nsIThread> mAudioThread;

  // The task queue in which we run decode tasks. This is referred to as
  // the "decode thread", though in practise tasks can run on a different
  // thread every time they're called.
  RefPtr<MediaTaskQueue> mDecodeTaskQueue;

  RefPtr<SharedThreadPool> mStateMachineThreadPool;

  // Timer to run the state machine cycles. Used by
  // ScheduleStateMachine(). Access protected by decoder monitor.
  nsCOMPtr<nsITimer> mTimer;

  // Timestamp at which the next state machine cycle will run.
  // Access protected by decoder monitor.
  TimeStamp mTimeout;

  // Used to check if there are state machine cycles are running in sequence.
  DebugOnly<bool> mInRunningStateMachine;

  // The time that playback started from the system clock. This is used for
  // timing the presentation of video frames when there's no audio.
  // Accessed only via the state machine thread.
  TimeStamp mPlayStartTime;

  // When we start writing decoded data to a new DecodedDataStream, or we
  // restart writing due to PlaybackStarted(), we record where we are in the
  // MediaStream and what that corresponds to in the media.
  StreamTime mSyncPointInMediaStream;
  int64_t mSyncPointInDecodedStream; // microseconds

  // When the playbackRate changes, and there is no audio clock, it is necessary
  // to reset the mPlayStartTime. This is done next time the clock is queried,
  // when this member is true. Access protected by decoder monitor.
  bool mResetPlayStartTime;

  // The amount of time we've spent playing already the media. The current
  // playback position is therefore |Now() - mPlayStartTime +
  // mPlayDuration|, which must be adjusted by mStartTime if used with media
  // timestamps.  Accessed only via the state machine thread.
  int64_t mPlayDuration;

  // Time that buffering started. Used for buffering timeout and only
  // accessed on the state machine thread. This is null while we're not
  // buffering.
  TimeStamp mBufferingStart;

  // Start time of the media, in microseconds. This is the presentation
  // time of the first frame decoded from the media, and is used to calculate
  // duration and as a bounds for seeking. Accessed on state machine, decode,
  // and main threads. Access controlled by decoder monitor.
  int64_t mStartTime;

  // Time of the last frame in the media, in microseconds. This is the
  // end time of the last frame in the media. Accessed on state
  // machine, decode, and main threads. Access controlled by decoder monitor.
  int64_t mEndTime;

  // Position to seek to in microseconds when the seek state transition occurs.
  // The decoder monitor lock must be obtained before reading or writing
  // this value. Accessed on main and decode thread.
  SeekTarget mSeekTarget;

  // Media Fragment end time in microseconds. Access controlled by decoder monitor.
  int64_t mFragmentEndTime;

  // The audio stream resource. Used on the state machine, and audio threads.
  // This is created and destroyed on the audio thread, while holding the
  // decoder monitor, so if this is used off the audio thread, you must
  // first acquire the decoder monitor and check that it is non-null.
  RefPtr<AudioStream> mAudioStream;

  // The reader, don't call its methods with the decoder monitor held.
  // This is created in the play state machine's constructor, and destroyed
  // in the play state machine's destructor.
  nsAutoPtr<MediaDecoderReader> mReader;

  // Accessed only on the state machine thread.
  // Not an nsRevocableEventPtr since we must Revoke() it well before
  // this object is destroyed, anyway.
  // Protected by decoder monitor except during the SHUTDOWN state after the
  // decoder thread has been stopped.
  nsRevocableEventPtr<WakeDecoderRunnable> mPendingWakeDecoder;

  // The time of the current frame in microseconds. This is referenced from
  // 0 which is the initial playback position. Set by the state machine
  // thread, and read-only from the main thread to get the current
  // time value. Synchronised via decoder monitor.
  int64_t mCurrentFrameTime;

  // The presentation time of the first audio frame that was played in
  // microseconds. We can add this to the audio stream position to determine
  // the current audio time. Accessed on audio and state machine thread.
  // Synchronized by decoder monitor.
  int64_t mAudioStartTime;

  // The end time of the last audio frame that's been pushed onto the audio
  // hardware in microseconds. This will approximately be the end time of the
  // audio stream, unless another frame is pushed to the hardware.
  int64_t mAudioEndTime;

  // The presentation end time of the last video frame which has been displayed
  // in microseconds. Accessed from the state machine thread.
  int64_t mVideoFrameEndTime;

  // Volume of playback. 0.0 = muted. 1.0 = full volume. Read/Written
  // from the state machine and main threads. Synchronised via decoder
  // monitor.
  double mVolume;

  // Playback rate. 1.0 : normal speed, 0.5 : two times slower. Synchronized via
  // decoder monitor.
  double mPlaybackRate;

  // Pitch preservation for the playback rate. Synchronized via decoder monitor.
  bool mPreservesPitch;

  // Position at which the last playback rate change occured, used to compute
  // the actual position in the stream when the playback rate changes and there
  // is no audio to be sync-ed to. Synchronized via decoder monitor.
  int64_t mBasePosition;

  // Time at which we started decoding. Synchronised via decoder monitor.
  TimeStamp mDecodeStartTime;

  // The maximum number of second we spend buffering when we are short on
  // unbuffered data.
  uint32_t mBufferingWait;
  int64_t  mLowDataThresholdUsecs;

  // If we've got more than mAmpleVideoFrames decoded video frames waiting in
  // the video queue, we will not decode any more video frames until some have
  // been consumed by the play state machine thread.
  uint32_t mAmpleVideoFrames;

  // Low audio threshold. If we've decoded less than this much audio we
  // consider our audio decode "behind", and we may skip video decoding
  // in order to allow our audio decoding to catch up. We favour audio
  // decoding over video. We increase this threshold if we're slow to
  // decode video frames, in order to reduce the chance of audio underruns.
  // Note that we don't ever reset this threshold, it only ever grows as
  // we detect that the decode can't keep up with rendering.
  int64_t mLowAudioThresholdUsecs;

  // Our "ample" audio threshold. Once we've this much audio decoded, we
  // pause decoding. If we increase mLowAudioThresholdUsecs, we'll also
  // increase this too appropriately (we don't want mLowAudioThresholdUsecs
  // to be greater than ampleAudioThreshold, else we'd stop decoding!).
  // Note that we don't ever reset this threshold, it only ever grows as
  // we detect that the decode can't keep up with rendering.
  int64_t mAmpleAudioThresholdUsecs;

  // At the start of decoding we want to "preroll" the decode until we've
  // got a few frames decoded before we consider whether decode is falling
  // behind. Otherwise our "we're falling behind" logic will trigger
  // unneccessarily if we start playing as soon as the first sample is
  // decoded. These two fields store how many video frames and audio
  // samples we must consume before are considered to be finished prerolling.
  uint32_t mAudioPrerollUsecs;
  uint32_t mVideoPrerollFrames;

  // When we start decoding (either for the first time, or after a pause)
  // we may be low on decoded data. We don't want our "low data" logic to
  // kick in and decide that we're low on decoded data because the download
  // can't keep up with the decode, and cause us to pause playback. So we
  // have a "preroll" stage, where we ignore the results of our "low data"
  // logic during the first few frames of our decode. This occurs during
  // playback. The flags below are true when the corresponding stream is
  // being "prerolled".
  bool mIsAudioPrerolling;
  bool mIsVideoPrerolling;

  // True when we have an audio stream that we're decoding, and we have not
  // yet decoded to end of stream.
  bool mIsAudioDecoding;

  // True when we have a video stream that we're decoding, and we have not
  // yet decoded to end of stream.
  bool mIsVideoDecoding;

  // True when we have dispatched a task to the decode task queue to run
  // the audio decode.
  bool mDispatchedAudioDecodeTask;

  // True when we have dispatched a task to the decode task queue to run
  // the video decode.
  bool mDispatchedVideoDecodeTask;

  // True when the reader is initialized, but has been ordered "idle" by the
  // state machine. This happens when the MediaQueue's of decoded data are
  // "full" and playback is paused. The reader may choose to use the idle
  // notification to enter a low power state.
  bool mIsReaderIdle;

  // If the video decode is falling behind the audio, we'll start dropping the
  // inter-frames up until the next keyframe which is at or before the current
  // playback position. skipToNextKeyframe is true if we're currently
  // skipping up to the next keyframe.
  bool mSkipToNextKeyFrame;

  // True if we shouldn't play our audio (but still write it to any capturing
  // streams). When this is true, mStopAudioThread is always true and
  // the audio thread will never start again after it has stopped.
  bool mAudioCaptured;

  // True if the media resource can be seeked on a transport level. Accessed
  // from the state machine and main threads. Synchronised via decoder monitor.
  bool mTransportSeekable;

  // True if the media can be seeked. Accessed from the state machine and main
  // threads. Synchronised via decoder monitor.
  bool mMediaSeekable;

  // True if an event to notify about a change in the playback
  // position has been queued, but not yet run. It is set to false when
  // the event is run. This allows coalescing of these events as they can be
  // produced many times per second. Synchronised via decoder monitor.
  // Accessed on main and state machine threads.
  bool mPositionChangeQueued;

  // True if the audio playback thread has finished. It is finished
  // when either all the audio frames have completed playing, or we've moved
  // into shutdown state, and the threads are to be
  // destroyed. Written by the audio playback thread and read and written by
  // the state machine thread. Synchronised via decoder monitor.
  // When data is being sent to a MediaStream, this is true when all data has
  // been written to the MediaStream.
  bool mAudioCompleted;

  // True if mDuration has a value obtained from an HTTP header, or from
  // the media index/metadata. Accessed on the state machine thread.
  bool mGotDurationFromMetaData;

  // True if we've dispatched an event to the decode task queue to call
  // DecodeThreadRun(). We use this flag to prevent us from dispatching
  // unneccessary runnables, since the decode thread runs in a loop.
  bool mDispatchedEventToDecode;

  // False while audio thread should be running. Accessed state machine
  // and audio threads. Syncrhonised by decoder monitor.
  bool mStopAudioThread;

  // If this is true while we're in buffering mode, we can exit early,
  // as it's likely we may be able to playback. This happens when we enter
  // buffering mode soon after the decode starts, because the decode-ahead
  // ran fast enough to exhaust all data while the download is starting up.
  // Synchronised via decoder monitor.
  bool mQuickBuffering;

  // True if we should not decode/preroll unnecessary samples, unless we're
  // played. "Prerolling" in this context refers to when we decode and
  // buffer decoded samples in advance of when they're needed for playback.
  // This flag is set for preload=metadata media, and means we won't
  // decode more than the first video frame and first block of audio samples
  // for that media when we startup, or after a seek. When Play() is called,
  // we reset this flag, as we assume the user is playing the media, so
  // prerolling is appropriate then. This flag is used to reduce the overhead
  // of prerolling samples for media elements that may not play, both
  // memory and CPU overhead.
  bool mMinimizePreroll;

  // True if the decode thread has gone filled its buffers and is now
  // waiting to be awakened before it continues decoding. Synchronized
  // by the decoder monitor.
  bool mDecodeThreadWaiting;

  // True is we are decoding a realtime stream, like a camera stream
  bool mRealTime;

  // Stores presentation info required for playback. The decoder monitor
  // must be held when accessing this.
  MediaInfo mInfo;

  mozilla::MediaMetadataManager mMetadataManager;

  MediaDecoderOwner::NextFrameStatus mLastFrameStatus;

  // The id of timer tasks, used to ignore tasks that are scheduled previously.
  int mTimerId;
};

} // namespace mozilla;
#endif
