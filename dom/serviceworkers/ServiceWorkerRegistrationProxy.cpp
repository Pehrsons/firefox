/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ServiceWorkerRegistrationProxy.h"

#include "ServiceWorkerManager.h"
#include "ServiceWorkerRegistrationParent.h"
#include "ServiceWorkerUnregisterCallback.h"
#include "mozilla/SchedulerGroup.h"
#include "mozilla/ScopeExit.h"
#include "mozilla/dom/notification/NotificationUtils.h"
#include "mozilla/ipc/BackgroundParent.h"
#include "nsINotificationStorage.h"

namespace mozilla::dom {

using mozilla::ipc::AssertIsOnBackgroundThread;
using namespace mozilla::dom::notification;

class ServiceWorkerRegistrationProxy::DelayedUpdate final
    : public nsITimerCallback,
      public nsINamed {
  RefPtr<ServiceWorkerRegistrationProxy> mProxy;
  MozPromiseHolder<ServiceWorkerRegistrationPromise> mPromise;
  nsCOMPtr<nsITimer> mTimer;
  nsCString mNewestWorkerScriptUrl;

  ~DelayedUpdate() = default;

 public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSITIMERCALLBACK
  NS_DECL_NSINAMED

  DelayedUpdate(RefPtr<ServiceWorkerRegistrationProxy>&& aProxy,
                MozPromiseHolder<ServiceWorkerRegistrationPromise>&& aHolder,
                nsCString&& aNewestWorkerScriptUrl, uint32_t delay);

  void ChainTo(MozPromiseHolder<ServiceWorkerRegistrationPromise>&& aPromise);

  void Reject();

  void SetNewestWorkerScriptUrl(nsCString&& aNewestWorkerScriptUrl);
};

ServiceWorkerRegistrationProxy::~ServiceWorkerRegistrationProxy() {
  // Any thread
  MOZ_DIAGNOSTIC_ASSERT(!mActor);
  MOZ_DIAGNOSTIC_ASSERT(!mReg);
}

void ServiceWorkerRegistrationProxy::MaybeShutdownOnBGThread() {
  AssertIsOnBackgroundThread();
  if (!mActor) {
    return;
  }
  mActor->MaybeSendDelete();
}

void ServiceWorkerRegistrationProxy::UpdateStateOnBGThread(
    const ServiceWorkerRegistrationDescriptor& aDescriptor) {
  AssertIsOnBackgroundThread();
  if (!mActor) {
    return;
  }
  (void)mActor->SendUpdateState(aDescriptor.ToIPC());
}

void ServiceWorkerRegistrationProxy::FireUpdateFoundOnBGThread() {
  AssertIsOnBackgroundThread();
  if (!mActor) {
    return;
  }
  (void)mActor->SendFireUpdateFound();
}

void ServiceWorkerRegistrationProxy::InitOnMainThread() {
  AssertIsOnMainThread();

  auto scopeExit = MakeScopeExit([&] { MaybeShutdownOnMainThread(); });

  RefPtr<ServiceWorkerManager> swm = ServiceWorkerManager::GetInstance();
  NS_ENSURE_TRUE_VOID(swm);

  RefPtr<ServiceWorkerRegistrationInfo> reg =
      swm->GetRegistration(mDescriptor.PrincipalInfo(), mDescriptor.Scope());
  NS_ENSURE_TRUE_VOID(reg);

  if (reg->Id() != mDescriptor.Id()) {
    // This registration has already been replaced by another one.
    return;
  }

  scopeExit.release();

  mReg = new nsMainThreadPtrHolder<ServiceWorkerRegistrationInfo>(
      "ServiceWorkerRegistrationProxy::mInfo", reg);

  mReg->AddInstance(this, mDescriptor);
}

void ServiceWorkerRegistrationProxy::MaybeShutdownOnMainThread() {
  AssertIsOnMainThread();

  if (mDelayedUpdate) {
    mDelayedUpdate->Reject();
    mDelayedUpdate = nullptr;
  }
  nsCOMPtr<nsIRunnable> r = NewRunnableMethod(
      __func__, this, &ServiceWorkerRegistrationProxy::MaybeShutdownOnBGThread);

  MOZ_ALWAYS_SUCCEEDS(mEventTarget->Dispatch(r.forget(), NS_DISPATCH_NORMAL));
}

void ServiceWorkerRegistrationProxy::StopListeningOnMainThread() {
  AssertIsOnMainThread();

  if (mDelayedUpdate) {
    mDelayedUpdate->Reject();
    mDelayedUpdate = nullptr;
  }

  if (!mReg) {
    return;
  }

  mReg->RemoveInstance(this);
  mReg = nullptr;
}

void ServiceWorkerRegistrationProxy::UpdateState(
    const ServiceWorkerRegistrationDescriptor& aDescriptor) {
  AssertIsOnMainThread();

  if (mDescriptor == aDescriptor) {
    return;
  }
  mDescriptor = aDescriptor;

  nsCOMPtr<nsIRunnable> r =
      NewRunnableMethod<ServiceWorkerRegistrationDescriptor>(
          __func__, this,
          &ServiceWorkerRegistrationProxy::UpdateStateOnBGThread, aDescriptor);

  MOZ_ALWAYS_SUCCEEDS(mEventTarget->Dispatch(r.forget(), NS_DISPATCH_NORMAL));
}

void ServiceWorkerRegistrationProxy::FireUpdateFound() {
  AssertIsOnMainThread();

  nsCOMPtr<nsIRunnable> r = NewRunnableMethod(
      __func__, this,
      &ServiceWorkerRegistrationProxy::FireUpdateFoundOnBGThread);

  MOZ_ALWAYS_SUCCEEDS(mEventTarget->Dispatch(r.forget(), NS_DISPATCH_NORMAL));
}

void ServiceWorkerRegistrationProxy::RegistrationCleared() {
  MaybeShutdownOnMainThread();
}

void ServiceWorkerRegistrationProxy::GetScope(nsAString& aScope) const {
  CopyUTF8toUTF16(mDescriptor.Scope(), aScope);
}

bool ServiceWorkerRegistrationProxy::MatchesDescriptor(
    const ServiceWorkerRegistrationDescriptor& aDescriptor) {
  AssertIsOnMainThread();
  return aDescriptor.Id() == mDescriptor.Id() &&
         aDescriptor.PrincipalInfo() == mDescriptor.PrincipalInfo() &&
         aDescriptor.Scope() == mDescriptor.Scope();
}

ServiceWorkerRegistrationProxy::ServiceWorkerRegistrationProxy(
    const ServiceWorkerRegistrationDescriptor& aDescriptor,
    const ClientInfo& aForClient)
    : mEventTarget(GetCurrentSerialEventTarget()),
      mDescriptor(aDescriptor),
      mListeningClientInfo(aForClient) {}

void ServiceWorkerRegistrationProxy::Init(
    ServiceWorkerRegistrationParent* aActor) {
  AssertIsOnBackgroundThread();
  MOZ_DIAGNOSTIC_ASSERT(aActor);
  MOZ_DIAGNOSTIC_ASSERT(!mActor);
  MOZ_DIAGNOSTIC_ASSERT(mEventTarget);

  mActor = aActor;

  // Note, this must be done from a separate Init() method and not in
  // the constructor.  If done from the constructor the runnable can
  // execute, complete, and release its reference before the constructor
  // returns.
  nsCOMPtr<nsIRunnable> r =
      NewRunnableMethod("ServiceWorkerRegistrationProxy::Init", this,
                        &ServiceWorkerRegistrationProxy::InitOnMainThread);
  MOZ_ALWAYS_SUCCEEDS(SchedulerGroup::Dispatch(r.forget()));
}

void ServiceWorkerRegistrationProxy::RevokeActor(
    ServiceWorkerRegistrationParent* aActor) {
  AssertIsOnBackgroundThread();
  MOZ_DIAGNOSTIC_ASSERT(mActor);
  MOZ_DIAGNOSTIC_ASSERT(mActor == aActor);
  mActor = nullptr;

  nsCOMPtr<nsIRunnable> r = NewRunnableMethod(
      __func__, this,
      &ServiceWorkerRegistrationProxy::StopListeningOnMainThread);
  MOZ_ALWAYS_SUCCEEDS(SchedulerGroup::Dispatch(r.forget()));
}

RefPtr<GenericPromise> ServiceWorkerRegistrationProxy::Unregister() {
  AssertIsOnBackgroundThread();

  RefPtr<ServiceWorkerRegistrationProxy> self = this;
  MozPromiseHolder<GenericPromise> holder;
  RefPtr<GenericPromise> promise = holder.Ensure(__func__);

  nsCOMPtr<nsIRunnable> r = NS_NewRunnableFunction(
      __func__, [self, holder = std::move(holder)]() mutable {
        nsresult rv = NS_ERROR_DOM_INVALID_STATE_ERR;
        auto scopeExit = MakeScopeExit([&] { holder.Reject(rv, __func__); });

        NS_ENSURE_TRUE_VOID(self->mReg);

        RefPtr<ServiceWorkerManager> swm = ServiceWorkerManager::GetInstance();
        NS_ENSURE_TRUE_VOID(swm);

        scopeExit.release();

        RefPtr<UnregisterCallback> cb =
            new UnregisterCallback(std::move(holder));

        rv = swm->Unregister(self->mReg->Principal(), cb,
                             NS_ConvertUTF8toUTF16(self->mReg->Scope()));
        if (NS_WARN_IF(NS_FAILED(rv))) {
          cb->UnregisterFailed();
        }
      });

  MOZ_ALWAYS_SUCCEEDS(SchedulerGroup::Dispatch(r.forget()));

  return promise;
}

namespace {

class UpdateCallback final : public ServiceWorkerUpdateFinishCallback {
  MozPromiseHolder<ServiceWorkerRegistrationPromise> mPromise;

