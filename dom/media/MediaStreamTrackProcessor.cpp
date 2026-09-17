/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "MediaStreamTrackProcessor.h"

#include <algorithm>

#include "ImageContainer.h"
#include "MediaStreamTrack.h"
#include "MediaTrackGraph.h"
#include "MediaTrackListener.h"
#include "VideoSegment.h"
#include "mozilla/ErrorResult.h"
#include "mozilla/Maybe.h"
#include "mozilla/Mutex.h"
#include "mozilla/dom/ImageUtils.h"
#include "mozilla/dom/MediaStreamTrackProcessorBinding.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/dom/ReadableStream.h"
#include "mozilla/dom/ReadableStreamDefaultReader.h"
#include "mozilla/dom/ReadableStreamGenericReader.h"
#include "mozilla/dom/ScriptSettings.h"
#include "mozilla/dom/ToJSValue.h"
#include "mozilla/dom/UnderlyingSourceCallbackHelpers.h"
#include "mozilla/dom/VideoFrame.h"
#include "mozilla/dom/WebCodecsUtils.h"
#include "mozilla/dom/WorkerCommon.h"
#include "mozilla/dom/WorkerPrivate.h"
#include "mozilla/dom/WorkerRef.h"
#include "nsIGlobalObject.h"
#include "nsThreadUtils.h"

