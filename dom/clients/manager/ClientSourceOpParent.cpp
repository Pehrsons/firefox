/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ClientSourceOpParent.h"

#include "ClientSourceParent.h"

namespace mozilla::dom {

using mozilla::ipc::IPCResult;

void ClientSourceOpParent::ActorDestroy(ActorDestroyReason aReason) {
  CopyableErrorResult rv;
  rv.ThrowAbortError("Client torn down");
  mHolder.RejectIfExists(rv, __func__);
}

IPCResult ClientSourceOpParent::Recv__delete__(const ClientOpResult& aResult) {
  if (aResult.type() == ClientOpResult::TCopyableErrorResult &&
      aResult.get_CopyableErrorResult().Failed()) {
    // If a control message fails then clear the controller from
    // the ClientSourceParent.  We eagerly marked it controlled at
    // the start of the operation.
    if (mArgs.type() == ClientOpConstructorArgs::TClientControlledArgs) {
      auto source = static_cast<ClientSourceParent*>(Manager());
      if (source) {
        source->ClearController();
      }
    }

    mHolder.Reject(aResult.get_CopyableErrorResult(), __func__);
    return IPC_OK();
  }

  mHolder.Resolve(aResult, __func__);
  return IPC_OK();
}

ClientSourceOpParent::ClientSourceOpParent(ClientOpConstructorArgs&& aArgs)
    : mArgs(std::move(aArgs)), mPromise(mHolder.Ensure(__func__)) {}

ClientSourceOpParent::~ClientSourceOpParent() {
  MOZ_DIAGNOSTIC_ASSERT(mHolder.IsEmpty());
}

}  // namespace mozilla::dom
