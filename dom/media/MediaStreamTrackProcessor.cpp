/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "MediaStreamTrackProcessor.h"

#include "mozilla/dom/MediaStreamTrackProcessorBinding.h"
#include "mozilla/dom/ReadableStream.h"
#include "nsIGlobalObject.h"

namespace mozilla::dom {

NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE(MediaStreamTrackProcessor, mGlobal,
                                      mReadable)

NS_IMPL_CYCLE_COLLECTING_ADDREF(MediaStreamTrackProcessor)
NS_IMPL_CYCLE_COLLECTING_RELEASE(MediaStreamTrackProcessor)

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(MediaStreamTrackProcessor)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END

MediaStreamTrackProcessor::MediaStreamTrackProcessor(nsIGlobalObject* aGlobal)
    : mGlobal(aGlobal) {}

MediaStreamTrackProcessor::~MediaStreamTrackProcessor() = default;

/* static */
already_AddRefed<MediaStreamTrackProcessor>
MediaStreamTrackProcessor::Constructor(
    const GlobalObject& aGlobal, const MediaStreamTrackProcessorInit& aInit,
    ErrorResult& aRv) {
  nsCOMPtr<nsIGlobalObject> global = do_QueryInterface(aGlobal.GetAsSupports());
  MOZ_ASSERT(global);

  RefPtr<MediaStreamTrackProcessor> processor =
      new MediaStreamTrackProcessor(global);
  return processor.forget();
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

}  // namespace mozilla::dom
