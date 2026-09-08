/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "GraphTrackHolder.h"

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
    Owner* aOwner, MediaStreamTrackSource* aSource,
    mozilla::MediaTrack* aInputTrack, bool aEnabled) {
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(aOwner);
  MOZ_ASSERT(aSource);
  RefPtr<GraphTrackHolder> holder =
      new GraphTrackHolder(aOwner, aSource, aInputTrack, aEnabled);
  return holder.forget();
}

GraphTrackHolder::GraphTrackHolder(Owner* aOwner,
                                   MediaStreamTrackSource* aSource,
                                   mozilla::MediaTrack* aInputTrack,
                                   bool aEnabled)
    : mOwner(aOwner),
      mSource(aSource),
      mInputTrack(aInputTrack),
      mWatchManager(this, AbstractThread::MainThread()),
      mTrackEnded(AbstractThread::MainThread(), false,
                  "GraphTrackHolder::mTrackEnded"),
      mEnabled(aEnabled) {
  if (!mInputTrack || mInputTrack->IsDestroyed()) {
    // The producer has already stopped, or this is the clone of an ended
    // holder. The owner is notified of the ended state by whoever created us.
    LOG(LogLevel::Info, ("GraphTrackHolder {} created ended for source {}",
                         fmt::ptr(this), fmt::ptr(aSource)));
    mEnded = true;
    return;
  }

  mSource->RegisterSink(this);
  mTrack = mInputTrack->Graph()->CreateForwardedInputTrack(
      mInputTrack->mType, mozilla::MediaTrack::Flag::EnableCanonicals);
  mPort = mTrack->AllocateInputPort(mInputTrack);
  mTrackEnded.Connect(&mTrack->CanonicalEnded());
  mWatchManager.Watch(mTrackEnded, &GraphTrackHolder::OnTrackEnded);
  mTrack->SetDisabledTrackMode(mEnabled ? DisabledTrackMode::ENABLED
                                        : DisabledTrackMode::SILENCE_BLACK);
  mSource->SinkEnabledStateChanged();
  LOG(LogLevel::Info,
      ("GraphTrackHolder {} created for source {} with graph track {}",
       fmt::ptr(this), fmt::ptr(aSource), fmt::ptr(mTrack.get())));
}

GraphTrackHolder::~GraphTrackHolder() {
  MOZ_ASSERT(NS_IsMainThread());
  Shutdown();
  LOG(LogLevel::Info, ("GraphTrackHolder {} destroying graph track {}",
                       fmt::ptr(this), fmt::ptr(mTrack.get())));
  if (mPort) {
    mPort->Destroy();
  }
  if (mTrack) {
    mTrack->Destroy();
  }
}

already_AddRefed<GraphTrackHolder> GraphTrackHolder::Clone(
    Owner* aOwner, bool aEnabled) const {
  MOZ_ASSERT(NS_IsMainThread());
  if (mEnded) {
    return Create(aOwner, mSource, nullptr, aEnabled);
  }
  MediaStreamTrackSource::CloneResult cloneRes = mSource->Clone();
  if (!cloneRes.mSource) {
    // The source does not support independent clones. Share it.
    cloneRes.mSource = mSource;
    cloneRes.mInputTrack = mInputTrack;
  }
  return Create(aOwner, cloneRes.mSource, cloneRes.mInputTrack, aEnabled);
}

void GraphTrackHolder::SetEnabled(bool aEnabled) {
  MOZ_ASSERT(NS_IsMainThread());
  if (mEnabled == aEnabled) {
    return;
  }
  mEnabled = aEnabled;
  if (mEnded) {
    return;
  }
  mTrack->SetDisabledTrackMode(mEnabled ? DisabledTrackMode::ENABLED
                                        : DisabledTrackMode::SILENCE_BLACK);
  mSource->SinkEnabledStateChanged();
}

void GraphTrackHolder::MutedChanged(bool aNewState) {
  MOZ_ASSERT(NS_IsMainThread());
  if (mOwner) {
    mOwner->HolderMutedChanged(aNewState);
  }
}

void GraphTrackHolder::ConstraintsChanged(
    const MediaTrackConstraints& aConstraints) {
  MOZ_ASSERT(NS_IsMainThread());
  MediaTrackSettings settings;
  mSource->GetSettings(settings);
  if (mOwner) {
    mOwner->HolderConstraintsChanged(aConstraints, settings);
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
  mOwner = nullptr;
  End();
}

void GraphTrackHolder::End() {
  MOZ_ASSERT(NS_IsMainThread());
  if (mEnded) {
    return;
  }
  mEnded = true;
  LOG(LogLevel::Info, ("GraphTrackHolder {} ended", fmt::ptr(this)));
  mSource->UnregisterSink(this);
  mWatchManager.Shutdown();
  mTrackEnded.DisconnectIfConnected();
  if (mOwner) {
    mOwner->HolderEnded();
  }
}

}  // namespace mozilla::dom

#undef LOG
