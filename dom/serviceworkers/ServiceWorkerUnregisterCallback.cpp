/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ServiceWorkerUnregisterCallback.h"

namespace mozilla::dom {

NS_IMPL_ISUPPORTS(UnregisterCallback, nsIServiceWorkerUnregisterCallback)

UnregisterCallback::UnregisterCallback(
    MozPromiseHolder<GenericPromise>&& aPromise)
    : mPromise(std::move(aPromise)) {
  MOZ_DIAGNOSTIC_ASSERT(!mPromise.IsEmpty());
}

NS_IMETHODIMP
UnregisterCallback::UnregisterSucceeded(bool aState) {
  mPromise.Resolve(aState, __func__);
  return NS_OK;
}

NS_IMETHODIMP
UnregisterCallback::UnregisterFailed() {
  mPromise.Reject(NS_ERROR_DOM_SECURITY_ERR, __func__);
  return NS_OK;
}

}  // namespace mozilla::dom
