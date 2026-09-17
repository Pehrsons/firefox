/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_MEDIA_MEDIASTREAMTRACKPROCESSOR_H_
#define DOM_MEDIA_MEDIASTREAMTRACKPROCESSOR_H_

#include <cstdint>

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

class ReadableStream;
struct MediaStreamTrackProcessorInit;

class MediaStreamTrackProcessor final : public nsISupports,
                                        public nsWrapperCache {
 public:
  NS_DECL_CYCLE_COLLECTING_ISUPPORTS_FINAL
  NS_DECL_CYCLE_COLLECTION_WRAPPERCACHE_CLASS(MediaStreamTrackProcessor)

  explicit MediaStreamTrackProcessor(nsIGlobalObject* aGlobal);

  static already_AddRefed<MediaStreamTrackProcessor> Constructor(
      const GlobalObject& aGlobal, const MediaStreamTrackProcessorInit& aInit,
      ErrorResult& aRv);

  nsIGlobalObject* GetParentObject() const;

  JSObject* WrapObject(JSContext* aCx,
                       JS::Handle<JSObject*> aGivenProto) override;

  already_AddRefed<ReadableStream> Readable() const;

  uint64_t DiscardedFrames() const { return mDiscardedFrames; }
  uint64_t TotalFrames() const { return mTotalFrames; }

 private:
  ~MediaStreamTrackProcessor();

  nsCOMPtr<nsIGlobalObject> mGlobal;
  RefPtr<ReadableStream> mReadable;

  uint64_t mDiscardedFrames = 0;
  uint64_t mTotalFrames = 0;
};

}  // namespace dom
}  // namespace mozilla

#endif  // DOM_MEDIA_MEDIASTREAMTRACKPROCESSOR_H_
