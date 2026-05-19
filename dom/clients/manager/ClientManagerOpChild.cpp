/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ClientManagerOpChild.h"

#include "mozilla/dom/ClientManager.h"
#include "mozilla/ipc/ProtocolUtils.h"

namespace mozilla::dom {

void ClientManagerOpChild::ActorDestroy(ActorDestroyReason aReason) {
  mClientManager = nullptr;
  if (!mHolder.IsEmpty()) {
    CopyableErrorResult rv;
    rv.ThrowAbortError("Client aborted");
    mHolder.Reject(rv, __func__);
  }
}

mozilla::ipc::IPCResult ClientManagerOpChild::Recv__delete__(
    const ClientOpResult& aResult) {
  mClientManager = nullptr;
  if (aResult.type() == ClientOpResult::TCopyableErrorResult &&
      aResult.get_CopyableErrorResult().Failed()) {
    mHolder.Reject(aResult.get_CopyableErrorResult(), __func__);
    return IPC_OK();
  }
  mHolder.Resolve(aResult, __func__);
  return IPC_OK();
}

ClientManagerOpChild::ClientManagerOpChild(ClientManager* aClientManager,
                                           const ClientOpConstructorArgs& aArgs)
    : mClientManager(aClientManager), mPromise(mHolder.Ensure(__func__)) {
  MOZ_DIAGNOSTIC_ASSERT(mClientManager);
}

}  // namespace mozilla::dom
