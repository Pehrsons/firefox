/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_MEDIA_MEDIASTREAMTRACKPROCESSOR_H_
#define DOM_MEDIA_MEDIASTREAMTRACKPROCESSOR_H_

#include <cstdint>

#include "js/TypeDecls.h"
#include "mozilla/AlreadyAddRefed.h"
#include "mozilla/Atomics.h"
#include "mozilla/RefPtr.h"
#include "mozilla/dom/BindingDeclarations.h"
#include "nsCOMPtr.h"
#include "nsCycleCollectionParticipant.h"
#include "nsISupports.h"
#include "nsISupportsImpl.h"
#include "nsTArray.h"
#include "nsWrapperCache.h"

class nsIGlobalObject;

namespace mozilla {
class ErrorResult;

namespace dom {

class MediaStreamTrack;
class MediaStreamTrackProcessorListener;
class MediaStreamTrackProcessorSource;
class ReadableStream;
class StrongWorkerRef;
class VideoFrame;
struct MediaStreamTrackProcessorInit;

// The frame counts the processor exposes, shared with its listener. A frame
// counts as received once it reaches the listener, which is what lets frames
// the listener drops before the worker ever sees them count too. Refcounted
// because the listener can outlive the processor.
struct MediaStreamTrackProcessorCounters final {
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(MediaStreamTrackProcessorCounters)

  // [[numTotalFrames]]
  Atomic<uint64_t> mTotalFrames{0};
  // [[numDiscardedFrames]]
  Atomic<uint64_t> mDiscardedFrames{0};

 private:
  ~MediaStreamTrackProcessorCounters() = default;
};

// https://w3c.github.io/mediacapture-transform/#track-processor
//
// Only tracks of kind "video" are supported. The spec records WG consensus on
// video but none on audio, and WebAudio is the better fit for audio in Gecko.
class MediaStreamTrackProcessor final : public nsISupports,
                                        public nsWrapperCache {
 public:
  NS_DECL_CYCLE_COLLECTING_ISUPPORTS_FINAL
  NS_DECL_CYCLE_COLLECTION_WRAPPERCACHE_CLASS(MediaStreamTrackProcessor)

  MediaStreamTrackProcessor(nsIGlobalObject* aGlobal, MediaStreamTrack& aTrack,
                            uint16_t aMaxBufferSize);

  static already_AddRefed<MediaStreamTrackProcessor> Constructor(
      const GlobalObject& aGlobal, const MediaStreamTrackProcessorInit& aInit,
      ErrorResult& aRv);

  nsIGlobalObject* GetParentObject() const;

  JSObject* WrapObject(JSContext* aCx,
                       JS::Handle<JSObject*> aGivenProto) override;

  already_AddRefed<ReadableStream> Readable() const;

  uint64_t DiscardedFrames() const { return mCounters->mDiscardedFrames; }
  uint64_t TotalFrames() const { return mCounters->mTotalFrames; }

  // https://w3c.github.io/mediacapture-transform/#handle-new-frame
  // Called for each frame produced by [[track]]. Takes a reference to aFrame.
  // Deliberately not MOZ_CAN_RUN_SCRIPT: it only queues the task that may run
  // script, so a producer can fill [[queue]] without script observing it part
  // filled, and the analysis holds that.
  void HandleNewFrame(VideoFrame& aFrame);

  // https://w3c.github.io/mediacapture-transform/#maybe-read-frame
  MOZ_CAN_RUN_SCRIPT void MaybeReadFrame(JSContext* aCx);

  // https://w3c.github.io/mediacapture-transform/#processor-close
  MOZ_CAN_RUN_SCRIPT void Close(JSContext* aCx);

 private:
  ~MediaStreamTrackProcessor();

  // Creates the readable and takes the worker ref. Separate from Constructor
  // because it needs a JSContext, which WebIDL constructors are not passed.
  void Init(JSContext* aCx, ErrorResult& aRv);

  nsCOMPtr<nsIGlobalObject> mGlobal;
  // [[track]]. Nulled by Close() per processorClose step 3.
  RefPtr<MediaStreamTrack> mTrack;
  // [[maxBufferSize]]
  const uint16_t mMaxBufferSize;
  RefPtr<ReadableStream> mReadable;
  RefPtr<MediaStreamTrackProcessorSource> mSource;
  // Registered with mTrack for the processor's lifetime. Disconnected and
  // removed by Close() per processorClose step 2.
  RefPtr<MediaStreamTrackProcessorListener> mListener;
  // Keeps the worker alive while frames can still be delivered.
  RefPtr<StrongWorkerRef> mWorkerRef;
  // [[queue]], oldest frame first.
  nsTArray<RefPtr<VideoFrame>> mQueue;
  const RefPtr<MediaStreamTrackProcessorCounters> mCounters =
      MakeRefPtr<MediaStreamTrackProcessorCounters>();
  // [[isClosed]]
  bool mIsClosed = false;
};

}  // namespace dom
}  // namespace mozilla

#endif  // DOM_MEDIA_MEDIASTREAMTRACKPROCESSOR_H_
