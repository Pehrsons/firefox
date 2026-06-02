/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/IdentityNetworkHelpers.h"

#include "mozilla/dom/PromiseNativeHandler.h"

namespace mozilla::dom {

RefPtr<MozPromise<IdentityProviderWellKnown, nsresult, true>>
IdentityNetworkHelpers::FetchWellKnownHelper(
    nsIURI* aWellKnown, nsIPrincipal* aTriggeringPrincipal) {
  using PromiseType = MozPromise<IdentityProviderWellKnown, nsresult, true>;
  nsresult rv;
  nsCOMPtr<nsICredentialChooserService> ccService =
      mozilla::components::CredentialChooserService::Service(&rv);
  if (NS_FAILED(rv) || !ccService) {
    return PromiseType::CreateAndReject(rv, __func__);
  }

  RefPtr<Promise> serviceResult;
  rv = ccService->FetchWellKnown(aWellKnown, aTriggeringPrincipal,
                                 getter_AddRefs(serviceResult));
  if (NS_FAILED(rv)) {
    return PromiseType::CreateAndReject(rv, __func__);
  }

  auto handler = MakeRefPtr<MozPromiseNativeHandler<PromiseType>>(
      [](JSContext* aCx, JS::Handle<JS::Value> aValue) -> RefPtr<PromiseType> {
        IdentityProviderWellKnown value;
        if (!value.Init(aCx, aValue)) {
          JS_ClearPendingException(aCx);
          return PromiseType::CreateAndReject(NS_ERROR_INVALID_ARG, __func__);
        }
        return PromiseType::CreateAndResolve(std::move(value), __func__);
      },
      [](JSContext* aCx, JS::Handle<JS::Value> aValue) -> RefPtr<PromiseType> {
        return PromiseType::CreateAndReject(
            Promise::TryExtractNSResultFromRejectionValue(aValue), __func__);
      },
      __func__);
  serviceResult->AppendNativeHandler(handler);
  return handler->Promise();
}

RefPtr<MozPromise<
    std::tuple<Maybe<IdentityProviderWellKnown>, IdentityProviderAPIConfig>,
    nsresult, true>>
IdentityNetworkHelpers::FetchConfigHelper(
    nsIURI* aConfig, nsIPrincipal* aTriggeringPrincipal,
    Maybe<IdentityProviderWellKnown> aWellKnownConfig) {
  using PromiseType = MozPromise<
      std::tuple<Maybe<IdentityProviderWellKnown>, IdentityProviderAPIConfig>,
      nsresult, true>;
  nsresult rv;
  nsCOMPtr<nsICredentialChooserService> ccService =
      mozilla::components::CredentialChooserService::Service(&rv);
  if (NS_FAILED(rv) || !ccService) {
    return PromiseType::CreateAndReject(rv, __func__);
  }

  RefPtr<Promise> serviceResult;
  rv = ccService->FetchConfig(aConfig, aTriggeringPrincipal,
                              getter_AddRefs(serviceResult));
  if (NS_FAILED(rv)) {
    return PromiseType::CreateAndReject(rv, __func__);
  }

  auto handler = MakeRefPtr<MozPromiseNativeHandler<PromiseType>>(
      [aWellKnownConfig = std::move(aWellKnownConfig)](
          JSContext* aCx, JS::Handle<JS::Value> aValue) -> RefPtr<PromiseType> {
        IdentityProviderAPIConfig value;
        if (!value.Init(aCx, aValue)) {
          JS_ClearPendingException(aCx);
          return PromiseType::CreateAndReject(NS_ERROR_INVALID_ARG, __func__);
        }
        return PromiseType::CreateAndResolve(
            std::make_tuple(aWellKnownConfig, value), __func__);
      },
      [](JSContext* aCx, JS::Handle<JS::Value> aValue) -> RefPtr<PromiseType> {
        return PromiseType::CreateAndReject(
            Promise::TryExtractNSResultFromRejectionValue(aValue), __func__);
      },
      __func__);
  serviceResult->AppendNativeHandler(handler);
  return handler->Promise();
}

RefPtr<MozPromise<IdentityProviderAccountList, nsresult, true>>
IdentityNetworkHelpers::FetchAccountsHelper(
    nsIURI* aAccountsEndpoint, nsIPrincipal* aTriggeringPrincipal) {
  using PromiseType = MozPromise<IdentityProviderAccountList, nsresult, true>;
  nsresult rv;
  nsCOMPtr<nsICredentialChooserService> ccService =
      mozilla::components::CredentialChooserService::Service(&rv);
  if (NS_FAILED(rv) || !ccService) {
    return PromiseType::CreateAndReject(rv, __func__);
  }

  RefPtr<Promise> serviceResult;
  rv = ccService->FetchAccounts(aAccountsEndpoint, aTriggeringPrincipal,
                                getter_AddRefs(serviceResult));
  if (NS_FAILED(rv)) {
    return PromiseType::CreateAndReject(rv, __func__);
  }

  auto handler = MakeRefPtr<MozPromiseNativeHandler<PromiseType>>(
      [](JSContext* aCx, JS::Handle<JS::Value> aValue) -> RefPtr<PromiseType> {
        IdentityProviderAccountList value;
        if (!value.Init(aCx, aValue)) {
          JS_ClearPendingException(aCx);
          return PromiseType::CreateAndReject(NS_ERROR_INVALID_ARG, __func__);
        }
        return PromiseType::CreateAndResolve(std::move(value), __func__);
      },
      [](JSContext* aCx, JS::Handle<JS::Value> aValue) -> RefPtr<PromiseType> {
        return PromiseType::CreateAndReject(
            Promise::TryExtractNSResultFromRejectionValue(aValue), __func__);
      },
      __func__);
  serviceResult->AppendNativeHandler(handler);
  return handler->Promise();
}

RefPtr<MozPromise<IdentityAssertionResponse, nsresult, true>>
IdentityNetworkHelpers::FetchTokenHelper(nsIURI* aAccountsEndpoint,
                                         const nsCString& aBody,
                                         nsIPrincipal* aTriggeringPrincipal) {
  using PromiseType = MozPromise<IdentityAssertionResponse, nsresult, true>;
  nsresult rv;
  nsCOMPtr<nsICredentialChooserService> ccService =
      mozilla::components::CredentialChooserService::Service(&rv);
  if (NS_FAILED(rv) || !ccService) {
    return PromiseType::CreateAndReject(rv, __func__);
  }

  RefPtr<Promise> serviceResult;
  rv = ccService->FetchToken(aAccountsEndpoint, aBody.get(),
                             aTriggeringPrincipal,
                             getter_AddRefs(serviceResult));
  if (NS_FAILED(rv)) {
    return PromiseType::CreateAndReject(rv, __func__);
  }

  auto handler = MakeRefPtr<MozPromiseNativeHandler<PromiseType>>(
      [](JSContext* aCx, JS::Handle<JS::Value> aValue) -> RefPtr<PromiseType> {
        IdentityAssertionResponse value;
        if (!value.Init(aCx, aValue)) {
          JS_ClearPendingException(aCx);
          return PromiseType::CreateAndReject(NS_ERROR_INVALID_ARG, __func__);
        }
        return PromiseType::CreateAndResolve(std::move(value), __func__);
      },
      [](JSContext* aCx, JS::Handle<JS::Value> aValue) -> RefPtr<PromiseType> {
        JS_ClearPendingException(aCx);
        return PromiseType::CreateAndReject(
            Promise::TryExtractNSResultFromRejectionValue(aValue), __func__);
      },
      __func__);
  serviceResult->AppendNativeHandler(handler);
  return handler->Promise();
}

RefPtr<MozPromise<DisconnectedAccount, nsresult, true>>
IdentityNetworkHelpers::FetchDisconnectHelper(
    nsIURI* aAccountsEndpoint, const nsCString& aBody,
    nsIPrincipal* aTriggeringPrincipal) {
  using PromiseType = MozPromise<DisconnectedAccount, nsresult, true>;
  nsresult rv;
  nsCOMPtr<nsICredentialChooserService> ccService =
      mozilla::components::CredentialChooserService::Service(&rv);
  if (NS_FAILED(rv) || !ccService) {
    return PromiseType::CreateAndReject(rv, __func__);
  }

  RefPtr<Promise> serviceResult;
  rv = ccService->FetchToken(aAccountsEndpoint, aBody.get(),
                             aTriggeringPrincipal,
                             getter_AddRefs(serviceResult));
  if (NS_FAILED(rv)) {
    return PromiseType::CreateAndReject(rv, __func__);
  }

  auto handler = MakeRefPtr<MozPromiseNativeHandler<PromiseType>>(
      [](JSContext* aCx, JS::Handle<JS::Value> aValue) -> RefPtr<PromiseType> {
        DisconnectedAccount value;
        if (!value.Init(aCx, aValue)) {
          JS_ClearPendingException(aCx);
          return PromiseType::CreateAndReject(NS_ERROR_INVALID_ARG, __func__);
        }
        return PromiseType::CreateAndResolve(std::move(value), __func__);
      },
      [](JSContext* aCx, JS::Handle<JS::Value> aValue) -> RefPtr<PromiseType> {
        JS_ClearPendingException(aCx);
        return PromiseType::CreateAndReject(
            Promise::TryExtractNSResultFromRejectionValue(aValue), __func__);
      },
      __func__);
  serviceResult->AppendNativeHandler(handler);
  return handler->Promise();
}

}  // namespace mozilla::dom
