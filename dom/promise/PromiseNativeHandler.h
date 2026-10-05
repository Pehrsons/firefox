/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_PromiseNativeHandler_h
#define mozilla_dom_PromiseNativeHandler_h

#include <type_traits>

#include "js/TypeDecls.h"
#include "js/Value.h"
#include "mozilla/ErrorResult.h"
#include "mozilla/MoveOnlyFunction.h"
#include "mozilla/MozPromise.h"
#include "mozilla/StaticString.h"
#include "nsISupports.h"

namespace mozilla::dom {

/*
 * PromiseNativeHandler allows C++ to react to a Promise being
 * rejected/resolved. A PromiseNativeHandler can be appended to a Promise using
 * Promise::AppendNativeHandler().
 */
class PromiseNativeHandler : public nsISupports {
 protected:
  virtual ~PromiseNativeHandler() = default;

 public:
  MOZ_CAN_RUN_SCRIPT
  virtual void ResolvedCallback(JSContext* aCx, JS::Handle<JS::Value> aValue,
                                ErrorResult& aRv) = 0;

  MOZ_CAN_RUN_SCRIPT
  virtual void RejectedCallback(JSContext* aCx, JS::Handle<JS::Value> aValue,
                                ErrorResult& aRv) = 0;
};

// This base class exists solely to use NS_IMPL_ISUPPORTS because it doesn't
// support template classes.
class MozPromiseNativeHandlerBase : public PromiseNativeHandler {
  NS_DECL_ISUPPORTS

 protected:
  ~MozPromiseNativeHandlerBase() override = default;
};

// Use this when you subscribe to a JS promise to settle a MozPromise. It
// creates the MozPromise, which is exposed through Promise(). A JS promise is
// not guaranteed to settle, so the MozPromise is rejected with
// NS_BINDING_ABORTED if the handler goes away first.
//
// The handler is not cycle collected, so the SettleFns must not capture objects
// that are, or they might leak.
template <typename PromiseType>
class MozPromiseNativeHandler final : public MozPromiseNativeHandlerBase {
  static_assert(std::is_same_v<typename PromiseType::RejectValueType, nsresult>,
                "The MozPromise must reject with an nsresult");

 public:
  // Called with the value the JS promise resolved or rejected with. Returns the
  // MozPromise to settle the owned MozPromise with.
  using SettleFn =
      MoveOnlyFunction<RefPtr<PromiseType>(JSContext*, JS::Handle<JS::Value>)>;

  MozPromiseNativeHandler(SettleFn&& aResolve, SettleFn&& aReject,
                          StaticString aCallSite)
      : mPromise(mHolder.Ensure(aCallSite)),
        mCallSite(aCallSite),
        mResolve(std::move(aResolve)),
        mReject(std::move(aReject)) {}

  RefPtr<PromiseType> Promise() const { return mPromise; }

  MOZ_CAN_RUN_SCRIPT
  void ResolvedCallback(JSContext* aCx, JS::Handle<JS::Value> aValue,
                        ErrorResult&) override {
    mResolve(aCx, aValue)->ChainTo(std::move(mHolder), mCallSite);
  }

  MOZ_CAN_RUN_SCRIPT
  void RejectedCallback(JSContext* aCx, JS::Handle<JS::Value> aValue,
                        ErrorResult&) override {
    mReject(aCx, aValue)->ChainTo(std::move(mHolder), mCallSite);
  }

 private:
  ~MozPromiseNativeHandler() override {
    mHolder.RejectIfExists(NS_BINDING_ABORTED, mCallSite);
  }

  MozPromiseHolder<PromiseType> mHolder;
  const RefPtr<PromiseType> mPromise;
  const StaticString mCallSite;
  SettleFn mResolve;
  SettleFn mReject;
};

}  // namespace mozilla::dom

#endif  // mozilla_dom_PromiseNativeHandler_h
