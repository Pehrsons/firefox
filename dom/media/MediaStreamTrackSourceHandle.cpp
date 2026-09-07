/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "MediaStreamTrackSourceHandle.h"

#include "MediaStreamTrack.h"
#include "MediaTrackGraph.h"
#include "mozilla/AbstractThread.h"
#include "mozilla/Logging.h"
#include "nsThreadUtils.h"

extern mozilla::LazyLogModule gMediaStreamTrackLog;
#define LOG(type, ...) \
  MOZ_LOG_FMT(gMediaStreamTrackLog, type, MOZ_LOG_EXPAND_ARGS __VA_ARGS__)

namespace mozilla::dom {

/* static */
already_AddRefed<MediaStreamTrackSourceHandle>
MediaStreamTrackSourceHandle::Create(already_AddRefed<GraphTrackHolder> aHolder,
                                     MediaStreamTrackSource* aSource,
                                     bool aEnabled, bool aMuted) {
  MOZ_ASSERT(NS_IsMainThread());
  RefPtr<MediaStreamTrackSourceHandle> handle =
      new MediaStreamTrackSourceHandle(aEnabled, aMuted);
  handle->SetHolder(std::move(aHolder), aSource);
  return handle.forget();
}

MediaStreamTrackSourceHandle::MediaStreamTrackSourceHandle(bool aEnabled,
                                                           bool aMuted)
    : mMuted(AbstractThread::MainThread(), aMuted,
             "MediaStreamTrackSourceHandle::mMuted"),
      mEnded(AbstractThread::MainThread(), false,
             "MediaStreamTrackSourceHandle::mEnded"),
      mHolderState(AbstractThread::MainThread(), HolderState(),
                   "MediaStreamTrackSourceHandle::mHolderState"),
      mEnabled(aEnabled) {
  LOG(LogLevel::Debug,
      ("MediaStreamTrackSourceHandle {} created", fmt::ptr(this)));
}

MediaStreamTrackSourceHandle::~MediaStreamTrackSourceHandle() {
  LOG(LogLevel::Debug,
      ("MediaStreamTrackSourceHandle {} destroyed", fmt::ptr(this)));
  // Nothing depends on the holder anymore, so it can be shut down and
  // destroyed. Only on the main thread though.
  if (NS_IsMainThread()) {
    mHolder->Shutdown();
    return;
  }
  DispatchToMainThread(
      "MediaStreamTrackSourceHandle::~MediaStreamTrackSourceHandle",
      [holder = std::move(mHolder)] { holder->Shutdown(); });
}

GraphTrackHolder* MediaStreamTrackSourceHandle::Holder() const {
  MOZ_ASSERT(NS_IsMainThread());
  return mHolder.get();
}

template <typename Function>
/* static */
void MediaStreamTrackSourceHandle::DispatchToMainThread(const char* aName,
                                                        Function&& aFunction) {
  // A failed dispatch only happens during shutdown.
  (void)NS_DispatchToMainThread(
      NS_NewRunnableFunction(aName, std::forward<Function>(aFunction)));
}

void MediaStreamTrackSourceHandle::SetHolder(
    already_AddRefed<GraphTrackHolder> aHolder,
    MediaStreamTrackSource* aSource) {
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(!mHolder);
  mHolder = aHolder;
  MOZ_ASSERT(mHolder);
  LOG(LogLevel::Debug, ("MediaStreamTrackSourceHandle {} owns holder {}",
                        fmt::ptr(this), fmt::ptr(mHolder.get())));

  mHolder->Attach(this, aSource);
  if (mHolder->Ended()) {
    HolderEnded();
    return;
  }

  mHolder->SetEnabled(mEnabled);

  MediaStreamTrackSource& source = mHolder->Source();
  mHolderState = HolderState{.mSettings = &source.CanonicalSettings(),
                             .mCapabilities = &source.CanonicalCapabilities()};
}

void MediaStreamTrackSourceHandle::SetEnabled(bool aEnabled) {
  MOZ_ASSERT(!NS_IsMainThread());
  AbstractThread::MainThread()->Dispatch(NS_NewRunnableFunction(
      "MediaStreamTrackSourceHandle::ApplyEnabled",
      [self = RefPtr(this), aEnabled] { self->ApplyEnabled(aEnabled); }));
}

void MediaStreamTrackSourceHandle::ApplyEnabled(bool aEnabled) {
  MOZ_ASSERT(NS_IsMainThread());
  mEnabled = aEnabled;
  mHolder->SetEnabled(mEnabled);
}

void MediaStreamTrackSourceHandle::HolderEnded() {
  MOZ_ASSERT(NS_IsMainThread());
  if (mEnded) {
    return;
  }
  LOG(LogLevel::Info,
      ("MediaStreamTrackSourceHandle {} holder ended", fmt::ptr(this)));
  mEnded = true;
}

void MediaStreamTrackSourceHandle::HolderMutedChanged(bool aMuted) {
  MOZ_ASSERT(NS_IsMainThread());
  mMuted = aMuted;
}

}  // namespace mozilla::dom

#undef LOG
