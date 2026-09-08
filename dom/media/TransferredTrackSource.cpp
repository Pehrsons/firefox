/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "TransferredTrackSource.h"

#include "MediaStreamError.h"
#include "MediaTrackGraphImpl.h"
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
      aData.mDeviceId, aData.mGroupId, aData.mTrack);

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
    const nsAString& aDeviceId, const nsAString& aGroupId,
    ProcessedMediaTrack* aGraphTrack)
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
      mHolderState(mAbstractThread, HolderState{.mGraphTrack = aGraphTrack},
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
          // End our track from a regular task rather than from this
          // WorkerControlRunnable, so that what it dispatches to the graph
          // when disconnecting from it goes out in order with what Detach()
          // dispatches. mWorkerRef keeps the worker running until Detach()
          // has released it.
          nsresult rv = self->mOwnerThread->Dispatch(NS_NewRunnableFunction(
              "TransferredTrackSource::WorkerShutdown", [self] {
                self->OverrideEnded();
                self->Detach();
              }));
          if (NS_WARN_IF(NS_FAILED(rv))) {
            MOZ_ASSERT_UNREACHABLE("The worker runs while we hold a ref");
            self->Detach();
          }
        });
    if (NS_WARN_IF(!workerRef)) {
      mDetached = true;
      mHandle = nullptr;
      return false;
    }
    mWorkerRef = new ThreadSafeWorkerRef(workerRef);
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

MediaStreamTrackSource::CloneResult TransferredTrackSource::Clone() {
  NS_ASSERT_OWNINGTHREAD(TransferredTrackSource);
  if (mDetached) {
    // Our track has ended, and so will the clone.
    return {};
  }

  // The cloned track starts out enabled and sets its own state through the
  // clone once created, see MediaStreamTrack::CloneInternal.
  RefPtr<MediaStreamTrackSourceHandle> handle =
      MediaStreamTrackSourceHandle::CreateClone(mHandle, /* aEnabled = */ true,
                                                mMuted);
  RefPtr<TransferredTrackSource> clone = new TransferredTrackSource(
      handle, mAbstractThread, mLabel, mMediaSource, mHasAlpha, mMuted,
      Settings(), Capabilities(), mDeviceId, mGroupId, nullptr);
  if (!clone->Init(/* aEnabled = */ true)) {
    // The worker is shutting down. Sharing this source is as good as anything.
    return {};
  }
  LOG(LogLevel::Info, ("TransferredTrackSource {} cloned into {}",
                       fmt::ptr(this), fmt::ptr(clone.get())));
  // The clone's track gets its graph track from the clone's holder through
  // Sink::GraphTrackAvailable once it exists.
  return {.mSource = clone, .mInputTrack = nullptr};
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
  mHolderReadyPromise.RejectIfExists(NS_ERROR_ABORT, __func__);
  RefPtr<ProcessedMediaTrack> graphTrack = mHolderState.Ref().mGraphTrack;
  mMuted.DisconnectIfConnected();
  mEnded.DisconnectIfConnected();
  mHolderState.DisconnectIfConnected();
  mOriginalSettings.DisconnectIfConnected();
  mOriginalCapabilities.DisconnectIfConnected();
  RefPtr<MediaStreamTrackSourceHandle> handle = std::move(mHandle);
  if (graphTrack) {
    // Dropping the handle may destroy the graph track our track borrowed.
    // Its mirrors of it disconnect through a dispatch to the graph thread,
    // so drop the handle through a dispatch to the graph thread too, queued
    // behind those. That guarantees the mirrors are gone from the graph track
    // before it can be destroyed.
    RefPtr<MediaTrackGraphImpl> graph = graphTrack->GraphImpl();
    // The graph runs this even if it is shutting down.
    MOZ_ALWAYS_SUCCEEDS(graph->Dispatch(NS_NewRunnableFunction(
        "TransferredTrackSource::ReleaseHandle", [handle] {})));
  }
  if (mWorkerRef) {
    // Detach() may run from the WorkerRef callback, during which the
    // WorkerPrivate iterates its WorkerRefs. Destroying the StrongWorkerRef
    // synchronously would break that iteration, so release our reference from
    // a later runnable. Handlers in flight hold their own references.
    NS_ProxyRelease("TransferredTrackSource::mWorkerRef", mOwnerThread,
                    mWorkerRef.forget(), true);
  }
}

