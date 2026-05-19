/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef _mozilla_dom_ClientHandleOpChild_h
#define _mozilla_dom_ClientHandleOpChild_h

#include "mozilla/dom/ClientOpPromise.h"
#include "mozilla/dom/PClientHandleOpChild.h"

namespace mozilla::dom {

class ClientHandle;

class ClientHandleOpChild final : public PClientHandleOpChild {
  RefPtr<ClientHandle> mClientHandle;
  MozPromiseHolder<ClientOpPromise> mHolder;

  // PClientHandleOpChild interface
  void ActorDestroy(ActorDestroyReason aReason) override;

  mozilla::ipc::IPCResult Recv__delete__(
      const ClientOpResult& aResult) override;

 public:
  ClientHandleOpChild(ClientHandle* aClientHandle,
                      const ClientOpConstructorArgs& aArgs);

  const RefPtr<ClientOpPromise> mPromise;
};

}  // namespace mozilla::dom

#endif  // _mozilla_dom_ClientHandleOpChild_h
