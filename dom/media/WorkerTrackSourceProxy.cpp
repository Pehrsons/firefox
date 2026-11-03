/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "WorkerTrackSourceProxy.h"

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
  MOZ_RELEASE_ASSERT(IsCurrentThreadRunningWorker(),
                     "WorkerTrackSourceProxy lives on worker threads only");
  return AbstractThread::GetCurrent();
}

NS_IMPL_ISUPPORTS_INHERITED0(WorkerTrackSourceProxy, MediaStreamTrackSource)

/* static */
already_AddRefed<WorkerTrackSourceProxy> WorkerTrackSourceProxy::Create(
    const MediaStreamTrack::TransferredData& aData) {
  RefPtr<WorkerTrackSourceProxy> source = new WorkerTrackSourceProxy(
      aData.mSource, WorkerAbstractThread(), aData.mLabel, aData.mMediaSource,
      aData.mHasAlpha, aData.mMuted, aData.mSettings, aData.mCapabilities,
      aData.mDeviceId, aData.mGroupId);

  if (aData.mReadyState == MediaStreamTrackState::Ended) {
    // An ended track never registers as a sink, so there is nothing to keep
    // alive or mirror. We only keep the holder, to pass it on.
    source->mDetached = true;
    return source.forget();
  }

  if (!source->Init(aData.mEnabled)) {
    return nullptr;
  }
  return source.forget();
}

// aOwnerThread is the current thread's AbstractThread. Callers create it ahead
// of us so that the MediaStreamTrackSource Canonicals find it.
WorkerTrackSourceProxy::WorkerTrackSourceProxy(
    GraphTrackHolder* aHolder, AbstractThread* aOwnerThread,
    const nsAString& aLabel, MediaSourceEnum aMediaSource, bool aHasAlpha,
    bool aMuted, const MediaStreamTrackSourceSettings& aSettings,
    const MediaStreamTrackSourceCapabilities& aCapabilities,
    const nsAString& aDeviceId, const nsAString& aGroupId)
    : MediaStreamTrackSource(nullptr, nsString(aLabel), TrackingId(),
                             nsString(aDeviceId), nsString(aGroupId)),
      mHolder(aHolder),
      mOwnerThread(GetCurrentSerialEventTarget()),
      mAbstractThread(aOwnerThread),
      mMediaSource(aMediaSource),
      mHasAlpha(aHasAlpha),
      mWatchManager(this, mAbstractThread),
      mMuted(mAbstractThread, aMuted, "WorkerTrackSourceProxy::mMuted"),
      mEnded(mAbstractThread, false, "WorkerTrackSourceProxy::mEnded"),
      mHolderState(mAbstractThread, HolderState(),
                   "WorkerTrackSourceProxy::mHolderState"),
      mOriginalSettings(mAbstractThread, aSettings,
                        "WorkerTrackSourceProxy::mOriginalSettings"),
      mOriginalCapabilities(mAbstractThread, aCapabilities,
                            "WorkerTrackSourceProxy::mOriginalCapabilities") {
  MOZ_ASSERT(mHolder);
  MOZ_ASSERT(mAbstractThread == AbstractThread::GetCurrent());
  mSettings = aSettings;
  mCapabilities = aCapabilities;
  LOG(LogLevel::Info, ("WorkerTrackSourceProxy {} created for holder {}",
                       fmt::ptr(this), fmt::ptr(mHolder.get())));
}

WorkerTrackSourceProxy::~WorkerTrackSourceProxy() { Detach(); }

bool WorkerTrackSourceProxy::Init(bool aEnabled) {
  NS_ASSERT_OWNINGTHREAD(WorkerTrackSourceProxy);
  MOZ_ASSERT(!mDetached);

  if (WorkerPrivate* workerPrivate = GetCurrentThreadWorkerPrivate()) {
    RefPtr<StrongWorkerRef> workerRef = StrongWorkerRef::Create(
        workerPrivate, "WorkerTrackSourceProxy", [self = RefPtr{this}] {
          LOG(LogLevel::Info, ("WorkerTrackSourceProxy {} worker is going away",
                               fmt::ptr(self.get())));
          self->Detach();
        });
    if (NS_WARN_IF(!workerRef)) {
      mDetached = true;
      return false;
    }
    mWorkerRef = std::move(workerRef);
  }

  mHolder->SetEnabledAsync(aEnabled);
  mMuted.Connect(mHolder->CanonicalMuted());
  mWatchManager.Watch(mMuted, &WorkerTrackSourceProxy::OnMutedChanged);
  mEnded.Connect(mHolder->CanonicalEnded());
  mWatchManager.Watch(mEnded, &WorkerTrackSourceProxy::OnEnded);
  mHolderState.Connect(mHolder->CanonicalHolderState());
  mWatchManager.Watch(mHolderState,
                      &WorkerTrackSourceProxy::OnHolderStateChanged);
  return true;
}

