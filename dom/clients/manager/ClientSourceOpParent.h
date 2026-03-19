/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef _mozilla_dom_ClientSourceOpParent_h
#define _mozilla_dom_ClientSourceOpParent_h

#include "mozilla/dom/ClientOpPromise.h"
#include "mozilla/dom/PClientSourceOpParent.h"

namespace mozilla::dom {

class ClientSourceOpParent final : public PClientSourceOpParent {
  const ClientOpConstructorArgs mArgs;
  MozPromiseHolder<ClientOpPromise> mHolder;
  RefPtr<ClientOpPromise> mPromise;

  // PClientSourceOpParent interface
  void ActorDestroy(ActorDestroyReason aReason) override;

  mozilla::ipc::IPCResult Recv__delete__(
      const ClientOpResult& aResult) override;

 public:
  explicit ClientSourceOpParent(ClientOpConstructorArgs&& aArgs);

  const ClientOpConstructorArgs& Args() const { return mArgs; }
  ClientOpPromise* Promise() const { return mPromise; }

  ~ClientSourceOpParent();
};

}  // namespace mozilla::dom

#endif  // _mozilla_dom_ClientSourceOpParent_h
