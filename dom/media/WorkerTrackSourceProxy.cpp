/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "WorkerTrackSourceProxy.h"

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
      aData.mDeviceId, aData.mGroupId, aData.mTrack);

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
    const nsAString& aDeviceId, const nsAString& aGroupId,
    ProcessedMediaTrack* aGraphTrack)
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
      mHolderState(mAbstractThread, HolderState{.mGraphTrack = aGraphTrack},
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
          // End our track from a regular task rather than from this
          // WorkerControlRunnable, so that what it dispatches to the graph
          // when disconnecting from it goes out in order with what Detach()
          // dispatches. mWorkerRef keeps the worker running until Detach()
          // has released it.
          nsresult rv = self->mOwnerThread->Dispatch(NS_NewRunnableFunction(
              "WorkerTrackSourceProxy::WorkerShutdown", [self] {
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
      return false;
    }
    mWorkerRef = new ThreadSafeWorkerRef(workerRef);
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

MediaStreamTrackSource::CloneResult WorkerTrackSourceProxy::Clone() {
  NS_ASSERT_OWNINGTHREAD(WorkerTrackSourceProxy);
  if (mDetached) {
    // Our track has ended, and so will the clone.
    return {};
  }

  // The cloned track starts out enabled and sets its own state through the
  // clone once created, see MediaStreamTrack::CloneInternal.
  RefPtr<GraphTrackHolder> holder =
      GraphTrackHolder::CreateClone(mHolder, /* aEnabled = */ true, mMuted);
  RefPtr<WorkerTrackSourceProxy> clone = new WorkerTrackSourceProxy(
      holder, mAbstractThread, mLabel, mMediaSource, mHasAlpha, mMuted,
      Settings(), Capabilities(), mDeviceId, mGroupId, nullptr);
  if (!clone->Init(/* aEnabled = */ true)) {
    // The worker is shutting down. Sharing this source is as good as anything.
    return {};
  }
  LOG(LogLevel::Info, ("WorkerTrackSourceProxy {} cloned into {}",
                       fmt::ptr(this), fmt::ptr(clone.get())));
  // The clone's track gets its graph track from the clone's holder through
  // Sink::GraphTrackAvailable once it exists.
  return {.mSource = clone, .mInputTrack = nullptr};
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
  mHolderReadyPromise.RejectIfExists(NS_ERROR_ABORT, __func__);
  RefPtr<ProcessedMediaTrack> graphTrack = mHolderState.Ref().mGraphTrack;
  mMuted.DisconnectIfConnected();
  mEnded.DisconnectIfConnected();
  mHolderState.DisconnectIfConnected();
  mOriginalSettings.DisconnectIfConnected();
  mOriginalCapabilities.DisconnectIfConnected();
  if (!mHolderTransferred) {
    if (graphTrack) {
      // Shutting down the holder destroys the graph track our track borrowed.
      // Its mirrors of it disconnect through a dispatch to the graph thread,
      // so shut the holder down through a dispatch to the graph thread too,
      // queued behind those. That guarantees the mirrors are gone from the
      // graph track before it can be destroyed.
      RefPtr<MediaTrackGraphImpl> graph = graphTrack->GraphImpl();
      // The graph runs this even if it is shutting down.
      MOZ_ALWAYS_SUCCEEDS(graph->Dispatch(NS_NewRunnableFunction(
          "WorkerTrackSourceProxy::ShutdownHolder",
          [holder = mHolder] { holder->ShutdownAsync(); })));
    } else {
      mHolder->ShutdownAsync();
    }
  }
  if (mWorkerRef) {
    // Detach() may run from the WorkerRef callback, during which the
    // WorkerPrivate iterates its WorkerRefs. Destroying the StrongWorkerRef
    // synchronously would break that iteration, so release our reference from
    // a later runnable. Handlers in flight hold their own references.
    NS_ProxyRelease("WorkerTrackSourceProxy::mWorkerRef", mOwnerThread,
                    mWorkerRef.forget(), true);
  }
}

RefPtr<MediaStreamTrackSource::ApplyConstraintsPromise>
WorkerTrackSourceProxy::ApplyConstraints(
    const MediaTrackConstraints& aConstraints, CallerType aCallerType) {
  NS_ASSERT_OWNINGTHREAD(WorkerTrackSourceProxy);
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

RefPtr<GenericPromise> WorkerTrackSourceProxy::HolderReady() {
  NS_ASSERT_OWNINGTHREAD(WorkerTrackSourceProxy);
  MOZ_ASSERT(!mDetached);
  if (mHolderReady) {
    return GenericPromise::CreateAndResolve(true, __func__);
  }
  return mHolderReadyPromise.Ensure(__func__);
}

RefPtr<MediaStreamTrackSource::ApplyConstraintsPromise>
WorkerTrackSourceProxy::ProxyApplyConstraints(
    const MediaTrackConstraints& aConstraints, CallerType aCallerType) {
  NS_ASSERT_OWNINGTHREAD(WorkerTrackSourceProxy);
  MOZ_ASSERT(!mDetached);
  MOZ_ASSERT(mHolderReady);
  return InvokeAsync(
             AbstractThread::MainThread(), __func__,
             [holder = mHolder,
              constraints = MediaTrackConstraints(aConstraints),
              aCallerType]() -> RefPtr<ApplyConstraintsPromise> {
               if (!holder->Attached()) {
                 // The holder was taken over by a track on the main thread,
                 // in which case our track is ending. There is no observable
                 // outcome.
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
  LOG(LogLevel::Debug, ("WorkerTrackSourceProxy {} graph track {} available",
                        fmt::ptr(this), fmt::ptr(state.mGraphTrack.get())));
  mOriginalSettings.Connect(state.mSettings);
  mWatchManager.Watch(mOriginalSettings,
                      &WorkerTrackSourceProxy::OnSettingsChanged);
  mOriginalCapabilities.Connect(state.mCapabilities);
  mWatchManager.Watch(mOriginalCapabilities,
                      &WorkerTrackSourceProxy::OnCapabilitiesChanged);
  mHolderReadyPromise.ResolveIfExists(true, __func__);
  mSinks.RemoveElementsBy([](const WeakPtr<Sink>& aElem) { return !aElem; });
  for (const auto& sink : mSinks.Clone()) {
    sink->GraphTrackAvailable(state.mGraphTrack);
  }
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
