/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "MediaStreamTrackSourceHandle.h"

#include "MediaStreamTrack.h"
#include "MediaTrackGraph.h"
#include "mozilla/Logging.h"
#include "nsThreadUtils.h"

extern mozilla::LazyLogModule gMediaStreamTrackLog;
#define LOG(type, ...) \
  MOZ_LOG_FMT(gMediaStreamTrackLog, type, MOZ_LOG_EXPAND_ARGS __VA_ARGS__)

namespace mozilla::dom {

/* static */
already_AddRefed<MediaStreamTrackSourceHandle>
MediaStreamTrackSourceHandle::Create(MediaStreamTrackSource* aSource,
                                     mozilla::MediaTrack* aInputTrack,
                                     bool aEnabled) {
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(aSource);
  RefPtr<MediaStreamTrackSourceHandle> handle =
      new MediaStreamTrackSourceHandle();
  handle->SetHolder(
      GraphTrackHolder::Create(handle.get(), aSource, aInputTrack, aEnabled));
  return handle.forget();
}

/* static */
already_AddRefed<MediaStreamTrackSourceHandle>
MediaStreamTrackSourceHandle::CreateClone(
    MediaStreamTrackSourceHandle* aOriginal, bool aEnabled) {
  MOZ_ASSERT(aOriginal);
  RefPtr<MediaStreamTrackSourceHandle> handle =
      new MediaStreamTrackSourceHandle();
  handle->AddUser(aEnabled);
  LOG(LogLevel::Debug,
      ("MediaStreamTrackSourceHandle {} created as pending clone of {}",
       fmt::ptr(handle.get()), fmt::ptr(aOriginal)));
  DispatchToMainThread("MediaStreamTrackSourceHandle::InitializeClone",
                       [handle, original = RefPtr(aOriginal)] {
                         handle->InitializeClone(original);
                       });
  return handle.forget();
}

MediaStreamTrackSourceHandle::MediaStreamTrackSourceHandle()
    : mMutex("MediaStreamTrackSourceHandle::mMutex") {
  LOG(LogLevel::Debug,
      ("MediaStreamTrackSourceHandle {} created", fmt::ptr(this)));
}

MediaStreamTrackSourceHandle::~MediaStreamTrackSourceHandle() {
  LOG(LogLevel::Debug,
      ("MediaStreamTrackSourceHandle {} destroyed", fmt::ptr(this)));
  if (IsOnSourceThread()) {
    ReleaseHolder();
    return;
  }
  // The holder can only be used and destroyed on the main thread. It is
  // normally already gone since ReleaseHolder() runs when the last user is
  // removed; it is only set here if that dispatch failed. The holder's owner
  // pointer must not be used from the runnable since we are gone.
  if (mHolder) {
    DispatchToMainThread(
        "MediaStreamTrackSourceHandle::~MediaStreamTrackSourceHandle",
        [holder = std::move(mHolder)] { holder->Shutdown(); });
  }
}

bool MediaStreamTrackSourceHandle::IsOnSourceThread() const {
  return NS_IsMainThread();
}

GraphTrackHolder* MediaStreamTrackSourceHandle::Holder() const {
  MOZ_ASSERT(IsOnSourceThread());
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
    already_AddRefed<GraphTrackHolder> aHolder) {
  MOZ_ASSERT(IsOnSourceThread());
  MOZ_ASSERT(!mHolder);
  mHolder = aHolder;
  MOZ_ASSERT(mHolder);
  LOG(LogLevel::Debug, ("MediaStreamTrackSourceHandle {} owns holder {}",
                        fmt::ptr(this), fmt::ptr(mHolder.get())));

  if (mReleased) {
    // All users went away while a clone was pending.
    ReleaseHolder();
    return;
  }

  if (mHolder->Ended()) {
    HolderEnded();
    return;
  }

  ApplyEnabled();

  RefPtr<ProcessedMediaTrack> graphTrack = mHolder->GraphTrack();
  MOZ_ASSERT(graphTrack);
  {
    MutexAutoLock lock(mMutex);
    mGraphTrack = graphTrack;
  }
  ForwardToListeners("MediaStreamTrackSourceHandle::GraphTrackAvailable",
                     [graphTrack](Listener* aListener) {
                       aListener->GraphTrackAvailable(graphTrack);
                     });
}

void MediaStreamTrackSourceHandle::InitializeClone(
    MediaStreamTrackSourceHandle* aOriginal) {
  MOZ_ASSERT(IsOnSourceThread());
  MOZ_ASSERT(!mHolder);

  GraphTrackHolder* originalHolder = aOriginal->Holder();
  if (!originalHolder) {
    LOG(LogLevel::Info,
        ("MediaStreamTrackSourceHandle {} cannot clone {}: its holder is gone "
         "or still pending",
         fmt::ptr(this), fmt::ptr(aOriginal)));
    HolderEnded();
    return;
  }

  LOG(LogLevel::Debug, ("MediaStreamTrackSourceHandle {} cloning holder {}",
                        fmt::ptr(this), fmt::ptr(originalHolder)));
  SetHolder(originalHolder->Clone(this, AnyUserEnabled()));
}

void MediaStreamTrackSourceHandle::ApplyEnabled() {
  MOZ_ASSERT(IsOnSourceThread());
  if (mHolder) {
    mHolder->SetEnabled(AnyUserEnabled());
  }
}

void MediaStreamTrackSourceHandle::ReleaseHolder() {
  MOZ_ASSERT(IsOnSourceThread());
  mReleased = true;
  if (!mHolder) {
    return;
  }
  LOG(LogLevel::Debug, ("MediaStreamTrackSourceHandle {} releasing holder {}",
                        fmt::ptr(this), fmt::ptr(mHolder.get())));
  {
    MutexAutoLock lock(mMutex);
    mGraphTrack = nullptr;
  }
  // No users depend on the graph track anymore, so it can be destroyed.
  mHolder->Shutdown();
  mHolder = nullptr;
}

void MediaStreamTrackSourceHandle::HolderEnded() {
  MOZ_ASSERT(IsOnSourceThread());
  {
    MutexAutoLock lock(mMutex);
    if (mEnded) {
      return;
    }
    mEnded = true;
  }
  LOG(LogLevel::Info,
      ("MediaStreamTrackSourceHandle {} holder ended", fmt::ptr(this)));
  ForwardToListeners("MediaStreamTrackSourceHandle::HolderEnded",
                     [](Listener* aListener) { aListener->OverrideEnded(); });
}

void MediaStreamTrackSourceHandle::HolderMutedChanged(bool aMuted) {
  MOZ_ASSERT(IsOnSourceThread());
  ForwardToListeners(
      "MediaStreamTrackSourceHandle::HolderMutedChanged",
      [aMuted](Listener* aListener) { aListener->MutedChanged(aMuted); });
}

void MediaStreamTrackSourceHandle::HolderConstraintsChanged(
    const MediaTrackConstraints& aConstraints,
    const MediaTrackSettings& aSettings) {
  MOZ_ASSERT(IsOnSourceThread());
  // WebIDL dictionaries have explicit copy constructors.
  ForwardToListeners(
      "MediaStreamTrackSourceHandle::HolderConstraintsChanged",
      [constraints = MediaTrackConstraints(aConstraints),
       settings = MediaTrackSettings(aSettings)](Listener* aListener) {
        aListener->ConstraintsChanged(constraints, settings);
      });
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
    NotifyEnabledStateChanged();
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
  bool ended;
  RefPtr<ProcessedMediaTrack> graphTrack;
  {
    MutexAutoLock lock(mMutex);
    enabledBefore = AnyEnabledLocked();
    mListeners.AppendElement(ListenerEntry{aListener, aTarget, aEnabled});
    enabledAfter = AnyEnabledLocked();
    ended = mEnded;
    graphTrack = mGraphTrack;
  }
  if (enabledBefore != enabledAfter) {
    NotifyEnabledStateChanged();
  }
  const ListenerEntry entry{aListener, aTarget, aEnabled};
  if (graphTrack) {
    DispatchToListener(entry,
                       "MediaStreamTrackSourceHandle::AddListener::GraphTrack",
                       [graphTrack](Listener* aListener) {
                         aListener->GraphTrackAvailable(graphTrack);
                       });
  }
  if (ended) {
    DispatchToListener(entry,
                       "MediaStreamTrackSourceHandle::AddListener::Ended",
                       [](Listener* aListener) { aListener->OverrideEnded(); });
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
    NotifyEnabledStateChanged();
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
    // Releasing destroys the holder, so the enabled state is moot.
    if (IsOnSourceThread()) {
      ReleaseHolder();
      return;
    }
    DispatchToMainThread("MediaStreamTrackSourceHandle::ReleaseHolder",
                         [self = RefPtr(this)] { self->ReleaseHolder(); });
    return;
  }
  if (enabledChanged) {
    NotifyEnabledStateChanged();
  }
}

void MediaStreamTrackSourceHandle::NotifyEnabledStateChanged() {
  if (IsOnSourceThread()) {
    ApplyEnabled();
    return;
  }
  DispatchToMainThread("MediaStreamTrackSourceHandle::ApplyEnabled",
                       [self = RefPtr(this)] { self->ApplyEnabled(); });
}

template <typename Function>
/* static */
void MediaStreamTrackSourceHandle::DispatchToListener(
    const ListenerEntry& aEntry, const char* aName, const Function& aFunction) {
  // A failed dispatch means the listener's thread is gone, which is fine.
  (void)aEntry.mTarget->Dispatch(
      NS_NewRunnableFunction(
          aName, [listener = aEntry.mListener,
                  function = aFunction] { function(listener.get()); }),
      NS_DISPATCH_FALLIBLE);
}

template <typename Function>
void MediaStreamTrackSourceHandle::ForwardToListeners(const char* aName,
                                                      Function&& aFunction) {
  MOZ_ASSERT(IsOnSourceThread());
  MutexAutoLock lock(mMutex);
  for (const ListenerEntry& entry : mListeners) {
    DispatchToListener(entry, aName, aFunction);
  }
}

}  // namespace mozilla::dom

#undef LOG