already_AddRefed<GraphTrackHolder> WorkerTrackSourceProxy::TransferHolder() {
  NS_ASSERT_OWNINGTHREAD(WorkerTrackSourceProxy);
  // A track transferred again stays tied to the original source only. The
  // holder carries on with the new owner, and must not shut down when our
  // track ends as part of the transfer.
  mHolderTransferred = true;
  return do_AddRef(mHolder);
}

void WorkerTrackSourceProxy::Destroy() {
  NS_ASSERT_OWNINGTHREAD(WorkerTrackSourceProxy);
  Detach();
}

void WorkerTrackSourceProxy::Detach() {
  NS_ASSERT_OWNINGTHREAD(WorkerTrackSourceProxy);
  if (mDetached) {
    return;
  }
  mDetached = true;
  LOG(LogLevel::Info, ("WorkerTrackSourceProxy {} detaching", fmt::ptr(this)));
  mWatchManager.Shutdown();
  mMuted.DisconnectIfConnected();
  mEnded.DisconnectIfConnected();
  mHolderState.DisconnectIfConnected();
  mOriginalSettings.DisconnectIfConnected();
  mOriginalCapabilities.DisconnectIfConnected();
  if (!mHolderTransferred) {
    mHolder->ShutdownAsync();
  }
  if (mWorkerRef) {
    // Detach() may run from the WorkerRef callback, during which the
    // WorkerPrivate iterates its WorkerRefs. Destroying the ref synchronously
    // would break that iteration, so release it from a later runnable.
    NS_ProxyRelease("WorkerTrackSourceProxy::mWorkerRef", mOwnerThread,
                    mWorkerRef.forget(), true);
  }
}

RefPtr<MediaStreamTrackSource::ApplyConstraintsPromise>
WorkerTrackSourceProxy::ApplyConstraints(
    const MediaTrackConstraints& aConstraints, CallerType aCallerType) {
  // TODO(Bug 1991619): Proxy to the original source through mHolder and
  // resolve on this thread. Note that MediaStreamTrack::ApplyConstraints
  // rejects before getting here when there is no window.
  return ApplyConstraintsPromise::CreateAndReject(
      MakeRefPtr<MediaMgrError>(MediaMgrError::Name::OverconstrainedError, ""),
      __func__);
}

void WorkerTrackSourceProxy::Stop() {
  NS_ASSERT_OWNINGTHREAD(WorkerTrackSourceProxy);
  LOG(LogLevel::Info, ("WorkerTrackSourceProxy {} stopped", fmt::ptr(this)));
  // Our track no longer keeps the original source alive. Whether it stops is
  // up to its remaining sinks, once the holder is shut down.
  Detach();
}

void WorkerTrackSourceProxy::Disable() {
  NS_ASSERT_OWNINGTHREAD(WorkerTrackSourceProxy);
  if (!mDetached) {
    mHolder->SetEnabledAsync(false);
  }
}

void WorkerTrackSourceProxy::Enable() {
  NS_ASSERT_OWNINGTHREAD(WorkerTrackSourceProxy);
  if (!mDetached) {
    mHolder->SetEnabledAsync(true);
  }
}

void WorkerTrackSourceProxy::OnMutedChanged() {
  NS_ASSERT_OWNINGTHREAD(WorkerTrackSourceProxy);
  MutedChanged(mMuted);
}

void WorkerTrackSourceProxy::OnEnded() {
  NS_ASSERT_OWNINGTHREAD(WorkerTrackSourceProxy);
  if (!mEnded) {
    return;
  }
  OverrideEnded();
}

void WorkerTrackSourceProxy::OnHolderStateChanged() {
  NS_ASSERT_OWNINGTHREAD(WorkerTrackSourceProxy);
  const HolderState& state = mHolderState.Ref();
  if (!state.mSettings) {
    return;
  }
  MOZ_ASSERT(!mHolderReady, "The holder is set once");
  mHolderReady = true;
  mOriginalSettings.Connect(state.mSettings);
  mWatchManager.Watch(mOriginalSettings,
                      &WorkerTrackSourceProxy::OnSettingsChanged);
  mOriginalCapabilities.Connect(state.mCapabilities);
  mWatchManager.Watch(mOriginalCapabilities,
                      &WorkerTrackSourceProxy::OnCapabilitiesChanged);
}

void WorkerTrackSourceProxy::OnSettingsChanged() {
  NS_ASSERT_OWNINGTHREAD(WorkerTrackSourceProxy);
  mSettings = mOriginalSettings.Ref();
}

void WorkerTrackSourceProxy::OnCapabilitiesChanged() {
  NS_ASSERT_OWNINGTHREAD(WorkerTrackSourceProxy);
  mCapabilities = mOriginalCapabilities.Ref();
}

}  // namespace mozilla::dom

#undef LOG
