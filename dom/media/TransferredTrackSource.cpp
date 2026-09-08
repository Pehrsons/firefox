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

/**
 * Thread-safe listener registered with the handle. The handle dispatches
 * notifications to the owner thread holding a reference to this. Once the
 * owner has detached, notifications are dropped.
 */
class TransferredTrackSource::Receiver final
    : public MediaStreamTrackSourceHandle::Listener {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(Receiver, override)

  explicit Receiver(TransferredTrackSource* aOwner) : mOwner(aOwner) {}

  void MutedChanged(bool aNewState) override {
    if (mOwner) {
      mOwner->OnMutedChanged(aNewState);
    }
  }
  void ConstraintsChanged(const MediaTrackConstraints& aConstraints,
                          const MediaTrackSettings& aSettings) override {
    if (mOwner) {
      mOwner->OnConstraintsChanged(aConstraints, aSettings);
    }
  }
  void OverrideEnded() override {
    if (mOwner) {
      mOwner->OnOverrideEnded();
    }
  }
  void GraphTrackAvailable(ProcessedMediaTrack* aTrack) override {
    if (mOwner) {
      mOwner->OnGraphTrackAvailable(aTrack);
    }
  }

  void Disconnect() { mOwner = nullptr; }

 private:
  ~Receiver() = default;

  // Raw pointer is safe because it is only accessed on the owner's thread,
  // and the owner clears it in Detach() before it can be destroyed.
  TransferredTrackSource* mOwner;
};

NS_IMPL_ISUPPORTS_INHERITED0(TransferredTrackSource, MediaStreamTrackSource)

/* static */
already_AddRefed<TransferredTrackSource> TransferredTrackSource::Create(
    const MediaStreamTrack::TransferredData& aData) {
  RefPtr<TransferredTrackSource> source = new TransferredTrackSource(
      aData.mSource, aData.mLabel, aData.mMediaSource, aData.mHasAlpha,
      aData.mSettings, aData.mCapabilities);

  if (aData.mReadyState == MediaStreamTrackState::Ended) {
    // An ended track never registers as a sink, so there is nothing to keep
    // alive or listen to.
    source->mDetached = true;
    return source.forget();
  }

  if (!source->Init(aData.mEnabled)) {
    return nullptr;
  }
  return source.forget();
}

TransferredTrackSource::TransferredTrackSource(
    MediaStreamTrackSourceHandle* aHandle, const nsAString& aLabel,
    MediaSourceEnum aMediaSource, bool aHasAlpha,
    const MediaTrackSettings& aSettings,
    const MediaTrackCapabilities& aCapabilities)
    : MediaStreamTrackSource(nullptr, nsString(aLabel), TrackingId()),
      mHandle(aHandle),
      mReceiver(MakeAndAddRef<Receiver>(this)),
      mOwnerThread(GetCurrentSerialEventTarget()),
      mMediaSource(aMediaSource),
      mHasAlpha(aHasAlpha),
      mSettings(aSettings),
      mCapabilities(aCapabilities),
      mEnabled(true) {
  MOZ_ASSERT(mHandle);
  LOG(LogLevel::Info, ("TransferredTrackSource {} created for handle {}",
                       fmt::ptr(this), fmt::ptr(mHandle.get())));
}

TransferredTrackSource::~TransferredTrackSource() { Detach(); }

