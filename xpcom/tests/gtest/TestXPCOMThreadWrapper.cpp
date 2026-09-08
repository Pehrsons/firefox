/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <atomic>

#include "VideoUtils.h"
#include "gtest/gtest.h"
#include "mozilla/AbstractThread.h"
#include "mozilla/SharedThreadPool.h"
#include "mozilla/TaskDispatcher.h"
#include "mozilla/TaskQueue.h"
#include "nsIThreadInternal.h"
#include "nsThreadUtils.h"

using namespace mozilla;

namespace {

// An XPCOM thread with an XPCOMThreadWrapper created on it, the way
// WorkerThread::GetAbstractThread() does. Tasks in these tests are dispatched
// with the plain nsIThread API, not through the wrapper, to cover tail dispatch
// from tasks the wrapper knows nothing about.
class XPCOMThreadWrapperTest : public ::testing::Test {
 protected:
  void SetUp() override {
    MOZ_ALWAYS_SUCCEEDS(
        NS_NewNamedThread("XPCOMThreadWrap", getter_AddRefs(mThread)));
    RunOnThread([this] {
      nsCOMPtr<nsIThreadInternal> internal = do_QueryInterface(mThread);
      ASSERT_TRUE(internal);
      ASSERT_FALSE(AbstractThread::GetCurrent());
      mWrapper = AbstractThread::CreateXPCOMThreadWrapper(
          internal, TailDispatchPolicy::ConsistentOrdering,
          /* aOnThread = */ true);
      ASSERT_EQ(AbstractThread::GetCurrent(), mWrapper.get());
    });
  }

  void TearDown() override {
    if (mWrapper) {
      RunOnThread([this] {
        mWrapper = nullptr;
        ASSERT_FALSE(AbstractThread::GetCurrent());
      });
    }
    if (mThread) {
      MOZ_ALWAYS_SUCCEEDS(mThread->Shutdown());
    }
  }

  // Runs aFunction on mThread as a plain task and waits for it to finish, so
  // that everything queued during aFunction, including its tail-dispatched
  // tasks, has run when this returns.
  template <typename Function>
  void RunOnThread(Function&& aFunction) {
    MOZ_ALWAYS_SUCCEEDS(NS_DispatchAndSpinEventLoopUntilComplete(
        "XPCOMThreadWrapperTest::RunOnThread"_ns, mThread,
        NS_NewRunnableFunction(__func__, std::forward<Function>(aFunction))));
  }

  nsCOMPtr<nsIThread> mThread;
  RefPtr<AbstractThread> mWrapper;
};

}  // namespace

TEST_F(XPCOMThreadWrapperTest, DirectTaskFromPlainTask) {
  // Only touched on mThread until Shutdown() in TearDown().
  nsTArray<int> order;
  RunOnThread([&] {
    AbstractThread* current = AbstractThread::GetCurrent();
    ASSERT_EQ(current, mWrapper.get());
    ASSERT_TRUE(current->IsTailDispatcherAvailable());

    // Queued before the direct task is added, but must run after it.
    MOZ_ALWAYS_SUCCEEDS(mThread->Dispatch(
        NS_NewRunnableFunction("NextTask", [&] { order.AppendElement(3); })));
    current->TailDispatcher().AddDirectTask(
        NS_NewRunnableFunction("DirectTask", [&] { order.AppendElement(2); }));
    order.AppendElement(1);
  });
  RunOnThread([&] {
    // Runs after "NextTask" since dispatch is FIFO.
    EXPECT_EQ(order, (nsTArray<int>{1, 2, 3}));
  });
}

TEST_F(XPCOMThreadWrapperTest, DirectTaskRunsBeforeNestedEventLoop) {
  nsTArray<int> order;
  RunOnThread([&] {
    AbstractThread* current = AbstractThread::GetCurrent();
    current->TailDispatcher().AddDirectTask(
        NS_NewRunnableFunction("DirectTask", [&] { order.AppendElement(1); }));
    // Spinning a nested event loop fires the tail dispatcher first.
    MOZ_ALWAYS_SUCCEEDS(NS_DispatchAndSpinEventLoopUntilComplete(
        "Nested"_ns, mThread,
        NS_NewRunnableFunction("Nested", [&] { order.AppendElement(2); })));
    order.AppendElement(3);
  });
  RunOnThread([&] { EXPECT_EQ(order, (nsTArray<int>{1, 2, 3})); });
}

TEST_F(XPCOMThreadWrapperTest, DispatchToTaskQueueIsTailDispatched) {
  RefPtr<TaskQueue> queue = TaskQueue::Create(
      GetMediaThreadPool(MediaThreadType::SUPERVISOR), "TestXPCOMThreadWrapper",
      TailDispatchPolicy::ConsistentOrdering);
  std::atomic<bool> taskFinished{false};
  std::atomic<bool> ranOnQueue{false};
  std::atomic<bool> sawTaskFinished{false};
  RunOnThread([&] {
    // A plain dispatch to an AbstractThread supporting tail dispatch, from a
    // plain task on the wrapped thread, is tail dispatched: it goes out when
    // this task finishes, so the queue can only see taskFinished set.
    MOZ_ALWAYS_SUCCEEDS(queue->Dispatch(NS_NewRunnableFunction("OnQueue", [&] {
      sawTaskFinished = taskFinished.load();
      ranOnQueue = true;
    })));
    taskFinished = true;
  });
  // The tail dispatcher fires after the task above has returned, so wait for
  // the next task on the thread before waiting for the queue.
  RunOnThread([] {});
  queue->AwaitIdle();
  EXPECT_TRUE(ranOnQueue);
  EXPECT_TRUE(sawTaskFinished);
  queue->BeginShutdown();
  queue->AwaitShutdownAndIdle();
}

TEST_F(XPCOMThreadWrapperTest, DetachedWrapperIsReleasedOnAnotherThread) {
  // Another thread holding the wrapper, like a mirror of state on the wrapped
  // thread would.
  RefPtr<AbstractThread> otherRef = mWrapper;
  RunOnThread([this] {
    AbstractThread::DetachXPCOMThreadWrapper(mWrapper);
    EXPECT_FALSE(AbstractThread::GetCurrent());
    mWrapper = nullptr;
  });
  MOZ_ALWAYS_SUCCEEDS(mThread->Shutdown());
  mThread = nullptr;
  // The last reference goes away here, on the main thread, after the wrapped
  // thread has shut down.
  otherRef = nullptr;
}
