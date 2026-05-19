/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ClientHandleOpChild.h"

#include "ClientHandle.h"

namespace mozilla::dom {

void ClientHandleOpChild::ActorDestroy(ActorDestroyReason aReason) {
  mClientHandle = nullptr;
  if (!mHolder.IsEmpty()) {
    CopyableErrorResult rv;
    rv.ThrowAbortError("Client load aborted");
    mHolder.Reject(rv, __func__);
  }
}

mozilla::ipc::IPCResult ClientHandleOpChild::Recv__delete__(
    const ClientOpResult& aResult) {
  mClientHandle = nullptr;
  if (aResult.type() == ClientOpResult::TCopyableErrorResult &&
      aResult.get_CopyableErrorResult().Failed()) {
    mHolder.Reject(aResult.get_CopyableErrorResult(), __func__);
    return IPC_OK();
  }
  mHolder.Resolve(aResult, __func__);
  return IPC_OK();
}

ClientHandleOpChild::ClientHandleOpChild(ClientHandle* aClientHandle,
                                         const ClientOpConstructorArgs& aArgs)
    : mClientHandle(aClientHandle), mPromise(mHolder.Ensure(__func__)) {
  MOZ_DIAGNOSTIC_ASSERT(mClientHandle);
}

}  // namespace mozilla::dom