  ~UpdateCallback() = default;

 public:
  explicit UpdateCallback(
      MozPromiseHolder<ServiceWorkerRegistrationPromise>&& aPromise)
      : mPromise(std::move(aPromise)) {
    MOZ_DIAGNOSTIC_ASSERT(!mPromise.IsEmpty());
  }

  void UpdateSucceeded(ServiceWorkerRegistrationInfo* aInfo) override {
    mPromise.Resolve(aInfo->Descriptor(), __func__);
  }

  void UpdateFailed(ErrorResult& aResult) override {
    mPromise.Reject(CopyableErrorResult(aResult), __func__);
  }
};

}  // anonymous namespace

NS_IMPL_ISUPPORTS(ServiceWorkerRegistrationProxy::DelayedUpdate,
                  nsITimerCallback, nsINamed)

ServiceWorkerRegistrationProxy::DelayedUpdate::DelayedUpdate(
    RefPtr<ServiceWorkerRegistrationProxy>&& aProxy,
    MozPromiseHolder<ServiceWorkerRegistrationPromise>&& aHolder,
    nsCString&& aNewestWorkerScriptUrl, uint32_t delay)
    : mProxy(std::move(aProxy)),
      mPromise(std::move(aHolder)),
      mNewestWorkerScriptUrl(std::move(aNewestWorkerScriptUrl)) {
  MOZ_DIAGNOSTIC_ASSERT(mProxy);
  MOZ_DIAGNOSTIC_ASSERT(!mPromise.IsEmpty());
  MOZ_ASSERT(!mNewestWorkerScriptUrl.IsEmpty());
  mProxy->mDelayedUpdate = this;
  Result<nsCOMPtr<nsITimer>, nsresult> result =
      NS_NewTimerWithCallback(this, delay, nsITimer::TYPE_ONE_SHOT);
  mTimer = result.unwrapOr(nullptr);
  MOZ_DIAGNOSTIC_ASSERT(mTimer);
}

void ServiceWorkerRegistrationProxy::DelayedUpdate::ChainTo(
    MozPromiseHolder<ServiceWorkerRegistrationPromise>&& aPromise) {
  AssertIsOnMainThread();
  MOZ_ASSERT(mProxy->mDelayedUpdate == this);
  MOZ_ASSERT(!mPromise.IsEmpty());

  RefPtr promise = mPromise.Ensure(__func__);
  promise->ChainTo(std::move(aPromise), __func__);
}

void ServiceWorkerRegistrationProxy::DelayedUpdate::Reject() {
  MOZ_DIAGNOSTIC_ASSERT(!mPromise.IsEmpty());
  if (mTimer) {
    mTimer->Cancel();
    mTimer = nullptr;
  }
  mPromise.Reject(CopyableErrorResult(NS_ERROR_DOM_INVALID_STATE_ERR),
                  __func__);
}

void ServiceWorkerRegistrationProxy::DelayedUpdate::SetNewestWorkerScriptUrl(
    nsCString&& aNewestWorkerScriptUrl) {
  MOZ_ASSERT(NS_IsMainThread());
  mNewestWorkerScriptUrl = std::move(aNewestWorkerScriptUrl);
}

NS_IMETHODIMP
ServiceWorkerRegistrationProxy::DelayedUpdate::Notify(nsITimer* aTimer) {
  // Already shutting down.
  if (mProxy->mDelayedUpdate != this) {
    return NS_OK;
  }

  auto scopeExit = MakeScopeExit([&] {
    mPromise.Reject(CopyableErrorResult(NS_ERROR_DOM_INVALID_STATE_ERR),
                    __func__);
  });

  NS_ENSURE_TRUE(mProxy->mReg, NS_ERROR_FAILURE);

  RefPtr<ServiceWorkerManager> swm = ServiceWorkerManager::GetInstance();
  NS_ENSURE_TRUE(swm, NS_ERROR_FAILURE);

  RefPtr<UpdateCallback> cb = new UpdateCallback(std::move(mPromise));
  swm->Update(mProxy->mListeningClientInfo, mProxy->mReg->Principal(),
              mProxy->mReg->Scope(), std::move(mNewestWorkerScriptUrl), cb);

  mTimer = nullptr;
  mProxy->mDelayedUpdate = nullptr;

  scopeExit.release();
  return NS_OK;
}

NS_IMETHODIMP
ServiceWorkerRegistrationProxy::DelayedUpdate::GetName(nsACString& aName) {
  aName.AssignLiteral("ServiceWorkerRegistrationProxy::DelayedUpdate");
  return NS_OK;
}

RefPtr<ServiceWorkerRegistrationPromise> ServiceWorkerRegistrationProxy::Update(
    const nsACString& aNewestWorkerScriptUrl) {
  AssertIsOnBackgroundThread();

  RefPtr<ServiceWorkerRegistrationProxy> self = this;
  MozPromiseHolder<ServiceWorkerRegistrationPromise> holder;
  RefPtr<ServiceWorkerRegistrationPromise> promise = holder.Ensure(__func__);

  nsCOMPtr<nsIRunnable> r = NS_NewRunnableFunction(
      __func__,
      [self, holder = std::move(holder),
       newestWorkerScriptUrl = nsCString(aNewestWorkerScriptUrl)]() mutable {
        auto scopeExit = MakeScopeExit([&] {
          holder.Reject(CopyableErrorResult(NS_ERROR_DOM_INVALID_STATE_ERR),
                        __func__);
        });

        // Get the delay value for the update
        NS_ENSURE_TRUE_VOID(self->mReg);
        uint32_t delay = self->mReg->GetUpdateDelay(false);

        // If the delay value does not equal to 0, create a timer and a timer
        // callback to perform the delayed update. Otherwise, update directly.
        if (delay) {
          if (self->mDelayedUpdate) {
            // NOTE: if we `ChainTo(),` there will ultimately be a single
            // update, and this update will resolve all promises that were
            // issued while the update's timer was ticking down.
            self->mDelayedUpdate->ChainTo(std::move(holder));

            // Use the "newest newest worker"'s script URL.
            self->mDelayedUpdate->SetNewestWorkerScriptUrl(
                std::move(newestWorkerScriptUrl));
          } else {
            RefPtr<ServiceWorkerRegistrationProxy::DelayedUpdate> du =
                new ServiceWorkerRegistrationProxy::DelayedUpdate(
                    std::move(self), std::move(holder),
                    std::move(newestWorkerScriptUrl), delay);
          }
        } else {
          RefPtr<ServiceWorkerManager> swm =
              ServiceWorkerManager::GetInstance();
          NS_ENSURE_TRUE_VOID(swm);

          RefPtr<UpdateCallback> cb = new UpdateCallback(std::move(holder));
          swm->Update(self->mListeningClientInfo, self->mReg->Principal(),
                      self->mReg->Scope(), std::move(newestWorkerScriptUrl),
                      cb);
        }
        scopeExit.release();
      });

  MOZ_ALWAYS_SUCCEEDS(SchedulerGroup::Dispatch(r.forget()));

  return promise;
}

RefPtr<GenericPromise>
ServiceWorkerRegistrationProxy::SetNavigationPreloadEnabled(
    const bool& aEnabled) {
  AssertIsOnBackgroundThread();

  RefPtr<ServiceWorkerRegistrationProxy> self = this;
  MozPromiseHolder<GenericPromise> holder;
  RefPtr<GenericPromise> promise = holder.Ensure(__func__);

  nsCOMPtr<nsIRunnable> r = NS_NewRunnableFunction(
      __func__, [aEnabled, self, holder = std::move(holder)]() mutable {
        nsresult rv = NS_ERROR_DOM_INVALID_STATE_ERR;
        auto scopeExit = MakeScopeExit([&] { holder.Reject(rv, __func__); });

        NS_ENSURE_TRUE_VOID(self->mReg);
        NS_ENSURE_TRUE_VOID(self->mReg->GetActive());

        auto reg = self->mReg;
        reg->SetNavigationPreloadEnabled(aEnabled);

        RefPtr<ServiceWorkerManager> swm = ServiceWorkerManager::GetInstance();
        NS_ENSURE_TRUE_VOID(swm);
        swm->StoreRegistration(reg->Principal(), reg);

        scopeExit.release();

        holder.Resolve(true, __func__);
      });

  MOZ_ALWAYS_SUCCEEDS(SchedulerGroup::Dispatch(r.forget()));

  return promise;
}

RefPtr<GenericPromise>
ServiceWorkerRegistrationProxy::SetNavigationPreloadHeader(
    const nsACString& aHeader) {
  AssertIsOnBackgroundThread();

  RefPtr<ServiceWorkerRegistrationProxy> self = this;
  MozPromiseHolder<GenericPromise> holder;
  RefPtr<GenericPromise> promise = holder.Ensure(__func__);

  nsCOMPtr<nsIRunnable> r =
      NS_NewRunnableFunction(__func__, [aHeader = nsCString(aHeader), self,
                                        holder = std::move(holder)]() mutable {
        nsresult rv = NS_ERROR_DOM_INVALID_STATE_ERR;
        auto scopeExit = MakeScopeExit([&] { holder.Reject(rv, __func__); });

        NS_ENSURE_TRUE_VOID(self->mReg);
        NS_ENSURE_TRUE_VOID(self->mReg->GetActive());

        auto reg = self->mReg;
        reg->SetNavigationPreloadHeader(aHeader);

        RefPtr<ServiceWorkerManager> swm = ServiceWorkerManager::GetInstance();
        NS_ENSURE_TRUE_VOID(swm);
        swm->StoreRegistration(reg->Principal(), reg);

        scopeExit.release();

        holder.Resolve(true, __func__);
      });

  MOZ_ALWAYS_SUCCEEDS(SchedulerGroup::Dispatch(r.forget()));

  return promise;
}

RefPtr<NavigationPreloadStatePromise>
ServiceWorkerRegistrationProxy::GetNavigationPreloadState() {
  AssertIsOnBackgroundThread();

  RefPtr<ServiceWorkerRegistrationProxy> self = this;
  MozPromiseHolder<NavigationPreloadStatePromise> holder;
  RefPtr<NavigationPreloadStatePromise> promise = holder.Ensure(__func__);

  nsCOMPtr<nsIRunnable> r = NS_NewRunnableFunction(
      __func__, [self, holder = std::move(holder)]() mutable {
        auto scopeExit = MakeScopeExit([&] {
          holder.Reject(CopyableErrorResult(NS_ERROR_DOM_INVALID_STATE_ERR),
                        __func__);
        });

        NS_ENSURE_TRUE_VOID(self->mReg);
        scopeExit.release();

        holder.Resolve(self->mReg->GetNavigationPreloadState(), __func__);
      });

  MOZ_ALWAYS_SUCCEEDS(SchedulerGroup::Dispatch(r.forget()));

  return promise;
}

RefPtr<NotificationsPromise> ServiceWorkerRegistrationProxy::GetNotifications(
    const nsAString& aTag) {
  AssertIsOnBackgroundThread();

  RefPtr<ServiceWorkerRegistrationProxy> self = this;
  return InvokeAsync(
      GetMainThreadSerialEventTarget(), __func__,
      [self, tag = nsString(aTag)]() {
        Result<nsCOMPtr<nsIPrincipal>, nsresult> principalResult =
            self->mListeningClientInfo.GetPrincipal();
        if (principalResult.isErr()) {
          return NotificationsPromise::CreateAndReject(
              NS_ERROR_DOM_INVALID_STATE_ERR, __func__);
        }

        nsCOMPtr<nsIPrincipal> principal = principalResult.unwrap();
        nsString origin;
        GetOrigin(principal, origin);

        RefPtr<NotificationsPromise> promise = GetStoredNotificationsForScope(
            principal, self->mDescriptor.Scope(), tag);
        return promise;
      });
}

}  // namespace mozilla::dom
