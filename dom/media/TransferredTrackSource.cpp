/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "TransferredTrackSource.h"

#include "MediaStreamError.h"
#include "mozilla/Logging.h"
#include "mozilla/dom/WorkerCommon.h"
#include "mozilla/dom/WorkerPrivate.h"
#include "mozilla/dom/WorkerRef.h"
#include "nsProxyRelease.h"
#include "nsThreadUtils.h"

extern mozilla::LazyLogModule gMediaStreamTrackLog;
#define LOG(type, ...) \
  MOZ_LOG_FMT(gMediaStreamTrackLog, type, MOZ_LOG_EXPAND_ARGS __VA_ARGS__)

namespace mozilla::dom {

static AbstractThread* WorkerAbstractThread() {
  WorkerPrivate* workerPrivate = GetCurrentThreadWorkerPrivate();
  MOZ_RELEASE_ASSERT(workerPrivate,
                     "TransferredTrackSource lives on worker threads only");
  return workerPrivate->GetWorkerAbstractThread();
}

NS_IMPL_ISUPPORTS_INHERITED0(TransferredTrackSource, MediaStreamTrackSource)

/* static */
already_AddRefed<TransferredTrackSource> TransferredTrackSource::Create(
    const MediaStreamTrack::TransferredData& aData) {
  RefPtr<TransferredTrackSource> source = new TransferredTrackSource(
      aData.mSource, WorkerAbstractThread(), aData.mLabel, aData.mMediaSource,
      aData.mHasAlpha, aData.mMuted, aData.mSettings, aData.mCapabilities,
      aData.mDeviceId, aData.mGroupId);

  if (aData.mReadyState == MediaStreamTrackState::Ended) {
    // An ended track never registers as a sink, so there is nothing to keep
    // alive or mirror.
    source->mDetached = true;
    source->mHandle = nullptr;
    return source.forget();
  }

  if (!source->Init(aData.mEnabled)) {
    return nullptr;
  }
  return source.forget();
}

// aOwnerThread is the current thread's AbstractThread. Callers create it ahead
// of us so that the MediaStreamTrackSource Canonicals find it.
TransferredTrackSource::TransferredTrackSource(
    MediaStreamTrackSourceHandle* aHandle, AbstractThread* aOwnerThread,
    const nsAString& aLabel, MediaSourceEnum aMediaSource, bool aHasAlpha,
    bool aMuted, const MediaStreamTrackSourceSettings& aSettings,
    const MediaStreamTrackSourceCapabilities& aCapabilities,
    const nsAString& aDeviceId, const nsAString& aGroupId)
    : MediaStreamTrackSource(nullptr, nsString(aLabel), TrackingId(),
                             nsString(aDeviceId), nsString(aGroupId)),
      mHandle(aHandle),
      mOwnerThread(GetCurrentSerialEventTarget()),
      mAbstractThread(aOwnerThread),
      mMediaSource(aMediaSource),
      mHasAlpha(aHasAlpha),
      mWatchManager(this, mAbstractThread),
      mMuted(mAbstractThread, aMuted, "TransferredTrackSource::mMuted"),
      mEnded(mAbstractThread, false, "TransferredTrackSource::mEnded"),
      mHolderState(mAbstractThread, HolderState(),
                   "TransferredTrackSource::mHolderState"),
      mOriginalSettings(mAbstractThread, aSettings,
                        "TransferredTrackSource::mOriginalSettings"),
      mOriginalCapabilities(mAbstractThread, aCapabilities,
                            "TransferredTrackSource::mOriginalCapabilities") {
  MOZ_ASSERT(mHandle);
  MOZ_ASSERT(mAbstractThread == AbstractThread::GetCurrent());
  mSettings = aSettings;
  mCapabilities = aCapabilities;
  LOG(LogLevel::Info, ("TransferredTrackSource {} created for handle {}",
                       fmt::ptr(this), fmt::ptr(mHandle.get())));
}

TransferredTrackSource::~TransferredTrackSource() { Detach(); }

bool TransferredTrackSource::Init(bool aEnabled) {
  NS_ASSERT_OWNINGTHREAD(TransferredTrackSource);
  MOZ_ASSERT(!mDetached);

  if (WorkerPrivate* workerPrivate = GetCurrentThreadWorkerPrivate()) {
    RefPtr<StrongWorkerRef> workerRef = StrongWorkerRef::Create(
        workerPrivate, "TransferredTrackSource", [self = RefPtr{this}] {
          LOG(LogLevel::Info, ("TransferredTrackSource {} worker is going away",
                               fmt::ptr(self.get())));
          self->Detach();
        });
    if (NS_WARN_IF(!workerRef)) {
      mDetached = true;
      mHandle = nullptr;
      return false;
    }
    mWorkerRef = std::move(workerRef);
  }

  mHandle->SetEnabled(aEnabled);
  mMuted.Connect(mHandle->CanonicalMuted());
  mWatchManager.Watch(mMuted, &TransferredTrackSource::OnMutedChanged);
  mEnded.Connect(mHandle->CanonicalEnded());
  mWatchManager.Watch(mEnded, &TransferredTrackSource::OnEnded);
  mHolderState.Connect(mHandle->CanonicalHolderState());
  mWatchManager.Watch(mHolderState,
                      &TransferredTrackSource::OnHolderStateChanged);
  return true;
}

already_AddRefed<MediaStreamTrackSourceHandle>
TransferredTrackSource::TransferHandle() {
  // A track transferred again stays tied to the original source only.
  return do_AddRef(mHandle);
}

void TransferredTrackSource::Destroy() {
  NS_ASSERT_OWNINGTHREAD(TransferredTrackSource);
  Detach();
}

void TransferredTrackSource::Detach() {
  NS_ASSERT_OWNINGTHREAD(TransferredTrackSource);
  if (mDetached) {
    return;
  }
  mDetached = true;
  LOG(LogLevel::Info, ("TransferredTrackSource {} detaching", fmt::ptr(this)));
  mWatchManager.Shutdown();
  mMuted.DisconnectIfConnected();
  mEnded.DisconnectIfConnected();
  mHolderState.DisconnectIfConnected();
  mOriginalSettings.DisconnectIfConnected();
  mOriginalCapabilities.DisconnectIfConnected();
  mHandle = nullptr;
  if (mWorkerRef) {
    // Detach() may run from the WorkerRef callback, during which the
    // WorkerPrivate iterates its WorkerRefs. Destroying the ref synchronously
    // would break that iteration, so release it from a later runnable.
    NS_ProxyRelease("TransferredTrackSource::mWorkerRef", mOwnerThread,
                    mWorkerRef.forget(), true);
  }
}

RefPtr<MediaStreamTrackSource::ApplyConstraintsPromise>
TransferredTrackSource::ApplyConstraints(
    const MediaTrackConstraints& aConstraints, CallerType aCallerType) {
  // TODO(Bug 1991619): Proxy to the original source through mHandle and
  // resolve on this thread. Note that MediaStreamTrack::ApplyConstraints
  // rejects before getting here when there is no window.
  return ApplyConstraintsPromise::CreateAndReject(
      MakeRefPtr<MediaMgrError>(MediaMgrError::Name::OverconstrainedError, ""),
      __func__);
}

void TransferredTrackSource::Stop() {
  NS_ASSERT_OWNINGTHREAD(TransferredTrackSource);
  LOG(LogLevel::Info, ("TransferredTrackSource {} stopped", fmt::ptr(this)));
  // Our track no longer keeps the original source alive. Whether it stops is
  // up to its remaining sinks.
  Detach();
}

void TransferredTrackSource::Disable() {
  NS_ASSERT_OWNINGTHREAD(TransferredTrackSource);
  if (!mDetached) {
    mHandle->SetEnabled(false);
  }
}

void TransferredTrackSource::Enable() {
  NS_ASSERT_OWNINGTHREAD(TransferredTrackSource);
  if (!mDetached) {
    mHandle->SetEnabled(true);
  }
}

void TransferredTrackSource::OnMutedChanged() {
  NS_ASSERT_OWNINGTHREAD(TransferredTrackSource);
  MutedChanged(mMuted);
}

void TransferredTrackSource::OnEnded() {
  NS_ASSERT_OWNINGTHREAD(TransferredTrackSource);
  if (!mEnded) {
    return;
  }
  OverrideEnded();
}

void TransferredTrackSource::OnHolderStateChanged() {
  NS_ASSERT_OWNINGTHREAD(TransferredTrackSource);
  const HolderState& state = mHolderState.Ref();
  if (!state.mSettings) {
    return;
  }
  MOZ_ASSERT(!mHolderReady, "The holder is set once");
  mHolderReady = true;
  mOriginalSettings.Connect(state.mSettings);
  mWatchManager.Watch(mOriginalSettings,
                      &TransferredTrackSource::OnSettingsChanged);
  mOriginalCapabilities.Connect(state.mCapabilities);
  mWatchManager.Watch(mOriginalCapabilities,
                      &TransferredTrackSource::OnCapabilitiesChanged);
}

void TransferredTrackSource::OnSettingsChanged() {
  NS_ASSERT_OWNINGTHREAD(TransferredTrackSource);
  mSettings = mOriginalSettings.Ref();
}

void TransferredTrackSource::OnCapabilitiesChanged() {
  NS_ASSERT_OWNINGTHREAD(TransferredTrackSource);
  mCapabilities = mOriginalCapabilities.Ref();
}

}  // namespace mozilla::dom

#undef LOG
