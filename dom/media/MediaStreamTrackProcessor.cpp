/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "MediaStreamTrackProcessor.h"

#include <algorithm>

#include "MediaStreamTrack.h"
#include "mozilla/ErrorResult.h"
#include "mozilla/Maybe.h"
#include "mozilla/dom/MediaStreamTrackProcessorBinding.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/dom/ReadableStream.h"
#include "mozilla/dom/ReadableStreamDefaultReader.h"
#include "mozilla/dom/ReadableStreamGenericReader.h"
#include "mozilla/dom/ScriptSettings.h"
#include "mozilla/dom/ToJSValue.h"
#include "mozilla/dom/UnderlyingSourceCallbackHelpers.h"
#include "mozilla/dom/VideoFrame.h"
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

MediaStreamTrackProcessor::~MediaStreamTrackProcessor() = default;

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

  // Null if the worker is already shutting down, in which case there is nothing
  // left to keep alive.
  mWorkerRef = StrongWorkerRef::Create(
      workerPrivate, "MediaStreamTrackProcessor",
      [self = RefPtr(this)]() MOZ_CAN_RUN_SCRIPT_BOUNDARY_LAMBDA {
        AutoJSAPI jsapi;
        if (NS_WARN_IF(!jsapi.Init(self->mGlobal))) {
          return;
        }
        self->Close(jsapi.cx());
      });
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
    return;
  }

  // Step 1. Discard the oldest frame if [[queue]] is full.
  if (mQueue.Length() >= mMaxBufferSize) {
    const RefPtr<VideoFrame> oldest = std::move(mQueue[0]);
    mQueue.RemoveElementAt(0);
    oldest->Close();
    ++mNumDiscardedFrames;
  }

  // Step 2. Increment [[numTotalFrames]].
  ++mNumTotalFrames;

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
      ++mNumDiscardedFrames;
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
  // TODO(Bug 1991617): Remove the frame listener once it exists. Nothing is
  // connected to the track yet, so there is nothing to disconnect.

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
