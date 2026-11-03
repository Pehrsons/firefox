/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "GraphTrackHolder.h"

#include "MediaTrackGraph.h"
#include "mozilla/AbstractThread.h"
#include "mozilla/AppShutdown.h"
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
      mMuted(AbstractThread::MainThread(), false, "GraphTrackHolder::mMuted"),
      mEnded(AbstractThread::MainThread(), !aInputTrack,
             "GraphTrackHolder::mEnded"),
      mHolderState(AbstractThread::MainThread(), HolderState(),
                   "GraphTrackHolder::mHolderState"),
      mEnabled(aEnabled) {
  if (!mInputTrack) {
    LOG(LogLevel::Info, ("GraphTrackHolder {} created ended", fmt::ptr(this)));
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
  LOG(LogLevel::Debug, ("GraphTrackHolder {} destroyed", fmt::ptr(this)));
  Shutdown();
}

template <typename Function>
/* static */
void GraphTrackHolder::DispatchToMainThread(const char* aName,
                                            Function&& aFunction) {
  // A failed dispatch only happens during shutdown.
  (void)NS_DispatchToMainThread(
      NS_NewRunnableFunction(aName, std::forward<Function>(aFunction)));
}

void GraphTrackHolder::Attach(MediaStreamTrackSource* aSource, bool aMuted) {
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(aSource);
  MOZ_ASSERT(!mSource);
  mSource = aSource;
  LOG(LogLevel::Info, ("GraphTrackHolder {} attached to source {}",
                       fmt::ptr(this), fmt::ptr(aSource)));
  mMuted = aMuted;
  if (mEnded.Ref()) {
    return;
  }
  mSource->RegisterSink(this);
  mHolderState =
      HolderState{.mSettings = &mSource->CanonicalSettings(),
                  .mCapabilities = &mSource->CanonicalCapabilities()};
  mSource->SinkEnabledStateChanged();
}

void GraphTrackHolder::Unattach() {
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(mSource);
  LOG(LogLevel::Info, ("GraphTrackHolder {} unattached from source {}",
                       fmt::ptr(this), fmt::ptr(mSource.get())));
  RefPtr<MediaStreamTrackSource> source = std::move(mSource);
  mHolderState = HolderState();
  if (!mEnded.Ref()) {
    source->UnregisterSink(this);
  }
}

void GraphTrackHolder::SetEnabled(bool aEnabled) {
  MOZ_ASSERT(NS_IsMainThread());
  if (mEnabled == aEnabled) {
    return;
  }
  mEnabled = aEnabled;
  if (mEnded.Ref() || !mSource) {
    return;
  }
  mSource->SinkEnabledStateChanged();
}

void GraphTrackHolder::SetEnabledAsync(bool aEnabled) {
  MOZ_ASSERT(!NS_IsMainThread());
  DispatchToMainThread("GraphTrackHolder::SetEnabled",
                       [self = RefPtr(this), aEnabled] {
                         if (self->mSource) {
                           self->SetEnabled(aEnabled);
                         }
                       });
}

void GraphTrackHolder::ShutdownAsync() {
  MOZ_ASSERT(!NS_IsMainThread());
  DispatchToMainThread("GraphTrackHolder::Shutdown",
                       [self = RefPtr(this)] { self->Shutdown(); });
}

void GraphTrackHolder::MutedChanged(bool aNewState) {
  MOZ_ASSERT(NS_IsMainThread());
  mMuted = aNewState;
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
  mInputTrack = nullptr;
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
  if (mEnded.Ref()) {
    return;
  }
  LOG(LogLevel::Info, ("GraphTrackHolder {} ended", fmt::ptr(this)));
  // The final shutdown cycle collection runs after
  // AbstractThread::ShutdownMainThread(), and nothing mirrors the holder's
  // Canonicals by then.
  MOZ_DIAGNOSTIC_ASSERT_IF(
      !AbstractThread::GetCurrent(),
      AppShutdown::IsInOrBeyond(ShutdownPhase::XPCOMShutdownFinal));
  if (AbstractThread::GetCurrent()) {
    mEnded = true;
  }
  if (mSource) {
    mSource->UnregisterSink(this);
  }
  mWatchManager.Shutdown();
  mTrackEnded.DisconnectIfConnected();
}

}  // namespace mozilla::dom

#undef LOG
