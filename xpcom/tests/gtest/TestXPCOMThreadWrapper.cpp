/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <atomic>
#include <functional>

#include "VideoUtils.h"
#include "gtest/gtest.h"
#include "mozilla/AbstractThread.h"
#include "mozilla/SharedThreadPool.h"
#include "mozilla/SpinEventLoopUntil.h"
#include "mozilla/TaskDispatcher.h"
#include "mozilla/TaskQueue.h"
#include "nsIThreadInternal.h"
#include "nsThreadUtils.h"

using namespace mozilla;

namespace {

// An XPCOM thread running a primary task that holds an AutoXPCOMThreadWrapper
// and spins a nested event loop, the way WorkerThreadPrimaryRunnable does.
// Tasks in these tests are dispatched with the plain nsIThread API, not through
// the wrapper, to cover tail dispatch from tasks the wrapper knows nothing
// about.
class XPCOMThreadWrapperTest : public ::testing::Test {
 protected:
  void SetUp() override {
    MOZ_ALWAYS_SUCCEEDS(
        NS_NewNamedThread("XPCOMThreadWrap", getter_AddRefs(mThread)));
    StartPrimary();
  }

  void TearDown() override {
    StopPrimary();
    MOZ_ALWAYS_SUCCEEDS(mThread->Shutdown());
  }

  void StartPrimary() {
    MOZ_ASSERT(!mPrimaryRunning);
    mStopPrimary = false;
    mPrimaryRunning = true;
    MOZ_ALWAYS_SUCCEEDS(
        mThread->Dispatch(NS_NewRunnableFunction("Primary", [this] {
          nsCOMPtr<nsIThreadInternal> internal = do_QueryInterface(mThread);
          ASSERT_TRUE(internal);
          ASSERT_FALSE(AbstractThread::GetCurrent());
          {
            AutoXPCOMThreadWrapper wrapper(
                internal, TailDispatchPolicy::ConsistentOrdering);
            ASSERT_EQ(AbstractThread::GetCurrent(), wrapper.get());
            mWrapper = wrapper.get();
            MOZ_ALWAYS_TRUE(
                SpinEventLoopUntil("XPCOMThreadWrapperTest::Primary"_ns,
                                   [this] { return mStopPrimary.load(); }));
            if (mBeforeUnregister) {
              mBeforeUnregister();
            }
          }
          EXPECT_FALSE(AbstractThread::GetCurrent());
          mPrimaryRunning = false;
        })));
    MOZ_ALWAYS_TRUE(SpinEventLoopUntil("XPCOMThreadWrapperTest::Started"_ns,
                                       [this] { return !!mWrapper; }));
  }

  void StopPrimary() {
    if (!mPrimaryRunning) {
      return;
    }
    mStopPrimary = true;
    // Wakes the primary's event loop to see mStopPrimary.
    MOZ_ALWAYS_SUCCEEDS(
        mThread->Dispatch(NS_NewRunnableFunction("Wake", [] {})));
    MOZ_ALWAYS_TRUE(SpinEventLoopUntil("XPCOMThreadWrapperTest::Stopped"_ns,
                                       [this] { return !mPrimaryRunning; }));
    mWrapper = nullptr;
  }

  // Runs aFunction on mThread as a plain task and waits for it to finish, so
  // that everything queued during aFunction, including its tail-dispatched
  // tasks, has run when this returns. While the primary task runs, aFunction
  // runs in its nested event loop.
  template <typename Function>
  void RunOnThread(Function&& aFunction) {
    MOZ_ALWAYS_SUCCEEDS(NS_DispatchAndSpinEventLoopUntilComplete(
        "XPCOMThreadWrapperTest::RunOnThread"_ns, mThread,
        NS_NewRunnableFunction(__func__, std::forward<Function>(aFunction))));
  }

