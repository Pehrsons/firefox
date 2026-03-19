/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef _mozilla_dom_ClientNavigateOpParent_h
#define _mozilla_dom_ClientNavigateOpParent_h

#include "mozilla/dom/ClientOpPromise.h"
#include "mozilla/dom/PClientNavigateOpParent.h"

namespace mozilla::dom {

class ClientNavigateOpParent final : public PClientNavigateOpParent {
  MozPromiseHolder<ClientOpPromise> mHolder;
  RefPtr<ClientOpPromise> mPromise;

  // PClientNavigateOpParent interface
  void ActorDestroy(ActorDestroyReason aReason) override;

  mozilla::ipc::IPCResult Recv__delete__(
      const ClientOpResult& aResult) override;

 public:
  explicit ClientNavigateOpParent(const ClientNavigateOpConstructorArgs& aArgs);

  ~ClientNavigateOpParent();

  ClientOpPromise* Promise() const { return mPromise; }
};

}  // namespace mozilla::dom

#endif  // _mozilla_dom_ClientNavigateOpParent_h
