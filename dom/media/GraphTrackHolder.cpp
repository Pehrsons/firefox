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

// aPort is null for a graph track without input.
static void DestroyGraphObjects(ProcessedMediaTrack* aTrack,
                                MediaInputPort* aPort) {
  MOZ_ASSERT(NS_IsMainThread());
  LOG(LogLevel::Info,
      ("GraphTrackHolder destroying graph track {}", fmt::ptr(aTrack)));
  if (aPort) {
    aPort->Destroy();
  }
  aTrack->Destroy();
}

/* static */
already_AddRefed<GraphTrackHolder> GraphTrackHolder::Create(
    mozilla::MediaTrack* aInputTrack, MediaTrackGraph* aGraph, bool aEnabled) {
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT_IF(aInputTrack, aGraph);
  RefPtr<GraphTrackHolder> holder =
      new GraphTrackHolder(aInputTrack, aGraph, aEnabled, /* aMuted = */ false,
                           /* aPending = */ false);
  return holder.forget();
}

/* static */
already_AddRefed<GraphTrackHolder> GraphTrackHolder::CreateClone(
    GraphTrackHolder* aOriginal, bool aEnabled, bool aMuted) {
  MOZ_ASSERT(aOriginal);
  RefPtr<GraphTrackHolder> holder = new GraphTrackHolder(
      nullptr, nullptr, aEnabled, aMuted, /* aPending = */ true);
  LOG(LogLevel::Debug, ("GraphTrackHolder {} created as pending clone of {}",
                        fmt::ptr(holder.get()), fmt::ptr(aOriginal)));
  DispatchToMainThread("GraphTrackHolder::InitializeClone",
                       [holder, original = RefPtr(aOriginal)] {
                         holder->InitializeClone(original);
                       });
  return holder.forget();
}

GraphTrackHolder::GraphTrackHolder(mozilla::MediaTrack* aInputTrack,
                                   MediaTrackGraph* aGraph, bool aEnabled,
                                   bool aMuted, bool aPending)
    : mWatchManager(this, AbstractThread::MainThread()),
      mTrackEnded(AbstractThread::MainThread(), false,
                  "GraphTrackHolder::mTrackEnded"),
      mMuted(AbstractThread::MainThread(), aMuted, "GraphTrackHolder::mMuted"),
      mEnded(AbstractThread::MainThread(), !aInputTrack && !aPending,
             "GraphTrackHolder::mEnded"),
      mHolderState(AbstractThread::MainThread(), HolderState(),
                   "GraphTrackHolder::mHolderState"),
      mEnabled(aEnabled) {
  if (aPending) {
    return;
  }
  if (!aInputTrack) {
    LOG(LogLevel::Info, ("GraphTrackHolder {} created ended", fmt::ptr(this)));
    return;
  }
  InitGraphTrack(aInputTrack->mType, aGraph, aInputTrack);
}

void GraphTrackHolder::InitGraphTrack(MediaSegment::Type aType,
                                      MediaTrackGraph* aGraph,
                                      mozilla::MediaTrack* aInputTrack) {
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(!mTrack);
  // Even if the input track is destroyed we need a graph track so that methods
  // like MediaStreamTrack::AddListener still work. Keeping the number of paths
  // to a minimum also helps prevent bugs elsewhere. We'll be ended through the
  // source soon enough.
  mInputTrack = aInputTrack;
  mTrack = aGraph->CreateForwardedInputTrack(
      aType, mozilla::MediaTrack::Flag::EnableCanonicals);
  if (mInputTrack) {
    mPort = mTrack->AllocateInputPort(mInputTrack);
  }
  mTrackEnded.Connect(&mTrack->CanonicalEnded());
  mWatchManager.Watch(mTrackEnded, &GraphTrackHolder::OnTrackEnded);
  LOG(LogLevel::Info, ("GraphTrackHolder {} created with graph track {}",
                       fmt::ptr(this), fmt::ptr(mTrack.get())));
}