  nsCOMPtr<nsIThread> mThread;
  // Owned by the primary task's AutoXPCOMThreadWrapper while it runs.
  std::atomic<AbstractThread*> mWrapper{nullptr};
  std::atomic<bool> mStopPrimary{false};
  std::atomic<bool> mPrimaryRunning{false};
  // Runs in the primary task after its event loop, while still registered.
  std::function<void()> mBeforeUnregister;
};

}  // namespace

TEST_F(XPCOMThreadWrapperTest, DirectTaskFromPlainTask) {
  nsTArray<int> order;
  RunOnThread([&] {
    AbstractThread* current = AbstractThread::GetCurrent();
    ASSERT_EQ(current, mWrapper);
    ASSERT_TRUE(current->IsTailDispatcherAvailable());

    // Queued before the direct task is added, but must run after it.
    MOZ_ALWAYS_SUCCEEDS(mThread->Dispatch(
        NS_NewRunnableFunction("NextTask", [&] { order.AppendElement(3); })));
    current->TailDispatcher().AddDirectTask(
        NS_NewRunnableFunction("DirectTask", [&] { order.AppendElement(2); }));
    order.AppendElement(1);
  });
  RunOnThread([&] { EXPECT_EQ(order, (nsTArray<int>{1, 2, 3})); });
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

TEST_F(XPCOMThreadWrapperTest, TailTasksGoOutWhenUnregistering) {
  RefPtr<TaskQueue> queue = TaskQueue::Create(
      GetMediaThreadPool(MediaThreadType::SUPERVISOR), "TestXPCOMThreadWrapper",
      TailDispatchPolicy::ConsistentOrdering);
  std::atomic<bool> ranOnQueue{false};
  // Dispatched from the primary task itself, outside any nested event, so the
  // tail dispatcher only fires when the wrapper is unregistered.
  mBeforeUnregister = [&] {
    MOZ_ALWAYS_SUCCEEDS(queue->Dispatch(
        NS_NewRunnableFunction("OnQueue", [&] { ranOnQueue = true; })));
  };
  StopPrimary();
  queue->AwaitIdle();
  EXPECT_TRUE(ranOnQueue);
  queue->BeginShutdown();
  queue->AwaitShutdownAndIdle();
}

TEST_F(XPCOMThreadWrapperTest, UnregisteredWrapperRefusesWork) {
  RefPtr<AbstractThread> wrapper = mWrapper.load();
  StopPrimary();
  RunOnThread([&] {
    EXPECT_FALSE(AbstractThread::GetCurrent());
    EXPECT_FALSE(wrapper->IsCurrentThreadIn());
  });
  bool ran = false;
  EXPECT_EQ(
      wrapper->Dispatch(NS_NewRunnableFunction("Refused", [&] { ran = true; })),
      NS_ERROR_NOT_AVAILABLE);
  RunOnThread([] {});
  EXPECT_FALSE(ran);
  // The last reference goes away on another thread than the wrapped one.
  wrapper = nullptr;
}

TEST_F(XPCOMThreadWrapperTest, TaskQueuedBeforeUnregisteringIsDropped) {
  std::atomic<bool> ran{false};
  mBeforeUnregister = [&] {
    // Bypasses the tail dispatcher, so the task is queued on the thread now
    // and only gets to run after the wrapper has been unregistered.
    MOZ_ALWAYS_SUCCEEDS(mWrapper.load()->Dispatch(
        NS_NewRunnableFunction("Dropped", [&] { ran = true; }),
        AbstractThread::TailDispatch));
  };
  StopPrimary();
  RunOnThread([] {});
  EXPECT_FALSE(ran);
}

TEST_F(XPCOMThreadWrapperTest, ThreadCanBeReused) {
  // Kept alive so that a new wrapper cannot reuse its address.
  RefPtr<AbstractThread> first = mWrapper.load();
  StopPrimary();
  StartPrimary();
  RunOnThread([&] {
    AbstractThread* current = AbstractThread::GetCurrent();
    EXPECT_EQ(current, mWrapper);
    EXPECT_TRUE(current->IsCurrentThreadIn());
  });
  EXPECT_NE(mWrapper.load(), first.get());
}