namespace mozilla::dom {

// Underlying source for the processor's readable. The spec keeps [[queue]] and
// the read bookkeeping on the processor, so this only forwards to it.
class MediaStreamTrackProcessorSource final
    : public UnderlyingSourceAlgorithmsWrapper {
 public:
  explicit MediaStreamTrackProcessorSource(
      MediaStreamTrackProcessor* aProcessor)
      : mProcessor(aProcessor) {}

  NS_DECL_ISUPPORTS_INHERITED
  NS_DECL_CYCLE_COLLECTION_CLASS_INHERITED(MediaStreamTrackProcessorSource,
                                           UnderlyingSourceAlgorithmsWrapper)

  // https://w3c.github.io/mediacapture-transform/#processor-pull
  MOZ_CAN_RUN_SCRIPT already_AddRefed<Promise> PullCallbackImpl(
      JSContext* aCx, ReadableStreamControllerBase& aController,
      ErrorResult& aRv) override {
    // Step 1. Increment [[numPendingReads]]. Nothing to do: the readable's own
    // read requests are that count, see MaybeReadFrame.

    // Step 2. Queue a task to run maybeReadFrame. The task boundary is load
    // bearing: pull runs synchronously from read(), so running maybeReadFrame
    // here would let a read overtake a frame already on its way to [[queue]]
    // and hand out a staler frame than the processor is holding.
    const RefPtr<MediaStreamTrackProcessorSource> self = this;
    (void)NS_DispatchToCurrentThread(NS_NewRunnableFunction(
        __func__, [self]() MOZ_CAN_RUN_SCRIPT_BOUNDARY_LAMBDA {
          const RefPtr<MediaStreamTrackProcessor> processor = self->mProcessor;
          AutoJSAPI jsapi;
          if (NS_WARN_IF(!jsapi.Init(processor->GetParentObject()))) {
            return;
          }
          processor->MaybeReadFrame(jsapi.cx());
        }));

    // Step 3. Return a promise resolved with undefined. Returning null means
    // "already resolved" to UnderlyingSourceAlgorithmsWrapper.
    return nullptr;
  }

  // https://w3c.github.io/mediacapture-transform/#processor-cancel
  MOZ_CAN_RUN_SCRIPT already_AddRefed<Promise> CancelCallbackImpl(
      JSContext* aCx, const Optional<JS::Handle<JS::Value>>& aReason,
      ErrorResult& aRv) override {
    const RefPtr<MediaStreamTrackProcessor> processor = mProcessor;
    processor->Close(aCx);
    return nullptr;
  }

 private:
  ~MediaStreamTrackProcessorSource() = default;

  RefPtr<MediaStreamTrackProcessor> mProcessor;
};

NS_IMPL_CYCLE_COLLECTION_INHERITED(MediaStreamTrackProcessorSource,
                                   UnderlyingSourceAlgorithmsWrapper,
                                   mProcessor)
NS_IMPL_ADDREF_INHERITED(MediaStreamTrackProcessorSource,
                         UnderlyingSourceAlgorithmsWrapper)
NS_IMPL_RELEASE_INHERITED(MediaStreamTrackProcessorSource,
                          UnderlyingSourceAlgorithmsWrapper)
NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(MediaStreamTrackProcessorSource)
NS_INTERFACE_MAP_END_INHERITING(UnderlyingSourceAlgorithmsWrapper)

// Feeds frames from [[track]] to the processor. Registered both as a direct
// listener, which is the only place video data arrives, and as a plain
// listener for the track's lifecycle, mirroring
// MediaPipelineTransmit::PipelineListener.
class MediaStreamTrackProcessorListener final
    : public DirectMediaTrackListener {
  // A frame waiting to be handed to the worker. Holds what the worker needs
  // to build a VideoFrame, since layers::Image is all we can carry across.
  struct PendingFrame {
    RefPtr<layers::Image> mImage;
    gfx::IntSize mIntrinsicSize;
    TimeStamp mTimeStamp;
    Maybe<uint64_t> mDuration;
    bool mForceBlack;
  };

 public:
  MediaStreamTrackProcessorListener(
      MediaStreamTrackProcessor* aProcessor,
      MediaStreamTrackProcessorCounters* aCounters, uint16_t aMaxBufferSize)
      : mProcessor(aProcessor),
        mCounters(aCounters),
        mWorkerThread(GetCurrentSerialEventTarget()),
        mMaxBufferSize(aMaxBufferSize),
        mMutex("MediaStreamTrackProcessorListener::mMutex") {
    MOZ_ASSERT(mProcessor);
    MOZ_ASSERT(mWorkerThread);
  }

  void NotifyRealtimeTrackData(MediaTrackGraph* aGraph, TrackTime,
                               const MediaSegment& aMedia) override {
    if (aMedia.GetType() != MediaSegment::VIDEO) {
      return;
    }
    const VideoSegment& video = static_cast<const VideoSegment&>(aMedia);
    for (VideoSegment::ConstChunkIterator it(video); !it.IsEnded(); it.Next()) {
      QueueFrame(aGraph->GraphRate(), *it);
    }
  }

  // Video always arrives through NotifyRealtimeTrackData, as in
  // MediaPipelineTransmit::PipelineListener.
  void NotifyQueuedChanges(MediaTrackGraph*, TrackTime,
                           const MediaSegment&) override {}

  void NotifyEnded(MediaTrackGraph*) override { DispatchClose(); }
  void NotifyRemoved(MediaTrackGraph*) override { DispatchClose(); }

  // Only SourceMediaTrack and ForwardedInputTrack install direct listeners,
  // and a video track's graph track is always one of those, so there is no
  // fallback to a non-direct listener here. MediaTrack's base implementation,
  // which reports TRACK_NOT_SUPPORTED, is reachable only for audio, which the
  // constructor turns away.
  void NotifyDirectListenerInstalled(InstallationResult aResult) override {
    MOZ_ASSERT(aResult == InstallationResult::SUCCESS,
               "Without a direct listener no frame ever arrives");
  }

  // Worker thread. The cutoff for everything this listener does: the graph can
  // still deliver until it processes the listener removal, which is why
  // QueueFrame and DispatchClose check mDisconnected rather than mProcessor.
  // Returns how many frames were still waiting here, none of which ever
  // reached [[queue]].
  size_t Disconnect() {
    MOZ_ASSERT(mWorkerThread->IsOnCurrentThread());
    mProcessor = nullptr;
    MutexAutoLock lock(mMutex);
    mDisconnected = true;
    const size_t stranded = mPending.Length();
    mPending.Clear();
    return stranded;
  }

  // Worker thread. Whether frames are still waiting to be handed over, in
  // which case [[queue]] is only part of the queue, see MaybeReadFrame.
  bool HasPendingFrames() {
    MOZ_ASSERT(mWorkerThread->IsOnCurrentThread());
    MutexAutoLock lock(mMutex);
    return !mPending.IsEmpty();
  }

 private:
  ~MediaStreamTrackProcessorListener() = default;

  // Called under SourceMediaTrack::mMutex on whichever thread appended to the
  // track, so this must not block or re-enter the graph. Blacking a frame
  // allocates, so that is left to the worker.
  void QueueFrame(TrackRate aRate, const VideoChunk& aChunk) {
    // "No video" for the chunk's duration. Not a frame, so it is not counted.
    if (aChunk.IsNull()) {
      return;
    }
    // Bookkeeping for a late start of the track rather than a frame, as in
    // SourceMediaTrack::AddDirectListenerImpl.
    if (aChunk.mTimeStamp.IsNull()) {
      return;
    }
    const gfx::IntSize size = aChunk.mFrame.GetIntrinsicSize();
    if (size.width == 0 || size.height == 0) {
      return;
    }

    PendingFrame frame{
        RefPtr<layers::Image>(aChunk.mFrame.GetImage()), size,
        aChunk.mTimeStamp,
        aChunk.mDuration > 0
            ? Some(static_cast<uint64_t>(
                  media::TimeUnit(aChunk.mDuration, aRate).ToMicroseconds()))
            : Nothing(),
        aChunk.mFrame.GetForceBlack()};

    bool dispatch;
    {
      MutexAutoLock lock(mMutex);
      if (mDisconnected) {
        return;
      }
      // Received, whether or not the worker ever gets to see it.
      ++mCounters->mTotalFrames;
      // Bound the frames held for the worker, which script can stop reading
      // for as long as it likes. Drop the oldest, as [[queue]] does, so that
      // the frames kept are always the most recent ones.
      if (mPending.Length() >= mMaxBufferSize) {
        mPending.RemoveElementAt(0);
        ++mCounters->mDiscardedFrames;
      }
      mPending.AppendElement(std::move(frame));
      dispatch = !mDraining;
      mDraining = true;
    }

    if (!dispatch) {
      return;
    }
    // A dispatch can still fail if the worker goes away just after the check
    // above, in which case the frames are moot.
    (void)mWorkerThread->Dispatch(NS_NewRunnableFunction(
        __func__, [self = RefPtr(this)] { self->Drain(); }));
  }

  // Worker thread. Hands every frame waiting in mPending to the processor
  // before returning, rather than one per task, so that whenever script can
  // run [[queue]] is the only place a frame waits. Splitting a drain across
  // tasks would leave older frames in [[queue]] sitting in front of newer ones
  // still in mPending, and a read would get the older frame where the spec's
  // single queue would have discarded it. handleNewFrame only queues the task
  // that may hand a frame to a read, so the whole batch lands before script
  // can see any of it. Taking each frame under the lock is what keeps
  // deliveries in order: nothing is held outside mPending for another drain to
  // overtake.
  void Drain() {
    MOZ_ASSERT(mWorkerThread->IsOnCurrentThread());

    // Disconnected since this was dispatched, which also emptied mPending and
    // stopped QueueFrame, so mDraining no longer matters.
    while (mProcessor) {
      Maybe<PendingFrame> frame;
      {
        MutexAutoLock lock(mMutex);
        if (mPending.IsEmpty()) {
          mDraining = false;
        } else {
          frame = Some(std::move(mPending[0]));
          mPending.RemoveElementAt(0);
        }
      }

      if (!frame) {
        return;
      }

      const RefPtr<MediaStreamTrackProcessor> processor = mProcessor;
      DeliverFrame(*processor, *frame);
    }
  }

  // Worker thread.
  void DeliverFrame(MediaStreamTrackProcessor& aProcessor,
                    const PendingFrame& aFrame) {
    // A disabled track is delivered as chunks flagged force-black that still
    // carry the source image, as MirrorAndDisableSegment leaves it in place.
    // Honouring the flag is the consumer's job, as in CaptureTask.
    RefPtr<layers::Image> image = aFrame.mImage;
    if (aFrame.mForceBlack) {
      if (!mBlackImage || mBlackImage->GetSize() != aFrame.mIntrinsicSize) {
        mBlackImage =
            mozilla::VideoFrame::CloneAsBlackImage(aFrame.mIntrinsicSize);
      }
      if (!mBlackImage) {
        // Never hand out the image of a disabled track. Dropping it counts as
        // discarded, as any other drop does.
        ++mCounters->mDiscardedFrames;
        return;
      }
      image = mBlackImage;
    }

    // pehrsons, Bug 1991617: a MediaStream has no duration and is not
    // seekable, so presentation time starts at 0 and follows wall clock from
    // there. Not the chunk's mMediaTime, which VideoSink sets to the
    // presentation time within the source's resource where there is one, and
    // which therefore restarts on loop or a new src.
    //
    // This is the fallback for a source that has no timestamp of its own. A
    // VideoTrackGenerator does, and the spec expects a frame written to one to
    // come back out of a processor with the timestamp it went in with, so
    // Bug 1991618 has to carry that through the chunk and prefer it here.
    if (mEpoch.IsNull()) {
      mEpoch = aFrame.mTimeStamp;
    }
    const int64_t timestamp =
        static_cast<int64_t>((aFrame.mTimeStamp - mEpoch).ToMicroseconds());

    // TODO(Bug 1991617): Only images ImageUtils understands report a format.
    // Full coverage needs VideoDecoder.cpp's GuessPixelFormat and
    // GuessColorSpace, which are static there.
    const ImageUtils imageUtils(image);
    const Maybe<ImageBitmapFormat> bitmapFormat = imageUtils.GetFormat();
    const Maybe<VideoPixelFormat> format =
        bitmapFormat ? ImageBitmapFormatToVideoPixelFormat(*bitmapFormat)
                     : Nothing();

    const RefPtr<VideoFrame> videoFrame = MakeRefPtr<VideoFrame>(
        aProcessor.GetParentObject(), image, format, image->GetSize(),
        image->GetPictureRect(), aFrame.mIntrinsicSize, aFrame.mDuration,
        timestamp, VideoColorSpaceInternal());
    aProcessor.HandleNewFrame(*videoFrame);
  }

  void DispatchClose() {
    {
      MutexAutoLock lock(mMutex);
      if (mDisconnected) {
        return;
      }
    }
    (void)mWorkerThread->Dispatch(NS_NewRunnableFunction(
        __func__, [self = RefPtr(this)]() MOZ_CAN_RUN_SCRIPT_BOUNDARY_LAMBDA {
          if (!self->mProcessor) {
            return;
          }
          const RefPtr<MediaStreamTrackProcessor> processor = self->mProcessor;
          AutoJSAPI jsapi;
          if (NS_WARN_IF(!jsapi.Init(processor->GetParentObject()))) {
            return;
          }
          processor->Close(jsapi.cx());
        }));
  }

  // The graph thread can drop the last reference to this listener, so every
  // member has to be safe to release there. That rules out the worker's
  // global, which DeliverFrame takes from the processor instead.
  //
  // Worker thread only. Raw because the processor owns this listener and
  // clears the pointer in Close() before it can go away.
  MediaStreamTrackProcessor* mProcessor;
  // Counted into on the graph thread, so it outlives mProcessor by design.
  const RefPtr<MediaStreamTrackProcessorCounters> mCounters;
  const nsCOMPtr<nsISerialEventTarget> mWorkerThread;
  const uint16_t mMaxBufferSize;
  Mutex mMutex;
  // Frames waiting for the worker to pick them up, oldest first.
  nsTArray<PendingFrame> mPending MOZ_GUARDED_BY(mMutex);
  // Whether a drain is already on its way to the worker.
  bool mDraining MOZ_GUARDED_BY(mMutex) = false;
  // Whether Disconnect() has run. Set on the worker, read on the graph thread.
  bool mDisconnected MOZ_GUARDED_BY(mMutex) = false;
  // Worker thread only. The first frame's wall clock time, which is
  // presentation time 0.
  TimeStamp mEpoch;
  // Worker thread only. Stands in for the frames of a disabled track, kept
  // because rebuilding it per frame would allocate for the whole time the
  // track stays disabled.
  RefPtr<layers::Image> mBlackImage;
};

NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE(MediaStreamTrackProcessor, mGlobal,
                                      mTrack, mReadable, mSource, mQueue)

NS_IMPL_CYCLE_COLLECTING_ADDREF(MediaStreamTrackProcessor)
NS_IMPL_CYCLE_COLLECTING_RELEASE(MediaStreamTrackProcessor)

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(MediaStreamTrackProcessor)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END

MediaStreamTrackProcessor::MediaStreamTrackProcessor(nsIGlobalObject* aGlobal,
                                                     MediaStreamTrack& aTrack,
                                                     uint16_t aMaxBufferSize)
    : mGlobal(aGlobal), mTrack(&aTrack), mMaxBufferSize(aMaxBufferSize) {}

MediaStreamTrackProcessor::~MediaStreamTrackProcessor() {
  // Close() normally does this, but nothing guarantees it ran. The listener
  // holds a raw pointer here, so it must not outlive us still registered.
  if (mListener) {
    if (mTrack) {
      mTrack->RemoveDirectListener(mListener);
      mTrack->RemoveListener(mListener);
    }
    mListener->Disconnect();
  }
}

/* static */
already_AddRefed<MediaStreamTrackProcessor>
MediaStreamTrackProcessor::Constructor(
    const GlobalObject& aGlobal, const MediaStreamTrackProcessorInit& aInit,
    ErrorResult& aRv) {
  nsCOMPtr<nsIGlobalObject> global = do_QueryInterface(aGlobal.GetAsSupports());
  MOZ_ASSERT(global);

  // Step 1. If init.track is a MediaStreamTrack that is not valid, throw a
  // TypeError. The spec does not define validity; an ended track can never
  // produce a frame, and a track detached by transfer is ended.
  if (aInit.mTrack->Ended()) {
    aRv.ThrowTypeError("Track is ended");
    return nullptr;
  }

  // Step 2 covers MediaStreamTrackHandle, which Gecko does not implement.
  // TODO(Bug 1991617): Accept MediaStreamTrackOrHandle.

  // Not a spec step. See the class comment on audio.
  if (!aInit.mTrack->AsVideoStreamTrack()) {
    aRv.ThrowTypeError("Track is not a video track");
    return nullptr;
  }

  // Steps 3-4. maxBufferSize defaults to 1, and is only honoured above 1.
  uint16_t maxBufferSize = 1;
  if (aInit.mMaxBufferSize.WasPassed()) {
    maxBufferSize = std::max<uint16_t>(1, aInit.mMaxBufferSize.Value());
  }

  // Steps 5-12. Initialize the internal slots.
  RefPtr<MediaStreamTrackProcessor> processor =
      new MediaStreamTrackProcessor(global, *aInit.mTrack, maxBufferSize);

  AutoJSAPI jsapi;
  if (NS_WARN_IF(!jsapi.Init(global))) {
    aRv.ThrowUnknownError("Failed to enter the global");
    return nullptr;
  }
  processor->Init(jsapi.cx(), aRv);
  if (aRv.Failed()) {
    return nullptr;
  }

  // Step 13. Return processor.
  return processor.forget();
}

void MediaStreamTrackProcessor::Init(JSContext* aCx, ErrorResult& aRv) {
  mSource = new MediaStreamTrackProcessorSource(this);

  // A high water mark of 0 keeps the readable's own queue empty, so that
  // backpressure reaches [[queue]] and frames are discarded there rather than
  // accumulating out of our sight.
  const RefPtr<MediaStreamTrackProcessorSource> source = mSource;
  mReadable = ReadableStream::CreateNative(aCx, mGlobal, *source, Some(0.0),
                                           nullptr, aRv);
  if (aRv.Failed()) {
    return;
  }

  WorkerPrivate* workerPrivate = GetCurrentThreadWorkerPrivate();
  MOZ_ASSERT(workerPrivate,
             "MediaStreamTrackProcessor is DedicatedWorker-only");

  // Taken before the listener is registered, so that the worker the listener
  // dispatches to cannot go away while it is registered. Null means the worker
  // is already shutting down.
  mWorkerRef = StrongWorkerRef::Create(
      workerPrivate, "MediaStreamTrackProcessor",
      [self = RefPtr(this)]() MOZ_CAN_RUN_SCRIPT_BOUNDARY_LAMBDA {
        AutoJSAPI jsapi;
        if (NS_WARN_IF(!jsapi.Init(self->mGlobal))) {
          return;
        }
        self->Close(jsapi.cx());
      });
  if (NS_WARN_IF(!mWorkerRef)) {
    aRv.ThrowInvalidStateError("Worker is shutting down");
    return;
  }

  // Steps 5-12 do not cover connecting to the track; that is left to the UA,
  // as is disconnecting in processorClose step 2.
  mListener =
      new MediaStreamTrackProcessorListener(this, mCounters, mMaxBufferSize);
  mTrack->AddDirectListener(mListener);
  mTrack->AddListener(mListener);
}

nsIGlobalObject* MediaStreamTrackProcessor::GetParentObject() const {
  return mGlobal;
}

JSObject* MediaStreamTrackProcessor::WrapObject(
    JSContext* aCx, JS::Handle<JSObject*> aGivenProto) {
  return MediaStreamTrackProcessor_Binding::Wrap(aCx, this, aGivenProto);
}

already_AddRefed<ReadableStream> MediaStreamTrackProcessor::Readable() const {
  return do_AddRef(mReadable);
}

void MediaStreamTrackProcessor::HandleNewFrame(VideoFrame& aFrame) {
  if (mIsClosed) {
    // The frame was never handed to script. Release its image now rather than
    // at the next GC, which the spec warns can stop the source emitting.
    aFrame.Close();
    return;
  }

  // Step 1. Discard the oldest frame if [[queue]] is full.
  if (mQueue.Length() >= mMaxBufferSize) {
    const RefPtr<VideoFrame> oldest = std::move(mQueue[0]);
    mQueue.RemoveElementAt(0);
    oldest->Close();
    ++mCounters->mDiscardedFrames;
  }

  // Step 2. Increment [[numTotalFrames]]. Counted as the frame reaches the
  // listener instead, so that frames dropped on the way here still count and
  // so that a close cannot lose counts not yet carried over.

  // Step 3. Enqueue the frame in [[queue]].
  mQueue.AppendElement(&aFrame);

  // Step 4. Queue a task to run maybeReadFrame. The task boundary is load
  // bearing: running it here would answer a read from the head of [[queue]]
  // while the producer is still filling it, handing out a frame that the
  // frames it has yet to enqueue were about to displace.
  (void)NS_DispatchToCurrentThread(NS_NewRunnableFunction(
      __func__, [self = RefPtr(this)]() MOZ_CAN_RUN_SCRIPT_BOUNDARY_LAMBDA {
        AutoJSAPI jsapi;
        if (NS_WARN_IF(!jsapi.Init(self->GetParentObject()))) {
          return;
        }
        const RefPtr<MediaStreamTrackProcessor> processor = self;
        processor->MaybeReadFrame(jsapi.cx());
      }));
}

void MediaStreamTrackProcessor::MaybeReadFrame(JSContext* aCx) {
  // Steps 1-5. Satisfy pending reads from [[queue]] until either runs out.
  // Enqueueing runs script, which can cancel the readable and so close us
  // mid-loop; every condition is re-checked on each pass.
  while (!mIsClosed && !mQueue.IsEmpty()) {
    // [[queue]] is the whole queue only while the listener holds nothing.
    // Reading from it with newer frames still waiting there would hand out a
    // frame those arrivals have already displaced, which is what a read
    // issued before the worker stops servicing its event loop would otherwise
    // get: its task is queued ahead of the drain that moves them in. The
    // drain queues another maybeReadFrame behind itself, so the read is
    // answered from the whole queue instead of from part of it.
    if (mListener && mListener->HasPendingFrames()) {
      return;
    }

    // [[numPendingReads]] is the readable's outstanding read requests, rather
    // than a count of our own. Counting pull calls drifts from the reads they
    // stand for: the controller collapses reads made while it is already
    // pulling into a single call, and calls pull again after each read it
    // fulfils. Enqueueing a frame that no read is waiting for would leave it
    // in the controller's queue, out of reach of [[queue]]'s discarding, to go
    // stale until the next read.
    ReadableStreamGenericReader* reader = mReadable->GetReader();
    if (!reader) {
      return;
    }
    // The readable is not a byte stream, so it can have no BYOB reader.
    MOZ_ASSERT(reader->IsDefault());
    if (reader->AsDefault()->ReadRequests().isEmpty()) {
      return;
    }

    // Reflect the frame before dequeueing it, so that failing here leaves
    // [[queue]] untouched and the read is retried on the next pull instead of
    // being lost with the frame.
    JS::Rooted<JS::Value> value(aCx);
    if (NS_WARN_IF(!ToJSValue(aCx, *mQueue[0], &value))) {
      return;
    }

    const RefPtr<VideoFrame> frame = std::move(mQueue[0]);
    mQueue.RemoveElementAt(0);

    IgnoredErrorResult rv;
    const RefPtr<ReadableStream> readable = mReadable;
    readable->EnqueueNative(aCx, value, rv);
    if (NS_WARN_IF(rv.Failed())) {
      // Dequeued but never handed to script, so release the image here rather
      // than leaving it to the next GC, and count the drop as any other.
      frame->Close();
      ++mCounters->mDiscardedFrames;
      return;
    }
  }
}

void MediaStreamTrackProcessor::Close(JSContext* aCx) {
  // Step 1. Abort if [[isClosed]].
  if (mIsClosed) {
    return;
  }

  // Step 2. Disconnect from [[track]].
  if (mListener) {
    mTrack->RemoveDirectListener(mListener);
    mTrack->RemoveListener(mListener);
    const size_t stranded = mListener->Disconnect();
    mListener = nullptr;

    // [[queue]] and the frames the listener was still holding are one queue
    // between them, so anything they hold over [[maxBufferSize]] had already
    // been displaced by the frames that arrived after it. Those displacements
    // are counted here because the frames doing the displacing never reached
    // [[queue]] to do it themselves. What remains is dropped by step 5, which
    // the spec does not count as discarded.
    const size_t buffered = mQueue.Length() + stranded;
    if (buffered > mMaxBufferSize) {
      mCounters->mDiscardedFrames += buffered - mMaxBufferSize;
    }
  }

  // Step 3. Set [[track]] to null.
  mTrack = nullptr;

  // Step 4. Close the readable's controller.
  IgnoredErrorResult rv;
  const RefPtr<ReadableStream> readable = mReadable;
  readable->CloseNative(aCx, rv);

  // Step 5. Empty [[queue]]. Closing the frames releases their images now
  // rather than at the next GC; they were never handed out to script.
  for (const RefPtr<VideoFrame>& frame : mQueue) {
    frame->Close();
  }
  mQueue.Clear();

  // Step 6. Set [[isClosed]] to true.
  mIsClosed = true;

  mWorkerRef = nullptr;
}

}  // namespace mozilla::dom
