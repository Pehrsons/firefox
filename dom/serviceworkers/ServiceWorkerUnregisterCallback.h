/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_ServiceWorkerUnregisterCallback_h
#define mozilla_dom_ServiceWorkerUnregisterCallback_h

#include "mozilla/MozPromise.h"
#include "nsIServiceWorkerManager.h"

namespace mozilla::dom {

class UnregisterCallback final : public nsIServiceWorkerUnregisterCallback {
 public:
  NS_DECL_ISUPPORTS

  explicit UnregisterCallback(MozPromiseHolder<GenericPromise>&& aPromise);

  // nsIServiceWorkerUnregisterCallback implementation
  NS_IMETHOD
  UnregisterSucceeded(bool aState) override;

  NS_IMETHOD
  UnregisterFailed() override;

 private:
  ~UnregisterCallback() = default;

  MozPromiseHolder<GenericPromise> mPromise;
};

}  // namespace mozilla::dom

#endif  // mozilla_dom_ServiceWorkerUnregisterCallback_h