bool TransferredTrackSource::Init(bool aEnabled) {
  NS_ASSERT_OWNINGTHREAD(TransferredTrackSource);
  MOZ_ASSERT(!mDetached);
  mEnabled = aEnabled;

  if (WorkerPrivate* workerPrivate = GetCurrentThreadWorkerPrivate()) {
    RefPtr<StrongWorkerRef> workerRef = StrongWorkerRef::Create(
        workerPrivate, "TransferredTrackSource", [self = RefPtr{this}] {
          LOG(LogLevel::Info, ("TransferredTrackSource {} worker is going away",
                               fmt::ptr(self.get())));
          // End our tracks from a regular task rather than from this
          // WorkerControlRunnable, so that what they dispatch to the graph
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
      return false;
    }
    mWorkerRef = new ThreadSafeWorkerRef(workerRef);
  }

  mHandle->AddListener(mReceiver, mOwnerThread, mEnabled);
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
    // Our tracks have ended, and so will the clone.
    return {};
  }

  // The handle carries the enabled state as a placeholder user until the clone
  // has attached its own listener.
  RefPtr<MediaStreamTrackSourceHandle> handle =
      MediaStreamTrackSourceHandle::CreateClone(mHandle, mEnabled);
  RefPtr<TransferredTrackSource> clone = new TransferredTrackSource(
      handle, mLabel, mMediaSource, mHasAlpha, mSettings, mCapabilities);
  const bool attached = clone->Init(mEnabled);
  handle->RemoveUser(mEnabled);
  if (!attached) {
    // The worker is shutting down. Sharing this source is as good as anything.
    return {};
  }
  LOG(LogLevel::Info, ("TransferredTrackSource {} cloned into {}",
                       fmt::ptr(this), fmt::ptr(clone.get())));
  // The clone's tracks get their graph track from the clone's holder through
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
  mReceiver->Disconnect();
  if (mGraphTrack) {
    // Removing our listener from the handle may destroy the graph track our
    // tracks borrowed. Their mirrors of it disconnect through a dispatch to
    // the graph thread, so remove the listener through a dispatch to the graph
    // thread too, queued behind those. That guarantees the mirrors are gone
    // from the graph track before it can be destroyed. The graph track is
    // alive until then since we stay a user of the handle.
    RefPtr<MediaTrackGraphImpl> graph = mGraphTrack->GraphImpl();
    mGraphTrack = nullptr;
    // The graph runs this even if it is shutting down.
    MOZ_ALWAYS_SUCCEEDS(graph->Dispatch(
        NS_NewRunnableFunction("TransferredTrackSource::RemoveListener",
                               [handle = mHandle, receiver = mReceiver] {
                                 handle->RemoveListener(receiver);
                               })));
  } else {
    mHandle->RemoveListener(mReceiver);
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
    // Our tracks have ended. There is no observable outcome, so pretend we
    // succeeded, like LocalTrackSource does.
    return ApplyConstraintsPromise::CreateAndResolve(false, __func__);
  }
  return InvokeAsync(
             GetMainThreadSerialEventTarget(), __func__,
             [handle = mHandle,
              constraints = MediaTrackConstraints(aConstraints),
              aCallerType]() -> RefPtr<ApplyConstraintsPromise> {
               GraphTrackHolder* holder = handle->Holder();
               if (!holder) {
                 // The holder is gone or a pending clone, in which case our
                 // tracks are ending. There is no observable outcome.
                 return ApplyConstraintsPromise::CreateAndResolve(false,
                                                                  __func__);
               }
               // The source notifies its sinks of the new constraints and
               // settings from a Then() on the main thread when this
               // resolves. Resolving our promise from a later Then() on the
               // same thread makes that notification reach the caller's
               // thread through the handle before the promise resolution
               // does, so getSettings() is up to date.
               return holder->Source()
                   .ApplyConstraints(constraints, aCallerType)
                   ->Then(
                       GetMainThreadSerialEventTarget(), __func__,
                       [](const ApplyConstraintsPromise::ResolveOrRejectValue&
                              aValue) {
                         return ApplyConstraintsPromise::
                             CreateAndResolveOrReject(aValue, __func__);
                       });
             })
      // Resolve on this thread while holding the worker alive, so the result
      // is guaranteed to land here, and so that the caller's Then() on the
      // returned promise is dispatched from this thread.
      ->Then(mOwnerThread, __func__,
             [workerRef = mWorkerRef](
                 const ApplyConstraintsPromise::ResolveOrRejectValue& aValue) {
               return ApplyConstraintsPromise::CreateAndResolveOrReject(
                   aValue, __func__);
             });
}

void TransferredTrackSource::GetSettings(MediaTrackSettings& aResult) {
  NS_ASSERT_OWNINGTHREAD(TransferredTrackSource);
  aResult = mSettings;
}

void TransferredTrackSource::GetCapabilities(MediaTrackCapabilities& aResult) {
  NS_ASSERT_OWNINGTHREAD(TransferredTrackSource);
  aResult = mCapabilities;
}

void TransferredTrackSource::Stop() {
  NS_ASSERT_OWNINGTHREAD(TransferredTrackSource);
  LOG(LogLevel::Info, ("TransferredTrackSource {} stopped", fmt::ptr(this)));
  // Our tracks no longer keep the original source alive. Whether it stops is
  // up to its remaining sinks.
  Detach();
}

void TransferredTrackSource::Disable() {
  NS_ASSERT_OWNINGTHREAD(TransferredTrackSource);
  mEnabled = false;
  if (mDetached) {
    return;
  }
  mHandle->SetListenerEnabled(mReceiver, false);
}

void TransferredTrackSource::Enable() {
  NS_ASSERT_OWNINGTHREAD(TransferredTrackSource);
  mEnabled = true;
  if (mDetached) {
    return;
  }
  mHandle->SetListenerEnabled(mReceiver, true);
}

void TransferredTrackSource::OnMutedChanged(bool aNewState) {
  NS_ASSERT_OWNINGTHREAD(TransferredTrackSource);
  MutedChanged(aNewState);
}

void TransferredTrackSource::OnConstraintsChanged(
    const MediaTrackConstraints& aConstraints,
    const MediaTrackSettings& aSettings) {
  NS_ASSERT_OWNINGTHREAD(TransferredTrackSource);
  mSettings = aSettings;
  ConstraintsChanged(aConstraints);
}

void TransferredTrackSource::OnOverrideEnded() {
  NS_ASSERT_OWNINGTHREAD(TransferredTrackSource);
  OverrideEnded();
}

void TransferredTrackSource::OnGraphTrackAvailable(
    ProcessedMediaTrack* aTrack) {
  NS_ASSERT_OWNINGTHREAD(TransferredTrackSource);
  LOG(LogLevel::Debug, ("TransferredTrackSource {} graph track {} available",
                        fmt::ptr(this), fmt::ptr(aTrack)));
  mGraphTrack = aTrack;
  mSinks.RemoveElementsBy([](const WeakPtr<Sink>& aElem) { return !aElem; });
  for (const auto& sink : mSinks.Clone()) {
    sink->GraphTrackAvailable(aTrack);
  }
}

}  // namespace mozilla::dom

#undef LOG
