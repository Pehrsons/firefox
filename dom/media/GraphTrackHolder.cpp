/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "GraphTrackHolder.h"

#include "MediaTrackGraph.h"
#include "mozilla/Logging.h"

extern mozilla::LazyLogModule gMediaStreamTrackLog;
#define LOG(type, ...) \
  MOZ_LOG_FMT(gMediaStreamTrackLog, type, MOZ_LOG_EXPAND_ARGS __VA_ARGS__)

namespace mozilla::dom {

/* static */
already_AddRefed<GraphTrackHolder> GraphTrackHolder::Create(
    mozilla::MediaTrack* aInputTrack, MediaTrackGraph* aGraph) {
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(aInputTrack);
  MOZ_ASSERT(aGraph);
  RefPtr<GraphTrackHolder> holder = new GraphTrackHolder(aInputTrack, aGraph);
  return holder.forget();
}

GraphTrackHolder::GraphTrackHolder(mozilla::MediaTrack* aInputTrack,
                                   MediaTrackGraph* aGraph)
    : mInputTrack(aInputTrack) {
  // Even if the input track is destroyed we need a graph track so that methods
  // like MediaStreamTrack::AddListener still work. Keeping the number of paths
  // to a minimum also helps prevent bugs elsewhere. We'll be ended through the
  // source soon enough.
  mTrack = aGraph->CreateForwardedInputTrack(
      mInputTrack->mType, mozilla::MediaTrack::Flag::EnableCanonicals);
  mPort = mTrack->AllocateInputPort(mInputTrack);
  LOG(LogLevel::Info, ("GraphTrackHolder {} created with graph track {}",
                       fmt::ptr(this), fmt::ptr(mTrack.get())));
}

GraphTrackHolder::~GraphTrackHolder() {
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(!mTrack, "The owner must Shutdown() the holder");
}

void GraphTrackHolder::Shutdown() {
  MOZ_ASSERT(NS_IsMainThread());
  if (!mTrack) {
    return;
  }
  LOG(LogLevel::Info, ("GraphTrackHolder {} destroying graph track {}",
                       fmt::ptr(this), fmt::ptr(mTrack.get())));
  mPort->Destroy();
  mTrack->Destroy();
  mPort = nullptr;
  mTrack = nullptr;
}

}  // namespace mozilla::dom

#undef LOG