void GraphTrackHolder::InitializeClone(GraphTrackHolder* aOriginal) {
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(!mSource && !mTrack);
  if (!aOriginal->mSource || !aOriginal->mTrack) {
    // The original has not been initialized or has no graph track, so the clone
    // starts out ended.
    LOG(LogLevel::Info, ("GraphTrackHolder {} cannot clone {}: it has no "
                         "graph track",
                         fmt::ptr(this), fmt::ptr(aOriginal)));
    End();
    mSource = aOriginal->mSource;
    return;
  }
  MOZ_ASSERT(!aOriginal->mTrack->IsDestroyed());
  if (aOriginal->mEnded.Ref() || aOriginal->mInputTrack->IsDestroyed()) {
    // The original's producer has stopped, and the source will not notify a
    // sink registering now. Our graph track gets no input so that it ends,
    // like a track whose source ended while it was in flight, and we do not
    // register as a sink.
    LOG(LogLevel::Info,
        ("GraphTrackHolder {} cloning {}: it is ended, so we end too",
         fmt::ptr(this), fmt::ptr(aOriginal)));
    InitGraphTrack(aOriginal->mTrack->mType, aOriginal->mTrack->Graph(),
                   nullptr);
    mSource = aOriginal->mSource;
    mHolderState =
        HolderState{.mGraphTrack = mTrack,
                    .mSettings = &mSource->CanonicalSettings(),
                    .mCapabilities = &mSource->CanonicalCapabilities()};
    return;
  }

  LOG(LogLevel::Debug, ("GraphTrackHolder {} cloning holder {}", fmt::ptr(this),
                        fmt::ptr(aOriginal)));
  MediaStreamTrackSource::CloneResult cloneRes = aOriginal->mSource->Clone();
  if (!cloneRes.mSource) {
    // The source does not support independent clones. Share it.
    cloneRes.mSource = aOriginal->mSource;
    cloneRes.mInputTrack = aOriginal->mInputTrack;
  }
  InitGraphTrack(cloneRes.mInputTrack->mType, aOriginal->mTrack->Graph(),
                 cloneRes.mInputTrack);
  Attach(cloneRes.mSource, mMuted.Ref());
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
  mTrack->SetDisabledTrackMode(mEnabled ? DisabledTrackMode::ENABLED
                                        : DisabledTrackMode::SILENCE_BLACK);
  mHolderState =
      HolderState{.mGraphTrack = mTrack,
                  .mSettings = &mSource->CanonicalSettings(),
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
  mTrack->SetDisabledTrackMode(mEnabled ? DisabledTrackMode::ENABLED
                                        : DisabledTrackMode::SILENCE_BLACK);
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

void GraphTrackHolder::SetFence(TransferFencePromise* aFence) {
  MOZ_ASSERT(aFence);
  if (!NS_IsMainThread()) {
    DispatchToMainThread("GraphTrackHolder::SetFence",
                         [self = RefPtr(this), fence = RefPtr(aFence)] {
                           self->SetFence(fence);
                         });
    return;
  }
  MOZ_ASSERT(mTrack, "A holder is not shut down while its track is live");
  mFenceRequest.DisconnectIfExists();
  mFence = aFence;
  // Keeps us alive until the fence has resolved.
  mFence
      ->Then(AbstractThread::MainThread(), __func__, this,
             &GraphTrackHolder::OnFenceResolved)
      ->Track(mFenceRequest);
}

void GraphTrackHolder::OnFenceResolved(
    const TransferFencePromise::ResolveOrRejectValue&) {
  MOZ_ASSERT(NS_IsMainThread());
  mFenceRequest.Complete();
  mFence = nullptr;
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
  RefPtr<ProcessedMediaTrack> track = std::move(mTrack);
  RefPtr<MediaInputPort> port = std::move(mPort);
  if (!mFence) {
    DestroyGraphObjects(track, port);
    return;
  }
  LOG(LogLevel::Info, ("GraphTrackHolder {} waiting for its fence to destroy "
                       "graph track {}",
                       fmt::ptr(this), fmt::ptr(track.get())));
  mFence->Then(
      AbstractThread::MainThread(), __func__,
      [track, port](const TransferFencePromise::ResolveOrRejectValue&) {
        DestroyGraphObjects(track, port);
      });
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
