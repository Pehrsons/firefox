/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "MediaStreamTrackSourceHandle.h"

#include "MediaStreamTrack.h"
#include "MediaTrackGraph.h"
#include "mozilla/Logging.h"
#include "nsProxyRelease.h"
#include "nsThreadUtils.h"

extern mozilla::LazyLogModule gMediaStreamTrackLog;
#define LOG(type, ...) \
  MOZ_LOG_FMT(gMediaStreamTrackLog, type, MOZ_LOG_EXPAND_ARGS __VA_ARGS__)

namespace mozilla::dom {

/**
 * The Sink registered with the source on the source's thread. It keeps the
 * source alive on behalf of the handle's users and forwards notifications to
 * the handle's listeners.
 */
class MediaStreamTrackSourceHandle::KeepAliveSink final
    : public MediaStreamTrackSource::Sink {
 public:
  explicit KeepAliveSink(MediaStreamTrackSourceHandle* aHandle)
      : mHandle(aHandle) {}

  bool KeepsSourceAlive() const override { return true; }
  bool Enabled() const override { return mHandle->AnyUserEnabled(); }
  void PrincipalChanged() override { mHandle->ForwardPrincipalChanged(); }
  void MutedChanged(bool aNewState) override {
    mHandle->ForwardMutedChanged(aNewState);
  }
  void ConstraintsChanged(const MediaTrackConstraints& aConstraints) override {
    mHandle->ForwardConstraintsChanged(aConstraints);
  }
  void OverrideEnded() override { mHandle->ForwardOverrideEnded(); }

 private:
  // Raw pointer is safe because the handle owns this sink and only deletes it
  // on the source thread, where all calls into this sink happen.
  MediaStreamTrackSourceHandle* const mHandle;
};

/* static */
already_AddRefed<MediaStreamTrackSourceHandle>
MediaStreamTrackSourceHandle::Create(MediaStreamTrackSource* aSource,
                                     mozilla::MediaTrack* aInputTrack) {
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(aSource);
  RefPtr<MediaStreamTrackSourceHandle> handle =
      new MediaStreamTrackSourceHandle(aSource, aInputTrack);
  handle->RegisterSink();
  return handle.forget();
}

MediaStreamTrackSourceHandle::MediaStreamTrackSourceHandle(
    MediaStreamTrackSource* aSource, mozilla::MediaTrack* aInputTrack)
    : mSource(aSource),
      mInputTrack(aInputTrack),
      mMutex("MediaStreamTrackSourceHandle::mMutex") {
  LOG(LogLevel::Debug, ("MediaStreamTrackSourceHandle {} created for source {}",
                        fmt::ptr(this), fmt::ptr(aSource)));
}

MediaStreamTrackSourceHandle::~MediaStreamTrackSourceHandle() {
  LOG(LogLevel::Debug,
      ("MediaStreamTrackSourceHandle {} destroyed", fmt::ptr(this)));
  if (IsOnSourceThread()) {
    UnregisterSink();
    return;
  }
  // mSink can only be deleted on the source thread, since the source holds a
  // WeakPtr to it. It is normally already gone since UnregisterSink() runs
  // when the last user is removed; this only happens if that dispatch failed.
  if (mSink) {
    (void)NS_DispatchToMainThread(NS_NewRunnableFunction(
        "MediaStreamTrackSourceHandle::~MediaStreamTrackSourceHandle",
        [sink = std::move(mSink)] {}));
  }
  NS_ReleaseOnMainThread("MediaStreamTrackSourceHandle::mSource",
                         mSource.forget());
}

bool MediaStreamTrackSourceHandle::IsOnSourceThread() const {
  return NS_IsMainThread();
}

MediaStreamTrackSource* MediaStreamTrackSourceHandle::Source() const {
  MOZ_ASSERT(IsOnSourceThread());
  return mSource;
}

mozilla::MediaTrack* MediaStreamTrackSourceHandle::InputTrack() const {
  MOZ_ASSERT(IsOnSourceThread());
  return mInputTrack;
}

void MediaStreamTrackSourceHandle::RegisterSink() {
  MOZ_ASSERT(IsOnSourceThread());
  MOZ_ASSERT(!mSink);
  mSink = MakeUnique<KeepAliveSink>(this);
  mSource->RegisterSink(mSink.get());
}

void MediaStreamTrackSourceHandle::UnregisterSink() {
  MOZ_ASSERT(IsOnSourceThread());
  if (!mSink) {
    return;
  }
  LOG(LogLevel::Debug,
      ("MediaStreamTrackSourceHandle {} unregistering from source {}",
       fmt::ptr(this), fmt::ptr(mSource.get())));
  mSource->UnregisterSink(mSink.get());
  mSink = nullptr;
}

bool MediaStreamTrackSourceHandle::AnyEnabledLocked() const {
  if (mEnabledUsers > 0) {
    return true;
  }
  for (const ListenerEntry& entry : mListeners) {
    if (entry.mEnabled) {
      return true;
    }
  }
  return false;
}

bool MediaStreamTrackSourceHandle::AnyUserEnabled() const {
  MutexAutoLock lock(mMutex);
  return AnyEnabledLocked();
}

void MediaStreamTrackSourceHandle::AddUser(bool aEnabled) {
  bool enabledBefore;
  bool enabledAfter;
  {
    MutexAutoLock lock(mMutex);
    enabledBefore = AnyEnabledLocked();
    ++mUsers;
    if (aEnabled) {
      ++mEnabledUsers;
    }
    enabledAfter = AnyEnabledLocked();
  }
  if (enabledBefore != enabledAfter) {
    NotifySourceEnabledStateChanged();
  }
}

void MediaStreamTrackSourceHandle::RemoveUser(bool aEnabled) {
  {
    MutexAutoLock lock(mMutex);
    MOZ_ASSERT(mUsers > 0);
    --mUsers;
    if (aEnabled) {
      MOZ_ASSERT(mEnabledUsers > 0);
      --mEnabledUsers;
    }
  }
  OnUserRemoved(aEnabled);
}

void MediaStreamTrackSourceHandle::AddListener(Listener* aListener,
                                               nsISerialEventTarget* aTarget,
                                               bool aEnabled) {
  MOZ_ASSERT(aListener);
  MOZ_ASSERT(aTarget);
  bool enabledBefore;
  bool enabledAfter;
  {
    MutexAutoLock lock(mMutex);
    enabledBefore = AnyEnabledLocked();
    mListeners.AppendElement(ListenerEntry{aListener, aTarget, aEnabled});
    enabledAfter = AnyEnabledLocked();
  }
  if (enabledBefore != enabledAfter) {
    NotifySourceEnabledStateChanged();
  }
}

void MediaStreamTrackSourceHandle::RemoveListener(Listener* aListener) {
  bool wasEnabled = false;
  {
    MutexAutoLock lock(mMutex);
    for (size_t i = 0; i < mListeners.Length(); ++i) {
      if (mListeners[i].mListener == aListener) {
        wasEnabled = mListeners[i].mEnabled;
        mListeners.RemoveElementAt(i);
        break;
      }
    }
  }
  OnUserRemoved(wasEnabled);
}

void MediaStreamTrackSourceHandle::SetListenerEnabled(Listener* aListener,
                                                      bool aEnabled) {
  bool enabledBefore;
  bool enabledAfter;
  {
    MutexAutoLock lock(mMutex);
    enabledBefore = AnyEnabledLocked();
    for (ListenerEntry& entry : mListeners) {
      if (entry.mListener == aListener) {
        entry.mEnabled = aEnabled;
        break;
      }
    }
    enabledAfter = AnyEnabledLocked();
  }
  if (enabledBefore != enabledAfter) {
    NotifySourceEnabledStateChanged();
  }
}

void MediaStreamTrackSourceHandle::OnUserRemoved(bool aWasEnabled) {
  bool lastUser;
  bool enabledChanged;
  {
    MutexAutoLock lock(mMutex);
    lastUser = mUsers == 0 && mListeners.IsEmpty();
    enabledChanged = aWasEnabled && !AnyEnabledLocked();
  }
  if (lastUser) {
    // Unregistering may stop the source, so the enabled state is moot.
    if (IsOnSourceThread()) {
      UnregisterSink();
      return;
    }
    (void)NS_DispatchToMainThread(NS_NewRunnableFunction(
        "MediaStreamTrackSourceHandle::UnregisterSink",
        [self = RefPtr(this)] { self->UnregisterSink(); }));
    return;
  }
  if (enabledChanged) {
    NotifySourceEnabledStateChanged();
  }
}

void MediaStreamTrackSourceHandle::NotifySourceEnabledStateChanged() {
  if (IsOnSourceThread()) {
    if (mSink) {
      mSource->SinkEnabledStateChanged();
    }
    return;
  }
  (void)NS_DispatchToMainThread(NS_NewRunnableFunction(
      "MediaStreamTrackSourceHandle::NotifySourceEnabledStateChanged",
      [self = RefPtr(this)] {
        if (self->mSink) {
          self->mSource->SinkEnabledStateChanged();
        }
      }));
}

template <typename Function>
void MediaStreamTrackSourceHandle::ForwardToListeners(const char* aName,
                                                      Function&& aFunction) {
  MOZ_ASSERT(IsOnSourceThread());
  MutexAutoLock lock(mMutex);
  for (const ListenerEntry& entry : mListeners) {
    // A failed dispatch means the listener's thread is gone, which is fine.
    (void)entry.mTarget->Dispatch(
        NS_NewRunnableFunction(
            aName, [listener = entry.mListener,
                    function = aFunction] { function(listener.get()); }),
        NS_DISPATCH_FALLIBLE);
  }
}

void MediaStreamTrackSourceHandle::ForwardPrincipalChanged() {
  MOZ_ASSERT(IsOnSourceThread());
  ForwardToListeners("MediaStreamTrackSourceHandle::ForwardPrincipalChanged",
                     [principalHandle = MakePrincipalHandle(
                          mSource->GetPrincipal())](Listener* aListener) {
                       aListener->PrincipalChanged(principalHandle);
                     });
}

void MediaStreamTrackSourceHandle::ForwardMutedChanged(bool aNewState) {
  MOZ_ASSERT(IsOnSourceThread());
  ForwardToListeners(
      "MediaStreamTrackSourceHandle::ForwardMutedChanged",
      [aNewState](Listener* aListener) { aListener->MutedChanged(aNewState); });
}

void MediaStreamTrackSourceHandle::ForwardConstraintsChanged(
    const MediaTrackConstraints& aConstraints) {
  MOZ_ASSERT(IsOnSourceThread());
  // WebIDL dictionaries have explicit copy constructors.
  ForwardToListeners(
      "MediaStreamTrackSourceHandle::ForwardConstraintsChanged",
      [constraints = MediaTrackConstraints(aConstraints)](Listener* aListener) {
        aListener->ConstraintsChanged(constraints);
      });
}

void MediaStreamTrackSourceHandle::ForwardOverrideEnded() {
  MOZ_ASSERT(IsOnSourceThread());
  ForwardToListeners("MediaStreamTrackSourceHandle::ForwardOverrideEnded",
                     [](Listener* aListener) { aListener->OverrideEnded(); });
}

}  // namespace mozilla::dom

#undef LOG
