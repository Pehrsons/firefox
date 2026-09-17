/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "VideoTrackGenerator.h"

#include "mozilla/ErrorResult.h"
#include "mozilla/Maybe.h"
#include "mozilla/dom/BindingUtils.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/dom/UnderlyingSinkCallbackHelpers.h"
#include "mozilla/dom/VideoFrame.h"
#include "mozilla/dom/VideoTrackGeneratorBinding.h"
#include "mozilla/dom/WritableStream.h"
#include "nsIGlobalObject.h"

namespace mozilla::dom {

// Underlying sink of the generator's writable.
class VideoTrackGeneratorSink final : public UnderlyingSinkAlgorithmsWrapper {
 public:
  VideoTrackGeneratorSink() = default;

  // https://w3c.github.io/mediacapture-transform/#writeframe
  already_AddRefed<Promise> WriteCallbackImpl(
      JSContext* aCx, JS::Handle<JS::Value> aChunk,
      WritableStreamDefaultController& aController, ErrorResult& aRv) override {
    // Step 1. If frame is not a VideoFrame object, return a promise rejected
    // with a TypeError.
    VideoFrame* frame = nullptr;
    if (!aChunk.isObject()) {
      aRv.ThrowTypeError("Chunk is not a VideoFrame");
      return nullptr;
    }
    JS::Rooted<JSObject*> obj(aCx, &aChunk.toObject());
    if (NS_FAILED(UNWRAP_OBJECT(VideoFrame, &obj, frame))) {
      aRv.ThrowTypeError("Chunk is not a VideoFrame");
      return nullptr;
    }

    // Step 2. If frame's [[Detached]] is true, return a promise rejected with
    // a TypeError.
    if (frame->IsClosed()) {
      aRv.ThrowTypeError("VideoFrame is closed");
      return nullptr;
    }

    // Step 3. If [[isMuted]] is false, clone frame for each live track sourced
    // from the generator and send it there. Not implemented: the generator
    // sources no tracks yet, see Bug 1991618.

    // Step 4. Run the Close VideoFrame algorithm with frame.
    frame->Close();

    // Step 5. Return a promise resolved with undefined. Returning null means
    // "already resolved" to UnderlyingSinkAlgorithmsWrapper.
    return nullptr;
  }

  // https://w3c.github.io/mediacapture-transform/#closewritable
  already_AddRefed<Promise> CloseCallbackImpl(JSContext* aCx,
                                              ErrorResult& aRv) override {
    // Step 1. End each track sourced from the generator. Not implemented, see
    // WriteCallbackImpl step 3.
    return nullptr;
  }

  already_AddRefed<Promise> AbortCallbackImpl(
      JSContext* aCx, const Optional<JS::Handle<JS::Value>>& aReason,
      ErrorResult& aRv) override {
    return CloseCallbackImpl(aCx, aRv);
  }

 private:
  ~VideoTrackGeneratorSink() = default;
};

NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE(VideoTrackGenerator, mGlobal, mWritable)

NS_IMPL_CYCLE_COLLECTING_ADDREF(VideoTrackGenerator)
NS_IMPL_CYCLE_COLLECTING_RELEASE(VideoTrackGenerator)

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(VideoTrackGenerator)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END

VideoTrackGenerator::VideoTrackGenerator(nsIGlobalObject* aGlobal)
    : mGlobal(aGlobal) {}

VideoTrackGenerator::~VideoTrackGenerator() = default;

/* static */
already_AddRefed<VideoTrackGenerator> VideoTrackGenerator::Constructor(
    const GlobalObject& aGlobal) {
  nsCOMPtr<nsIGlobalObject> global = do_QueryInterface(aGlobal.GetAsSupports());
  MOZ_ASSERT(global);

  // The spec also creates [[track]] here, see the class comment.
  return do_AddRef(new VideoTrackGenerator(global));
}

nsIGlobalObject* VideoTrackGenerator::GetParentObject() const {
  return mGlobal;
}

JSObject* VideoTrackGenerator::WrapObject(JSContext* aCx,
                                          JS::Handle<JSObject*> aGivenProto) {
  return VideoTrackGenerator_Binding::Wrap(aCx, this, aGivenProto);
}

already_AddRefed<WritableStream> VideoTrackGenerator::GetWritable(
    JSContext* aCx, ErrorResult& aRv) {
  if (!mWritable) {
    // Step 1. Initialize writable to be a new WritableStream.
    // Step 2. Set it up with writeFrame as its write algorithm and
    // closeWritable as both its close and abort algorithms.
    const auto sink = MakeRefPtr<VideoTrackGeneratorSink>();
    mWritable = WritableStream::CreateNative(aCx, *mGlobal, *sink, Nothing(),
                                             nullptr, aRv);
    if (aRv.Failed()) {
      return nullptr;
    }
  }
  return do_AddRef(mWritable);
}

void VideoTrackGenerator::SetMuted(bool aMuted) {
  // Step 1. If newValue is equal to [[isMuted]], abort these steps.
  if (aMuted == mMuted) {
    return;
  }

  // Step 2. Set [[isMuted]] to newValue.
  mMuted = aMuted;

  // Step 3. Queue a task to set the muted state of each live track sourced by
  // the generator. Not implemented, see WriteCallbackImpl step 3.
}

already_AddRefed<MediaStreamTrack> VideoTrackGenerator::GetTrack(
    ErrorResult& aRv) const {
  aRv.ThrowNotSupportedError("VideoTrackGenerator.track is not implemented");
  return nullptr;
}

}  // namespace mozilla::dom
