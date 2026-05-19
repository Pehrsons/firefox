/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ClientHandle.h"

#include "ClientHandleChild.h"
#include "ClientHandleOpChild.h"
#include "ClientManager.h"
#include "ClientPrincipalUtils.h"
#include "ClientState.h"
#include "mozilla/dom/PClientManagerChild.h"
#include "mozilla/dom/ServiceWorkerDescriptor.h"
#include "mozilla/dom/ipc/StructuredCloneData.h"

namespace mozilla::dom {

using mozilla::dom::ipc::StructuredCloneData;

ClientHandle::~ClientHandle() { Shutdown(); }

void ClientHandle::Shutdown() {
  NS_ASSERT_OWNINGTHREAD(ClientHandle);
  if (IsShutdown()) {
    return;
  }

  ShutdownThing();

  mManager = nullptr;
}

RefPtr<ClientOpPromise> ClientHandle::StartOp(
    const ClientOpConstructorArgs& aArgs) {
  RefPtr<ClientOpPromise> promise;

  // Hold a ref to the client until the remote operation completes.  Otherwise
  // the ClientHandle might get de-refed and teardown the actor before we
  // get an answer.
  RefPtr<ClientHandle> kungFuGrip = this;

  MaybeExecute(
      [&aArgs, &promise, kungFuGrip](ClientHandleChild* aActor) {
        MOZ_DIAGNOSTIC_ASSERT(aActor);
        ClientHandleOpChild* actor = new ClientHandleOpChild(kungFuGrip, aArgs);
        promise = actor->mPromise;
        // Constructor failure will reject via ActorDestroy()
        aActor->SendPClientHandleOpConstructor(actor, aArgs);
      },
      [&promise] {
        CopyableErrorResult rv;
        rv.ThrowInvalidStateError("Client has been destroyed");
        promise = ClientOpPromise::CreateAndReject(rv, __func__);
      });

  return promise;
}

void ClientHandle::OnShutdownThing() {
  NS_ASSERT_OWNINGTHREAD(ClientHandle);
  if (!mDetachPromise) {
    return;
  }
  mDetachHolder.ResolveIfExists(true, __func__);
}

ClientHandle::ClientHandle(ClientManager* aManager,
                           nsISerialEventTarget* aSerialEventTarget,
                           const ClientInfo& aClientInfo)
    : mManager(aManager),
      mSerialEventTarget(aSerialEventTarget),
      mClientInfo(aClientInfo) {
  MOZ_DIAGNOSTIC_ASSERT(mManager);
  MOZ_DIAGNOSTIC_ASSERT(mSerialEventTarget);
  MOZ_ASSERT(mSerialEventTarget->IsOnCurrentThread());
}

void ClientHandle::Activate(PClientManagerChild* aActor) {
  NS_ASSERT_OWNINGTHREAD(ClientHandle);

  if (IsShutdown()) {
    return;
  }

  RefPtr<ClientHandleChild> actor = new ClientHandleChild();
  if (!aActor->SendPClientHandleConstructor(actor, mClientInfo.ToIPC())) {
    Shutdown();
    return;
  }

  ActivateThing(actor);
}

void ClientHandle::ExecutionReady(const ClientInfo& aClientInfo) {
  mClientInfo = aClientInfo;
}

const ClientInfo& ClientHandle::Info() const { return mClientInfo; }

RefPtr<GenericErrorResultPromise> ClientHandle::Control(
    const ServiceWorkerDescriptor& aServiceWorker) {
  // We should never have a cross-origin controller.  Since this would be
  // same-origin policy violation we do a full release assertion here.
  MOZ_RELEASE_ASSERT(ClientMatchPrincipalInfo(mClientInfo.PrincipalInfo(),
                                              aServiceWorker.PrincipalInfo()));

  return StartOp(ClientControlledArgs(aServiceWorker.ToIPC()))
      ->Then(
          mSerialEventTarget, __func__,
          [](const ClientOpResult&) {
            return GenericErrorResultPromise::CreateAndResolve(true, __func__);
          },
          [](const CopyableErrorResult& aRv) {
            return GenericErrorResultPromise::CreateAndReject(aRv, __func__);
          });
}

RefPtr<ClientStatePromise> ClientHandle::Focus(CallerType aCallerType) {
  return StartOp(ClientFocusArgs(aCallerType))
      ->Then(
          mSerialEventTarget, __func__,
          [](const ClientOpResult& aResult) {
            return ClientStatePromise::CreateAndResolve(
                ClientState::FromIPC(aResult.get_IPCClientState()), __func__);
          },
          [](const CopyableErrorResult& aRv) {
            return ClientStatePromise::CreateAndReject(aRv, __func__);
          });
}

RefPtr<GenericErrorResultPromise> ClientHandle::PostMessage(
    NotNull<StructuredCloneData*> aData,
    const ServiceWorkerDescriptor& aSource) {
  if (IsShutdown()) {
    CopyableErrorResult rv;
    rv.ThrowInvalidStateError("Client has been destroyed");
    return GenericErrorResultPromise::CreateAndReject(rv, __func__);
  }

  ClientPostMessageArgs args(/* clonedData */ aData,
                             /* serviceWorker */ aSource.ToIPC());

  return StartOp(std::move(args))
      ->Then(
          mSerialEventTarget, __func__,
          [](const ClientOpResult&) {
            return GenericErrorResultPromise::CreateAndResolve(true, __func__);
          },
          [](const CopyableErrorResult& aRv) {
            return GenericErrorResultPromise::CreateAndReject(aRv, __func__);
          });
}

RefPtr<GenericPromise> ClientHandle::OnDetach() {
  NS_ASSERT_OWNINGTHREAD(ClientHandle);

  if (!mDetachPromise) {
    mDetachPromise = mDetachHolder.Ensure(__func__);
    if (IsShutdown()) {
      mDetachHolder.Resolve(true, __func__);
    }
  }

  return mDetachPromise;
}

void ClientHandle::EvictFromBFCache() { StartOp(ClientEvictBFCacheArgs()); }

}  // namespace mozilla::dom
