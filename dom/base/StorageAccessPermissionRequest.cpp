/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "StorageAccessPermissionRequest.h"

#include <cstdlib>

#include "mozilla/StaticPrefs_dom.h"
#include "nsGlobalWindowInner.h"

namespace mozilla::dom {

NS_IMPL_CYCLE_COLLECTION_INHERITED(StorageAccessPermissionRequest,
                                   ContentPermissionRequestBase)

NS_IMPL_ISUPPORTS_CYCLE_COLLECTION_INHERITED_0(StorageAccessPermissionRequest,
                                               ContentPermissionRequestBase)

StorageAccessPermissionRequest::StorageAccessPermissionRequest(
    nsPIDOMWindowInner* aWindow, nsIPrincipal* aNodePrincipal, bool aFrameOnly,
    Callback&& aCallback)
    : ContentPermissionRequestBase(aNodePrincipal, aWindow,
                                   "dom.storage_access"_ns,
                                   "storage-access"_ns),
      mCallback(std::move(aCallback)),
      mCallbackCalled(false) {
  mOptions.SetLength(2);
  // Location 0 is no longer sent
  if (aFrameOnly) {
    mOptions.ElementAt(1) = u"1"_ns;
  }
  mPermissionRequests.AppendElement(PermissionRequest(mType, mOptions));
}

NS_IMETHODIMP
StorageAccessPermissionRequest::Cancel() {
  if (!mCallbackCalled) {
    mCallbackCalled = true;
    mCallback(Response::Cancel);
  }
  return NS_OK;
}

NS_IMETHODIMP
StorageAccessPermissionRequest::Allow(JS::Handle<JS::Value> aChoices) {
  nsTArray<PermissionChoice> choices;
  nsresult rv = TranslateChoices(aChoices, mPermissionRequests, choices);
  if (NS_FAILED(rv)) {
    return rv;
  }

  // There is no support to allow grants automatically from the prompting code
  // path.

  if (!mCallbackCalled) {
    mCallbackCalled = true;
    if (choices.Length() == 1 && choices[0].choice().EqualsLiteral("allow")) {
      mCallback(Response::Allow);
    }
  }
  return NS_OK;
}

NS_IMETHODIMP
StorageAccessPermissionRequest::GetTypes(nsIArray** aTypes) {
  return nsContentPermissionUtils::CreatePermissionArray(mType, mOptions,
                                                         aTypes);
}

RefPtr<StorageAccessPermissionRequest::AutoGrantDelayPromise>
StorageAccessPermissionRequest::MaybeDelayAutomaticGrants() {
  MozPromiseHolder<AutoGrantDelayPromise> holder;
  RefPtr<AutoGrantDelayPromise> p = holder.Ensure(__func__);

  unsigned simulatedDelay = CalculateSimulatedDelay();
  if (simulatedDelay) {
    // Rejects the promise if the timer callback is destroyed without firing,
    // e.g. when the timer could not be created or is cancelled at shutdown.
    struct DelayHolder {
      explicit DelayHolder(MozPromiseHolder<AutoGrantDelayPromise>&& aHolder)
          : mHolder(std::move(aHolder)) {}
      DelayHolder(DelayHolder&&) = default;
      ~DelayHolder() { mHolder.RejectIfExists(false, __func__); }
      MozPromiseHolder<AutoGrantDelayPromise> mHolder;
    };
    nsCOMPtr<nsITimer> timer;
    nsresult rv = NS_NewTimerWithCallback(
        getter_AddRefs(timer),
        [delay = DelayHolder(std::move(holder))](nsITimer* aTimer) mutable {
          delay.mHolder.Resolve(true, __func__);
          NS_RELEASE(aTimer);
        },
        simulatedDelay, nsITimer::TYPE_ONE_SHOT,
        "DelayedAllowAutoGrantCallback"_ns);
    if (!NS_WARN_IF(NS_FAILED(rv))) {
      // Leak the timer reference; it will be released inside the callback.
      timer.forget().leak();
    }
  } else {
    holder.Resolve(false, __func__);
  }
  return p;
}

already_AddRefed<StorageAccessPermissionRequest>
StorageAccessPermissionRequest::Create(nsPIDOMWindowInner* aWindow,
                                       Callback&& aCallback) {
  if (!aWindow) {
    return nullptr;
  }
  nsGlobalWindowInner* win = nsGlobalWindowInner::Cast(aWindow);

  return Create(aWindow, win->GetPrincipal(), std::move(aCallback));
}

already_AddRefed<StorageAccessPermissionRequest>
StorageAccessPermissionRequest::Create(nsPIDOMWindowInner* aWindow,
                                       nsIPrincipal* aPrincipal,
                                       Callback&& aCallback) {
  return Create(aWindow, aPrincipal, true, std::move(aCallback));
}

already_AddRefed<StorageAccessPermissionRequest>
StorageAccessPermissionRequest::Create(nsPIDOMWindowInner* aWindow,
                                       nsIPrincipal* aPrincipal,
                                       bool aFrameOnly, Callback&& aCallback) {
  if (!aWindow) {
    return nullptr;
  }

  if (!aPrincipal) {
    return nullptr;
  }

  RefPtr<StorageAccessPermissionRequest> request =
      new StorageAccessPermissionRequest(aWindow, aPrincipal, aFrameOnly,
                                         std::move(aCallback));
  return request.forget();
}

unsigned StorageAccessPermissionRequest::CalculateSimulatedDelay() {
  if (!StaticPrefs::dom_storage_access_auto_grants_delayed()) {
    return 0;
  }

  // Generate a random time value that is at least 0 and and most 3 seconds.
  std::srand(static_cast<unsigned>(PR_Now()));

  const unsigned kMin = 0;
  const unsigned kMax = 3000;
  const unsigned random = std::abs(std::rand());

  return kMin + random % (kMax - kMin);
}

}  // namespace mozilla::dom
