/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef StorageAccessPermissionRequest_h_
#define StorageAccessPermissionRequest_h_

#include "mozilla/MoveOnlyFunction.h"
#include "mozilla/MozPromise.h"
#include "nsContentPermissionHelper.h"

namespace mozilla::dom {

class StorageAccessPermissionRequest final
    : public ContentPermissionRequestBase {
 public:
  NS_DECL_ISUPPORTS_INHERITED
  NS_DECL_CYCLE_COLLECTION_CLASS_INHERITED(StorageAccessPermissionRequest,
                                           ContentPermissionRequestBase)

  // nsIContentPermissionRequest
  NS_IMETHOD Cancel(void) override;
  NS_IMETHOD Allow(JS::Handle<JS::Value> choices) override;
  NS_IMETHOD GetTypes(nsIArray** aTypes) override;

  enum class Response { Allow, Cancel };
  using Callback = MoveOnlyFunction<void(Response)>;

  static already_AddRefed<StorageAccessPermissionRequest> Create(
      nsPIDOMWindowInner* aWindow, Callback&& aCallback);

  static already_AddRefed<StorageAccessPermissionRequest> Create(
      nsPIDOMWindowInner* aWindow, nsIPrincipal* aPrincipal,
      Callback&& aCallback);

  static already_AddRefed<StorageAccessPermissionRequest> Create(
      nsPIDOMWindowInner* aWindow, nsIPrincipal* aPrincipal, bool aFrameOnly,
      Callback&& aCallback);

  using AutoGrantDelayPromise = MozPromise<bool, bool, true>;
  RefPtr<AutoGrantDelayPromise> MaybeDelayAutomaticGrants();

 private:
  StorageAccessPermissionRequest(nsPIDOMWindowInner* aWindow,
                                 nsIPrincipal* aNodePrincipal, bool aFrameOnly,
                                 Callback&& aCallback);
  ~StorageAccessPermissionRequest() {
    // Invoke Cancel() to ensure we call a callback even if the request has
    // been destroyed before the request is completed.
    Cancel();
  }

  unsigned CalculateSimulatedDelay();

  Callback mCallback;
  nsTArray<nsString> mOptions;
  nsTArray<PermissionRequest> mPermissionRequests;
  bool mCallbackCalled;
};

}  // namespace mozilla::dom

#endif  // StorageAccessPermissionRequest_h_
