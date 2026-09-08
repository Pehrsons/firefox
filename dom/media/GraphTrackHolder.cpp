/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "GraphTrackHolder.h"

#include "MediaStreamTrackSourceHandle.h"
#include "MediaTrackGraph.h"
#include "mozilla/AbstractThread.h"
#include "mozilla/Logging.h"
#include "nsThreadUtils.h"

extern mozilla::LazyLogModule gMediaStreamTrackLog;
#define LOG(type, ...) \
  MOZ_LOG_FMT(gMediaStreamTrackLog, type, MOZ_LOG_EXPAND_ARGS __VA_ARGS__)

namespace mozilla::dom {

/* static */
already_AddRefed<GraphTrackHolder> GraphTrackHolder::Create(
    mozilla::MediaTrack* aInputTrack, MediaTrackGraph* aGraph, bool aEnabled) {
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT_IF(aInputTrack, aGraph);
  RefPtr<GraphTrackHolder> holder =
      new GraphTrackHolder(aInputTrack, aGraph, aEnabled);
  return holder.forget();
}

GraphTrackHolder::GraphTrackHolder(mozilla::MediaTrack* aInputTrack,
                                   MediaTrackGraph* aGraph, bool aEnabled)
    : mInputTrack(aInputTrack),
      mWatchManager(this, AbstractThread::MainThread()),
      mTrackEnded(AbstractThread::MainThread(), false,
                  "GraphTrackHolder::mTrackEnded"),
      mEnabled(aEnabled) {
  if (!mInputTrack) {
    LOG(LogLevel::Info, ("GraphTrackHolder {} created ended", fmt::ptr(this)));
    mEnded = true;
    return;
  }

  // Even if the input track is destroyed we need a graph track so that methods
  // like MediaStreamTrack::AddListener still work. Keeping the number of paths
  // to a minimum also helps prevent bugs elsewhere. We'll be ended through the
  // source soon enough.
  mTrack = aGraph->CreateForwardedInputTrack(
      mInputTrack->mType, mozilla::MediaTrack::Flag::EnableCanonicals);
  mPort = mTrack->AllocateInputPort(mInputTrack);
  mTrackEnded.Connect(&mTrack->CanonicalEnded());
  mWatchManager.Watch(mTrackEnded, &GraphTrackHolder::OnTrackEnded);
  LOG(LogLevel::Info, ("GraphTrackHolder {} created with graph track {}",
                       fmt::ptr(this), fmt::ptr(mTrack.get())));
}

GraphTrackHolder::~GraphTrackHolder() {
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(mEnded && !mTrack, "The owner must Shutdown() the holder");
}

void GraphTrackHolder::Attach(MediaStreamTrackSourceHandle* aHandle,
                              MediaStreamTrackSource* aSource) {
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(aHandle);
  MOZ_ASSERT(aSource);
  MOZ_ASSERT(!mAttached);
  mAttached = true;
  mHandle = RefPtr(aHandle);
  mSource = aSource;
  LOG(LogLevel::Info,
      ("GraphTrackHolder {} attached to handle {} for source {}",
       fmt::ptr(this), fmt::ptr(aHandle), fmt::ptr(aSource)));
  if (mEnded) {
    return;
  }
  mSource->RegisterSink(this);
  mTrack->SetDisabledTrackMode(mEnabled ? DisabledTrackMode::ENABLED
                                        : DisabledTrackMode::SILENCE_BLACK);
  mSource->SinkEnabledStateChanged();
}

already_AddRefed<GraphTrackHolder> GraphTrackHolder::Clone(
    bool aEnabled, RefPtr<MediaStreamTrackSource>* aSource) const {
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(mAttached);
  MOZ_ASSERT(aSource);
  if (mEnded || mInputTrack->IsDestroyed()) {
    // The producer has stopped and the source will not notify a sink
    // registering now, so the clone starts out ended.
    *aSource = mSource;
    return Create(nullptr, nullptr, aEnabled);
  }
  MediaStreamTrackSource::CloneResult cloneRes = mSource->Clone();
  if (!cloneRes.mSource) {
    // The source does not support independent clones. Share it.
    cloneRes.mSource = mSource;
    cloneRes.mInputTrack = mInputTrack;
  }
  *aSource = cloneRes.mSource;
  return Create(cloneRes.mInputTrack, mTrack->Graph(), aEnabled);
}

void GraphTrackHolder::SetEnabled(bool aEnabled) {
  MOZ_ASSERT(NS_IsMainThread());
  if (mEnabled == aEnabled) {
    return;
  }
  mEnabled = aEnabled;
  if (mEnded || !mAttached) {
    return;
  }
  mTrack->SetDisabledTrackMode(mEnabled ? DisabledTrackMode::ENABLED
                                        : DisabledTrackMode::SILENCE_BLACK);
  mSource->SinkEnabledStateChanged();
}

void GraphTrackHolder::MutedChanged(bool aNewState) {
  MOZ_ASSERT(NS_IsMainThread());
  if (RefPtr<MediaStreamTrackSourceHandle> handle(mHandle); handle) {
    handle->HolderMutedChanged(aNewState);
  }
}

void GraphTrackHolder::OverrideEnded() {
  MOZ_ASSERT(NS_IsMainThread());
  End();
}

void GraphTrackHolder::OnTrackEnded() {
  MOZ_ASSERT(NS_IsMainThread());
  if (!mTrackEnded) {
    // The mirror was seeded with the track's initial state.
    return;
  }
  End();
}

void GraphTrackHolder::Shutdown() {
  MOZ_ASSERT(NS_IsMainThread());
  End();
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

void GraphTrackHolder::End() {
  MOZ_ASSERT(NS_IsMainThread());
  if (mEnded) {
    return;
  }
  mEnded = true;
  LOG(LogLevel::Info, ("GraphTrackHolder {} ended", fmt::ptr(this)));
  if (mAttached) {
    mSource->UnregisterSink(this);
  }
  mWatchManager.Shutdown();
  mTrackEnded.DisconnectIfConnected();
  if (RefPtr<MediaStreamTrackSourceHandle> handle(mHandle); handle) {
    handle->HolderEnded();
  }
}

}  // namespace mozilla::dom

#undef LOG
