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

  void PrincipalChanged(const PrincipalHandle& aPrincipalHandle) override {
    if (mOwner) {
      mOwner->OnPrincipalChanged(aPrincipalHandle);
    }
  }
  void MutedChanged(bool aNewState) override {
    if (mOwner) {
      mOwner->OnMutedChanged(aNewState);
    }
  }
  void ConstraintsChanged(const MediaTrackConstraints& aConstraints) override {
    if (mOwner) {
      mOwner->OnConstraintsChanged(aConstraints);
    }
  }
  void OverrideEnded() override {
    if (mOwner) {
      mOwner->OnOverrideEnded();
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
  MOZ_ASSERT(!aData.mSource->IsOnSourceThread(),
             "Use the source directly when on its thread");
  RefPtr<TransferredTrackSource> source = new TransferredTrackSource(aData);

  if (aData.mReadyState == MediaStreamTrackState::Ended) {
    // An ended track never registers as a sink, so there is nothing to keep
    // alive or listen to.
    source->mDetached = true;
    return source.forget();
  }

  if (WorkerPrivate* workerPrivate = GetCurrentThreadWorkerPrivate()) {
    RefPtr<StrongWorkerRef> workerRef = StrongWorkerRef::Create(
        workerPrivate, "TransferredTrackSource", [self = RefPtr{source}] {
          LOG(LogLevel::Info, ("TransferredTrackSource {} worker is going away",
                               fmt::ptr(self.get())));
          self->Detach();
        });
    if (NS_WARN_IF(!workerRef)) {
      // Detaching before attaching, so mark it detached to avoid touching the
      // handle in Destroy().
      source->mDetached = true;
      return nullptr;
    }
    source->mWorkerRef = std::move(workerRef);
  }

  source->mHandle->AddListener(source->mReceiver, source->mOwnerThread,
                               aData.mEnabled);
  return source.forget();
}

TransferredTrackSource::TransferredTrackSource(
    const MediaStreamTrack::TransferredData& aData)
    : MediaStreamTrackSource(nullptr, aData.mLabel, TrackingId()),
      mHandle(aData.mSource),
      mReceiver(MakeAndAddRef<Receiver>(this)),
      mOwnerThread(GetCurrentSerialEventTarget()),
      mMediaSource(aData.mMediaSource),
      mHasAlpha(aData.mHasAlpha),
      mSettings(aData.mSettings),
      mCapabilities(aData.mCapabilities),
      mPrincipalHandle(aData.mPrincipalHandle) {
  LOG(LogLevel::Info, ("TransferredTrackSource {} created for handle {}",
                       fmt::ptr(this), fmt::ptr(mHandle.get())));
}

TransferredTrackSource::~TransferredTrackSource() { Detach(); }

already_AddRefed<MediaStreamTrackSourceHandle>
TransferredTrackSource::CreateTransferHandle(mozilla::MediaTrack* aInputTrack) {
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
  mReceiver->Disconnect();
  mHandle->RemoveListener(mReceiver);
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
  if (mDetached) {
    return;
  }
  mHandle->SetListenerEnabled(mReceiver, false);
}

void TransferredTrackSource::Enable() {
  NS_ASSERT_OWNINGTHREAD(TransferredTrackSource);
  if (mDetached) {
    return;
  }
  mHandle->SetListenerEnabled(mReceiver, true);
}

void TransferredTrackSource::OnPrincipalChanged(
    const PrincipalHandle& aPrincipalHandle) {
  NS_ASSERT_OWNINGTHREAD(TransferredTrackSource);
  mPrincipalHandle = aPrincipalHandle;
  // TODO(Bug 1991619): MediaStreamTrackSource::PrincipalChanged() would have
  // tracks combine nsIPrincipals, which cannot be done off the main thread.
}

void TransferredTrackSource::OnMutedChanged(bool aNewState) {
  NS_ASSERT_OWNINGTHREAD(TransferredTrackSource);
  MutedChanged(aNewState);
}

void TransferredTrackSource::OnConstraintsChanged(
    const MediaTrackConstraints& aConstraints) {
  NS_ASSERT_OWNINGTHREAD(TransferredTrackSource);
  ConstraintsChanged(aConstraints);
}

void TransferredTrackSource::OnOverrideEnded() {
  NS_ASSERT_OWNINGTHREAD(TransferredTrackSource);
  OverrideEnded();
}

}  // namespace mozilla::dom

#undef LOG
