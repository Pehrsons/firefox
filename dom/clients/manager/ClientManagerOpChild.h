/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef _mozilla_dom_ClientManagerOpChild_h
#define _mozilla_dom_ClientManagerOpChild_h

#include "mozilla/MozPromise.h"
#include "mozilla/dom/ClientOpPromise.h"
#include "mozilla/dom/PClientManagerOpChild.h"

namespace mozilla::dom {

class ClientManager;

class ClientManagerOpChild final : public PClientManagerOpChild {
  RefPtr<ClientManager> mClientManager;
  MozPromiseHolder<ClientOpPromise> mHolder;

  // PClientManagerOpChild interface
  void ActorDestroy(ActorDestroyReason aReason) override;

  mozilla::ipc::IPCResult Recv__delete__(
      const ClientOpResult& aResult) override;

 public:
  ClientManagerOpChild(ClientManager* aClientManager,
                       const ClientOpConstructorArgs& aArgs);

  const RefPtr<ClientOpPromise> mPromise;
};

}  // namespace mozilla::dom

#endif  // _mozilla_dom_ClientManagerOpChild_h
