/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_workers_WorkerThread_h_
#define mozilla_dom_workers_WorkerThread_h_

#include "mozilla/AbstractThread.h"
#include "mozilla/AlreadyAddRefed.h"
#include "mozilla/CondVar.h"
#include "mozilla/Mutex.h"
#include "mozilla/RefPtr.h"
#include "mozilla/dom/SafeRefPtr.h"
#include "nsISupports.h"
#include "nsThread.h"
#include "nscore.h"

class nsIRunnable;

namespace mozilla {
class Runnable;

namespace dom {

class WorkerRunnable;
class WorkerPrivate;
namespace workerinternals {
class RuntimeService;
}

// This class lets us restrict the public methods that can be called on
// WorkerThread to RuntimeService and WorkerPrivate without letting them gain
// full access to private methods (as would happen if they were simply friends).
class WorkerThreadFriendKey {
  friend class workerinternals::RuntimeService;
  friend class WorkerPrivate;

  WorkerThreadFriendKey();
  ~WorkerThreadFriendKey();
};

class WorkerThread final : public nsThread {
  class Observer;

  Mutex mLock MOZ_UNANNOTATED;
  CondVar mWorkerPrivateCondVar;

  // Protected by nsThread::mLock.
  WorkerPrivate* mWorkerPrivate;

  // Worker thread only. Created on demand by GetAbstractThread(), and detached
  // and released in ClearEventQueueAndWorker(), before the thread goes away.
  // Other threads may hold references to it for longer.
  RefPtr<AbstractThread> mAbstractThread;

  // Only touched on the target thread.
  RefPtr<Observer> mObserver;

  // Protected by nsThread::mLock and waited on with mWorkerPrivateCondVar.
  uint32_t mOtherThreadsDispatchingViaEventTarget;

#ifdef DEBUG
  // Protected by nsThread::mLock.
  bool mAcceptingNonWorkerRunnables;
#endif

  // Using this struct we restrict access to the constructor while still being
  // able to use MakeSafeRefPtr.
  struct ConstructorKey {};

 public:
  explicit WorkerThread(ConstructorKey);

  static SafeRefPtr<WorkerThread> Create(const WorkerThreadFriendKey& aKey);

  void SetWorker(const WorkerThreadFriendKey& aKey,
                 WorkerPrivate* aWorkerPrivate);

  // This method is used to decouple the connection with the WorkerPrivate which
  // is set in SetWorker(). And it also clears all pending runnables on this
  // WorkerThread.
  // After decoupling, WorkerThreadRunnable can not run on this WorkerThread
  // anymore, since WorkerPrivate is invalid.
  void ClearEventQueueAndWorker(const WorkerThreadFriendKey& aKey);

  nsresult DispatchPrimaryRunnable(const WorkerThreadFriendKey& aKey,
                                   already_AddRefed<nsIRunnable> aRunnable);

  nsresult DispatchAnyThread(const WorkerThreadFriendKey& aKey,
                             RefPtr<WorkerRunnable> aWorkerRunnable);

  uint32_t RecursionDepth(const WorkerThreadFriendKey& aKey) const;

  /**
   * The AbstractThread for this thread, for the few places that need tail
   * dispatch on a worker, e.g., for talking to the MediaTrackGraph or mirroring
   * state from it. Not for general worker tasks. Created on first use, after
   * which AbstractThread::GetCurrent() returns it on this thread.
   *
   * Tasks running on this thread may use its tail dispatcher whether or not
   * they were dispatched through it. Tasks queued in it go out when the task
   * running from the thread's event loop, i.e., through
   * nsThread::ProcessNextEvent(), finishes. That covers WorkerRunnables and
   * runnables dispatched through the worker's hybrid event target. The
   * worker's run loop itself runs from such a task, so code running outside
   * of a worker task, like WorkerControlRunnables and worker shutdown, may
   * also queue tasks; they go out when the next worker task starts or, at
   * the latest, when the wrapper is detached before the thread goes away.
   *
   * Worker thread only.
   */
  AbstractThread* GetAbstractThread();

  // Override HasPendingEvents to allow HasPendingEvents could be accessed by
  // the parent thread. WorkerPrivate::IsEligibleForCC calls this method on the
  // parent thread to check if there is any pending events on the worker thread.
  NS_IMETHOD HasPendingEvents(bool* aHasPendingEvents) override;

  NS_INLINE_DECL_REFCOUNTING_INHERITED(WorkerThread, nsThread)

 private:
  ~WorkerThread();

  // This should only be called by consumers that have an
  // nsIEventTarget/nsIThread pointer.
  NS_IMETHOD
  Dispatch(already_AddRefed<nsIRunnable> aRunnable,
           DispatchFlags aFlags) override;

  NS_IMETHOD
  DispatchFromScript(nsIRunnable* aRunnable, DispatchFlags aFlags) override;

  NS_IMETHOD
  DelayedDispatch(already_AddRefed<nsIRunnable>, uint32_t) override;
};

}  // namespace dom
}  // namespace mozilla

#endif  // mozilla_dom_workers_WorkerThread_h_