RefPtr<MediaStreamTrackSource::ApplyConstraintsPromise>
TransferredTrackSource::ApplyConstraints(
    const MediaTrackConstraints& aConstraints, CallerType aCallerType) {
  NS_ASSERT_OWNINGTHREAD(TransferredTrackSource);
  if (mDetached) {
    // Our track has ended. There is no observable outcome, so pretend we
    // succeeded, like LocalTrackSource does.
    return ApplyConstraintsPromise::CreateAndResolve(false, __func__);
  }
  if (mHolderReady) {
    return ProxyApplyConstraints(aConstraints, aCallerType);
  }
  // Our settings mirror must connect to the source's Canonical before the
  // request reaches the main thread, so that the settings it results in are
  // mirrored here ahead of the resolution.
  return HolderReady()->Then(
      mAbstractThread, __func__,
      [self = RefPtr(this), this,
       constraints = MediaTrackConstraints(aConstraints),
       aCallerType] { return ProxyApplyConstraints(constraints, aCallerType); },
      [] {
        // Detached while waiting. There is no observable outcome.
        return ApplyConstraintsPromise::CreateAndResolve(false, __func__);
      });
}

RefPtr<GenericPromise> TransferredTrackSource::HolderReady() {
  NS_ASSERT_OWNINGTHREAD(TransferredTrackSource);
  MOZ_ASSERT(!mDetached);
  if (mHolderReady) {
    return GenericPromise::CreateAndResolve(true, __func__);
  }
  return mHolderReadyPromise.Ensure(__func__);
}

RefPtr<MediaStreamTrackSource::ApplyConstraintsPromise>
TransferredTrackSource::ProxyApplyConstraints(
    const MediaTrackConstraints& aConstraints, CallerType aCallerType) {
  NS_ASSERT_OWNINGTHREAD(TransferredTrackSource);
  MOZ_ASSERT(!mDetached);
  MOZ_ASSERT(mHolderReady);
  return InvokeAsync(
             AbstractThread::MainThread(), __func__,
             [handle = mHandle,
              constraints = MediaTrackConstraints(aConstraints),
              aCallerType]() -> RefPtr<ApplyConstraintsPromise> {
               GraphTrackHolder* holder = handle->Holder();
               if (!holder) {
                 // The holder is a pending clone, in which case our track is
                 // ending. There is no observable outcome.
                 return ApplyConstraintsPromise::CreateAndResolve(false,
                                                                  __func__);
               }
               // The source notifies its sinks of the new constraints and
               // settings from a Then() on the main thread's AbstractThread
               // when this resolves, which updates the settings Canonical.
               // Reacting from a later Then() on that same AbstractThread
               // makes tail dispatch order the mirror update ahead of our
               // resolution on the caller's thread, so getSettings() is up to
               // date.
               return holder->Source()
                   .ApplyConstraints(constraints, aCallerType)
                   ->Then(
                       AbstractThread::MainThread(), __func__,
                       [](const ApplyConstraintsPromise::ResolveOrRejectValue&
                              aValue) {
                         return ApplyConstraintsPromise::
                             CreateAndResolveOrReject(aValue, __func__);
                       });
             })
      // Resolve on this thread's AbstractThread, so that tail dispatch orders
      // the mirror updates ahead of the resolution, while holding the worker
      // alive so the result is guaranteed to land here.
      ->Then(mAbstractThread, __func__,
             [workerRef = mWorkerRef](
                 const ApplyConstraintsPromise::ResolveOrRejectValue& aValue) {
               return ApplyConstraintsPromise::CreateAndResolveOrReject(
                   aValue, __func__);
             });
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
  LOG(LogLevel::Debug, ("TransferredTrackSource {} graph track {} available",
                        fmt::ptr(this), fmt::ptr(state.mGraphTrack.get())));
  mOriginalSettings.Connect(state.mSettings);
  mWatchManager.Watch(mOriginalSettings,
                      &TransferredTrackSource::OnSettingsChanged);
  mOriginalCapabilities.Connect(state.mCapabilities);
  mWatchManager.Watch(mOriginalCapabilities,
                      &TransferredTrackSource::OnCapabilitiesChanged);
  mHolderReadyPromise.ResolveIfExists(true, __func__);
  mSinks.RemoveElementsBy([](const WeakPtr<Sink>& aElem) { return !aElem; });
  for (const auto& sink : mSinks.Clone()) {
    sink->GraphTrackAvailable(state.mGraphTrack);
  }
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
