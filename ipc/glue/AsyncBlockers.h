/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_ipc_AsyncBlockers_h
#define mozilla_ipc_AsyncBlockers_h

#include "MainThreadUtils.h"
#include "mozilla/MozPromise.h"
#include "mozilla/ThreadSafety.h"
#include "nsITimer.h"
#include "nsTArray.h"
#include "nsThreadUtils.h"

// FIXME: when bug 1760855 is fixed, it should not be required anymore

namespace mozilla::ipc {

/**
 * AsyncBlockers provide a simple registration service that allows to suspend
 * completion of a particular task until all registered entries have been
 * cleared. This can be used to implement a similar service to
 * nsAsyncShutdownService in processes where it wouldn't normally be available.
 *
 * Register() and Deregister() are thread-safe. The destructor and
 * WaitUntilClear() must run on the main thread. The constructor is not
 * checked since owners are created before the main thread's event target is
 * available.
 */
class AsyncBlockers {
 public:
  AsyncBlockers() : mLock("AsyncRegistrar") {}

  void Register(void* aBlocker) {
    MutexAutoLock lock(mLock);
    mBlockers.InsertElementSorted(aBlocker);
  }

  void Deregister(void* aBlocker) {
    MutexAutoLock lock(mLock);
    MOZ_ASSERT(mBlockers.ContainsSorted(aBlocker));
    MOZ_ALWAYS_TRUE(mBlockers.RemoveElementSorted(aBlocker));
    if (mBlockers.IsEmpty()) {
      mHolder.ResolveIfExists(true, __func__);
    }
  }

  // The returned promise is resolved once no blockers are registered or, if
  // aTimeOutInMs is not 0, after that many milliseconds, whichever comes first.
  RefPtr<GenericPromise> WaitUntilClear(uint32_t aTimeOutInMs = 0) {
    AssertIsOnMainThread();
    RefPtr<GenericPromise> promise;
    {
      MutexAutoLock lock(mLock);
      if (mBlockers.IsEmpty()) {
        return GenericPromise::CreateAndResolve(true, __func__);
      }
      promise = mHolder.Ensure(__func__);
    }

    if (aTimeOutInMs > 0) {
      if (mTimer) {
        mTimer->Cancel();
      }
      // The timer fires on, and is cancelled from, the main thread, so the
      // callback cannot run after this object has been destroyed.
      (void)NS_WARN_IF(NS_FAILED(NS_NewTimerWithFuncCallback(
          getter_AddRefs(mTimer),
          [](nsITimer*, void* aClosure) {
            static_cast<AsyncBlockers*>(aClosure)->OnTimeout();
          },
          this, aTimeOutInMs, nsITimer::TYPE_ONE_SHOT,
          "AsyncBlockers::WaitUntilClear"_ns)));
    }

    return promise;
  }

  virtual ~AsyncBlockers() {
    AssertIsOnMainThread();
    if (mTimer) {
      mTimer->Cancel();
    }
    mHolder.ResolveIfExists(true, __func__);
  }

 private:
  void OnTimeout() {
    AssertIsOnMainThread();
    mTimer = nullptr;
    MutexAutoLock lock(mLock);
    mHolder.ResolveIfExists(true, __func__);
  }

  Mutex mLock;
  nsTArray<void*> mBlockers MOZ_GUARDED_BY(mLock);
  MozPromiseHolder<GenericPromise> mHolder MOZ_GUARDED_BY(mLock);
  nsCOMPtr<nsITimer> mTimer MOZ_GUARDED_BY(sMainThreadCapability);
};

}  // namespace mozilla::ipc

#endif  // mozilla_ipc_AsyncBlockers_h
