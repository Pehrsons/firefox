/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "IdentityCredentialRequestManager.h"
#include "mozilla/ClearOnShutdown.h"
#include "nsContentUtils.h"

namespace mozilla {

NS_IMPL_ISUPPORTS0(IdentityCredentialRequestManager);

StaticRefPtr<IdentityCredentialRequestManager>
    IdentityCredentialRequestManager::sSingleton;

// static
IdentityCredentialRequestManager*
IdentityCredentialRequestManager::GetInstance() {
  if (!sSingleton) {
    sSingleton = new IdentityCredentialRequestManager();
    ClearOnShutdown(&sSingleton);
  }
  return sSingleton;
}

RefPtr<MozPromise<std::tuple<nsCString, Maybe<nsCString>>, nsresult, true>>
IdentityCredentialRequestManager::GetTokenFromPopup(
    dom::WebIdentityParent* aRelyingPartyWindow, nsIURI* aURLToOpen) {
  MOZ_ASSERT(aRelyingPartyWindow);
  MOZ_ASSERT(aURLToOpen);

  NotNull<nsIURI*> uri = WrapNotNull(aURLToOpen);
  RefPtr<IdentityCredentialRequestManager> self = this;

  // Tell the RP child to open an IDP popup.
  // It will either resolve with a failing nsresult or the popup BC.
  // The returned promise is resolved *after* the child process opens the
  // window.
  return aRelyingPartyWindow->SendOpenContinuationWindow(uri)->Then(
      GetCurrentSerialEventTarget(), __func__,
      [self](dom::PWebIdentityParent::OpenContinuationWindowPromise::
                 ResolveOrRejectValue&& aValue) -> RefPtr<TokenPromise> {
        if (aValue.IsReject()) {
          return TokenPromise::CreateAndReject(NS_ERROR_DOM_NETWORK_ERR,
                                               __func__);
        }
        const auto& response = aValue.ResolveValue();
        // If it failed, reject now, rejecting the RP child's initial call.
        if (response.type() == dom::OpenContinuationWindowResponse::Tnsresult) {
          return TokenPromise::CreateAndReject(response.get_nsresult(),
                                               __func__);
        }
        // If we have a BC, a popup opened.
        const dom::MaybeDiscardedBrowsingContext& bc =
            response.get_MaybeDiscardedBrowsingContext();
        if (bc.IsNullOrDiscarded()) {
          return TokenPromise::CreateAndReject(NS_ERROR_DOM_NETWORK_ERR,
                                               __func__);
        }
        // Transform the BC into its top-chrome-window-bc, so we have
        // something stable through navigation and can listen for the popup's
        // close.
        dom::CanonicalBrowsingContext* chromeBC =
            bc.get_canonical()->TopCrossChromeBoundary();
        if (!chromeBC) {
          return TokenPromise::CreateAndReject(NS_ERROR_DOM_NETWORK_ERR,
                                               __func__);
        }

        // There really shouldn't be more than one request per top window
        MOZ_ASSERT(!self->mPendingTokenRequests.Contains(chromeBC->Id()));

        // Insert a holder so we can settle the promise later, findable by BC
        // id.
        MozPromiseHolder<TokenPromise> holder;
        RefPtr<TokenPromise> promise = holder.Ensure(__func__);
        self->mPendingTokenRequests.InsertOrUpdate(chromeBC->Id(),
                                                   std::move(holder));

        // If the window closes before we have a chance to resolve it,
        // remove the promise from our map and reject it.
        chromeBC->AddFinalDiscardListener([self](uint64_t id) {
          auto pending = self->mPendingTokenRequests.Extract(id);
          // If it already settled before the window closed, just drop the
          // holder.
          if (pending.isNothing()) {
            return;
          }
          pending->Reject(NS_ERROR_DOM_NETWORK_ERR, __func__);
        });
        return promise;
      });
}

nsresult IdentityCredentialRequestManager::MaybeResolvePopup(
    dom::WebIdentityParent* aPopupWindow, const nsCString& aToken,
    const dom::IdentityResolveOptions& aOptions) {
  // aPopupWindow is (theoretically) a popup window opened by
  // SendOpenContinuationWindow. So try to get its top chrome bc so we can  look
  // into the map!
  dom::WindowGlobalParent* manager =
      static_cast<dom::WindowGlobalParent*>(aPopupWindow->Manager());
  if (!manager) {
    return NS_ERROR_DOM_NOT_ALLOWED_ERR;
  }
  dom::CanonicalBrowsingContext* bc = manager->BrowsingContext();
  if (!bc) {
    return NS_ERROR_DOM_NOT_ALLOWED_ERR;
  }
  dom::CanonicalBrowsingContext* chromeBC = bc->TopCrossChromeBoundary();
  if (!chromeBC) {
    return NS_ERROR_DOM_NOT_ALLOWED_ERR;
  }

  // Get its entry, removing it from the map.
  auto pendingHolder = mPendingTokenRequests.Extract(chromeBC->Id());

  // This will be Nothing if the function was called on a window not opened by
  // SendOpenContinuationWindow. This error will be forwarded along to the JS
  // caller.
  if (!pendingHolder.isSome()) {
    return NS_ERROR_DOM_NOT_ALLOWED_ERR;
  }
  // Convert the Optional to a Maybe and send a successful response to the RP
  // window that opened this popup.
  Maybe<nsCString> overrideAccountId = Nothing();
  if (aOptions.mAccountId.WasPassed()) {
    overrideAccountId = Some(aOptions.mAccountId.Value());
  }
  pendingHolder->Resolve(std::make_tuple(aToken, overrideAccountId), __func__);
  return NS_OK;
}

bool IdentityCredentialRequestManager::IsActivePopup(
    dom::WebIdentityParent* aPopupWindow) {
  dom::WindowGlobalParent* manager =
      static_cast<dom::WindowGlobalParent*>(aPopupWindow->Manager());
  if (!manager) {
    return false;
  }
  dom::CanonicalBrowsingContext* bc = manager->BrowsingContext();
  if (!bc) {
    return false;
  }
  // Transform the BC ID into its top-chrome-window-bc, so we have
  // something stable through navigation and can listen for the popup's close.
  dom::CanonicalBrowsingContext* chromeBC = bc->TopCrossChromeBoundary();
  if (!chromeBC) {
    return false;
  }
  return mPendingTokenRequests.Contains(chromeBC->Id());
}

}  // namespace mozilla
