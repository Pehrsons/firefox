/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_MEDIA_VIDEOTRACKGENERATOR_H_
#define DOM_MEDIA_VIDEOTRACKGENERATOR_H_

#include "js/TypeDecls.h"
#include "mozilla/AlreadyAddRefed.h"
#include "mozilla/RefPtr.h"
#include "mozilla/dom/BindingDeclarations.h"
#include "nsCOMPtr.h"
#include "nsCycleCollectionParticipant.h"
#include "nsISupports.h"
#include "nsWrapperCache.h"

class nsIGlobalObject;

namespace mozilla {
class ErrorResult;

namespace dom {

class MediaStreamTrack;
class WritableStream;

// https://w3c.github.io/mediacapture-transform/#video-track-generator
//
// [[track]] is not implemented, so the track attribute throws. It needs a
// MediaStreamTrackSource owned by the worker the generator lives on, which
// MediaStreamTrackSourceHandle does not support yet. Until then no frame
// written to writable reaches a sink, so writeFrame step 3 is missing too.
class VideoTrackGenerator final : public nsISupports, public nsWrapperCache {
 public:
  NS_DECL_CYCLE_COLLECTING_ISUPPORTS_FINAL
  NS_DECL_CYCLE_COLLECTION_WRAPPERCACHE_CLASS(VideoTrackGenerator)

  explicit VideoTrackGenerator(nsIGlobalObject* aGlobal);

  static already_AddRefed<VideoTrackGenerator> Constructor(
      const GlobalObject& aGlobal);

  nsIGlobalObject* GetParentObject() const;

  JSObject* WrapObject(JSContext* aCx,
                       JS::Handle<JSObject*> aGivenProto) override;

  already_AddRefed<WritableStream> GetWritable(JSContext* aCx,
                                               ErrorResult& aRv);

  bool Muted() const { return mMuted; }
  void SetMuted(bool aMuted);

  already_AddRefed<MediaStreamTrack> GetTrack(ErrorResult& aRv) const;

 private:
  ~VideoTrackGenerator();

  nsCOMPtr<nsIGlobalObject> mGlobal;
  // Created on first access to the writable attribute, per its steps. Holds
  // the underlying sink through its controller.
  RefPtr<WritableStream> mWritable;
  // [[isMuted]]
  bool mMuted = false;
};

}  // namespace dom
}  // namespace mozilla

#endif  // DOM_MEDIA_VIDEOTRACKGENERATOR_H_
