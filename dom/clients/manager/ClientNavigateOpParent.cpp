/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ClientNavigateOpParent.h"

namespace mozilla::dom {

using mozilla::ipc::IPCResult;

void ClientNavigateOpParent::ActorDestroy(ActorDestroyReason aReason) {
  CopyableErrorResult rv;
  rv.ThrowAbortError("Client aborted");
  mHolder.RejectIfExists(rv, __func__);
}

IPCResult ClientNavigateOpParent::Recv__delete__(
    const ClientOpResult& aResult) {
  if (aResult.type() == ClientOpResult::TCopyableErrorResult &&
      aResult.get_CopyableErrorResult().Failed()) {
    mHolder.Reject(aResult.get_CopyableErrorResult(), __func__);
    return IPC_OK();
  }
  mHolder.Resolve(aResult, __func__);
  return IPC_OK();
}

ClientNavigateOpParent::ClientNavigateOpParent(
    const ClientNavigateOpConstructorArgs& aArgs)
    : mPromise(mHolder.Ensure(__func__)) {}

ClientNavigateOpParent::~ClientNavigateOpParent() {
  MOZ_DIAGNOSTIC_ASSERT(mHolder.IsEmpty());
}

}  // namespace mozilla::dom
