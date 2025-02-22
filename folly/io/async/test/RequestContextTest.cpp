/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <thread>

#include <folly/Memory.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/async/Request.h>
#include <folly/io/async/test/RequestContextHelper.h>
#include <folly/portability/GTest.h>
#include <folly/system/ThreadName.h>

using namespace folly;

RequestToken testtoken("test");

class RequestContextTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Make sure each test starts out using the default context, and not some
    // other context left over by a previous test.
    RequestContext::setContext(nullptr);

    // Make sure no data is set for the "test" key when we start.  There could
    // be left over data in the default context from a previous test.  If we
    // don't clear it out future calls to setContextData() won't actually work,
    // and will reset the data to null instead of properly setting the new
    // desired data.
    //
    // (All of the tests generally want the behavior of overwriteContextData()
    // rather than setContextData(), but that method is private.)
    //
    // We ideally want to clear out data for any keys that may be set, not just
    // the "test" key, but there also isn't a RequestContext API to do this.
    clearData();
  }

  RequestContext& getContext() {
    auto* ctx = RequestContext::get();
    EXPECT_TRUE(ctx != nullptr);
    return *ctx;
  }

  void setData(int data = 0, std::string key = "test") {
    getContext().setContextData(key, std::make_unique<TestData>(data));
  }

  bool hasData(std::string key = "test") {
    return getContext().hasContextData(key);
  }

  const TestData& getData(std::string key = "test") {
    auto* ptr = dynamic_cast<TestData*>(getContext().getContextData(key));
    EXPECT_TRUE(ptr != nullptr);
    return *ptr;
  }

  void clearData(std::string key = "test") {
    getContext().clearContextData(key);
  }

  std::vector<intptr_t> getRootIdsFromAllThreads() {
    auto rootids = RequestContext::getRootIdsFromAllThreads();
    std::vector<intptr_t> result;
    std::transform(
        rootids.begin(), rootids.end(), std::back_inserter(result), [](auto e) {
          return e.second;
        });
    return result;
  }
};

TEST_F(RequestContextTest, SimpleTest) {
  EventBase base;

  // There should always be a default context with get()
  EXPECT_TRUE(RequestContext::get() != nullptr);

  // but not with saveContext()
  EXPECT_EQ(RequestContext::saveContext(), nullptr);
  RequestContext::create();
  EXPECT_NE(RequestContext::saveContext(), nullptr);
  auto rootids = getRootIdsFromAllThreads();
  EXPECT_EQ(1, rootids.size());
  EXPECT_EQ(RequestContext::get()->getRootId(), rootids[0]);
  EXPECT_EQ(reinterpret_cast<intptr_t>(RequestContext::get()), rootids[0]);
  RequestContext::create();
  EXPECT_NE(RequestContext::saveContext(), nullptr);
  EXPECT_NE(RequestContext::get()->getRootId(), rootids[0]);

  EXPECT_EQ(nullptr, RequestContext::get()->getContextData("test"));

  RequestContext::get()->setContextData("test", std::make_unique<TestData>(10));
  base.runInEventBaseThread([&]() {
    EXPECT_TRUE(RequestContext::get() != nullptr);
    auto data = dynamic_cast<TestData*>(
                    RequestContext::get()->getContextData(testtoken))
                    ->data_;
    EXPECT_EQ(10, data);
    rootids = getRootIdsFromAllThreads();
    EXPECT_EQ(2, rootids.size());
    EXPECT_EQ(RequestContext::get()->getRootId(), rootids[0]);
    EXPECT_EQ(RequestContext::get()->getRootId(), rootids[1]);
    base.terminateLoopSoon();
  });
  auto th = std::thread([&]() { base.loopForever(); });
  th.join();
  EXPECT_TRUE(RequestContext::get() != nullptr);
  auto a =
      dynamic_cast<TestData*>(RequestContext::get()->getContextData("test"));
  auto data = a->data_;
  EXPECT_EQ(10, data);

  RequestContext::setContext(std::shared_ptr<RequestContext>());
  // There should always be a default context
  EXPECT_TRUE(nullptr != RequestContext::get());
}

TEST_F(RequestContextTest, RequestContextScopeGuard) {
  RequestContextScopeGuard g0;
  setData(10);
  {
    RequestContextScopeGuard g1;
    EXPECT_FALSE(hasData());
    setData(20);
    EXPECT_EQ(20, getData().data_);
    EXPECT_EQ(1, getData().set_);
    EXPECT_EQ(0, getData().unset_);
  }
  EXPECT_EQ(10, getData().data_);
  EXPECT_EQ(2, getData().set_);
  EXPECT_EQ(1, getData().unset_);
}

TEST_F(RequestContextTest, defaultContext) {
  // Don't create a top level guard
  setData(10);
  {
    RequestContextScopeGuard g1;
    EXPECT_FALSE(hasData());
  }
  EXPECT_EQ(10, getData().data_);
  EXPECT_EQ(1, getData().set_);
  EXPECT_EQ(0, getData().unset_);
}

TEST_F(RequestContextTest, setIfAbsentTest) {
  EXPECT_TRUE(RequestContext::get() != nullptr);

  RequestContext::get()->setContextData("test", std::make_unique<TestData>(10));
  EXPECT_FALSE(RequestContext::get()->setContextDataIfAbsent(
      "test", std::make_unique<TestData>(20)));
  EXPECT_EQ(
      10,
      dynamic_cast<TestData*>(RequestContext::get()->getContextData(testtoken))
          ->data_);

  EXPECT_TRUE(RequestContext::get()->setContextDataIfAbsent(
      "test2", std::make_unique<TestData>(20)));
  EXPECT_EQ(
      20,
      dynamic_cast<TestData*>(RequestContext::get()->getContextData("test2"))
          ->data_);

  RequestContext::setContext(std::shared_ptr<RequestContext>());
  EXPECT_TRUE(nullptr != RequestContext::get());
}

TEST_F(RequestContextTest, testSetUnset) {
  RequestContext::create();
  auto ctx1 = RequestContext::saveContext();
  ctx1->setContextData("test", std::make_unique<TestData>(10));
  auto testData1 = dynamic_cast<TestData*>(ctx1->getContextData("test"));

  // onSet called in setContextData
  EXPECT_EQ(1, testData1->set_);

  // Override RequestContext
  RequestContext::create();
  auto ctx2 = RequestContext::saveContext();
  ctx2->setContextData(testtoken, std::make_unique<TestData>(20));
  auto testData2 = dynamic_cast<TestData*>(ctx2->getContextData(testtoken));

  // onSet called in setContextData
  EXPECT_EQ(1, testData2->set_);

  // Check ctx1->onUnset was called
  EXPECT_EQ(1, testData1->unset_);

  RequestContext::setContext(ctx1);
  EXPECT_EQ(2, testData1->set_);
  EXPECT_EQ(1, testData1->unset_);
  EXPECT_EQ(1, testData2->unset_);

  RequestContext::setContext(ctx2);
  EXPECT_EQ(2, testData1->set_);
  EXPECT_EQ(2, testData1->unset_);
  EXPECT_EQ(2, testData2->set_);
  EXPECT_EQ(1, testData2->unset_);
}

TEST_F(RequestContextTest, deadlockTest) {
  class DeadlockTestData : public RequestData {
   public:
    explicit DeadlockTestData(const std::string& val) : val_(val) {}

    ~DeadlockTestData() override {
      RequestContext::get()->setContextData(
          val_, std::make_unique<TestData>(1));
    }

    bool hasCallback() override {
      return false;
    }

    std::string val_;
  };

  RequestContext::get()->setContextData(
      "test", std::make_unique<DeadlockTestData>("test2"));
  RequestContext::get()->clearContextData(testtoken);
}

// A common use case is to use set/unset to maintain a thread global
// Regression test to ensure that unset is always called before set
TEST_F(RequestContextTest, sharedGlobalTest) {
  static bool global = false;

  class GlobalTestData : public RequestData {
   public:
    void onSet() override {
      ASSERT_FALSE(global);
      global = true;
    }

    void onUnset() override {
      ASSERT_TRUE(global);
      global = false;
    }

    bool hasCallback() override {
      return true;
    }
  };

  intptr_t root = 0;
  {
    RequestContextScopeGuard g0;
    RequestContext::get()->setContextData(
        "test", std::make_unique<GlobalTestData>());
    auto root0 = RequestContext::saveContext().get()->getRootId();
    EXPECT_EQ(getRootIdsFromAllThreads()[0], root0);
    {
      RequestContextScopeGuard g1;
      RequestContext::get()->setContextData(
          "test", std::make_unique<GlobalTestData>());
      auto root1 = RequestContext::saveContext().get()->getRootId();
      EXPECT_EQ(getRootIdsFromAllThreads()[0], root1);
    }
    EXPECT_EQ(getRootIdsFromAllThreads()[0], root0);
  }
  EXPECT_EQ(getRootIdsFromAllThreads()[0], root);
}

TEST_F(RequestContextTest, ShallowCopyBasic) {
  ShallowCopyRequestContextScopeGuard g0;
  setData(123, "immutable");
  EXPECT_EQ(123, getData("immutable").data_);
  EXPECT_FALSE(hasData());
  EXPECT_EQ(0, getRootIdsFromAllThreads()[0]);

  {
    ShallowCopyRequestContextScopeGuard g1;
    EXPECT_EQ(123, getData("immutable").data_);
    setData(789);
    EXPECT_EQ(789, getData().data_);
    EXPECT_EQ(0, getRootIdsFromAllThreads()[0]);
  }

  EXPECT_FALSE(hasData());
  EXPECT_EQ(123, getData("immutable").data_);
  EXPECT_EQ(1, getData("immutable").set_);
  EXPECT_EQ(0, getData("immutable").unset_);
  EXPECT_EQ(0, getRootIdsFromAllThreads()[0]);
}

TEST_F(RequestContextTest, ShallowCopyOverwrite) {
  RequestContextScopeGuard g0;
  setData(123);
  EXPECT_EQ(123, getData().data_);
  auto rootid = RequestContext::get()->getRootId();
  EXPECT_EQ(rootid, getRootIdsFromAllThreads()[0]);
  {
    ShallowCopyRequestContextScopeGuard g1(
        "test", std::make_unique<TestData>(789));
    EXPECT_EQ(789, getData().data_);
    EXPECT_EQ(1, getData().set_);
    EXPECT_EQ(0, getData().unset_);
    // should have inherited parent's rootid
    EXPECT_EQ(rootid, getRootIdsFromAllThreads()[0]);

    {
      // rootId is preserved for shallow copies of shallow copies
      ShallowCopyRequestContextScopeGuard g2;
      EXPECT_EQ(rootid, getRootIdsFromAllThreads()[0]);
    }
    EXPECT_EQ(rootid, getRootIdsFromAllThreads()[0]);
  }
  EXPECT_EQ(123, getData().data_);
  EXPECT_EQ(2, getData().set_);
  EXPECT_EQ(1, getData().unset_);
  EXPECT_EQ(rootid, getRootIdsFromAllThreads()[0]);
}

TEST_F(RequestContextTest, ShallowCopyDefaultContext) {
  // Don't set global scope guard
  setData(123);
  EXPECT_EQ(123, getData().data_);
  {
    ShallowCopyRequestContextScopeGuard g1(
        "test", std::make_unique<TestData>(789));
    EXPECT_EQ(789, getData().data_);
  }
  EXPECT_EQ(123, getData().data_);
  EXPECT_EQ(1, getData().set_);
  EXPECT_EQ(0, getData().unset_);
}

TEST_F(RequestContextTest, ShallowCopyClear) {
  RequestContextScopeGuard g0;
  setData(123);
  EXPECT_EQ(123, getData().data_);
  {
    ShallowCopyRequestContextScopeGuard g1;
    EXPECT_EQ(123, getData().data_);
    clearData();
    setData(789);
    EXPECT_EQ(789, getData().data_);
  }
  EXPECT_EQ(123, getData().data_);
  EXPECT_EQ(2, getData().set_);
  EXPECT_EQ(1, getData().unset_);
}

TEST_F(RequestContextTest, RootIdOnCopy) {
  auto ctxBase = std::make_shared<RequestContext>();
  EXPECT_EQ(reinterpret_cast<intptr_t>(ctxBase.get()), ctxBase->getRootId());
  {
    auto ctx = RequestContext::copyAsRoot(*ctxBase);
    EXPECT_EQ(reinterpret_cast<intptr_t>(ctx.get()), ctx->getRootId());
  }
  {
    auto ctx = RequestContext::copyAsChild(*ctxBase);
    EXPECT_EQ(reinterpret_cast<intptr_t>(ctxBase.get()), ctx->getRootId());
  }
}

TEST_F(RequestContextTest, ThreadId) {
  folly::setThreadName("DummyThread");
  RequestContextScopeGuard g;
  auto ctxBase = std::make_shared<RequestContext>();
  auto rootids = RequestContext::getRootIdsFromAllThreads();
  EXPECT_EQ(*folly::getThreadName(rootids[0].first), "DummyThread");

  EventBase base;
  base.runInEventBaseThread([&]() {
    RequestContextScopeGuard g;
    folly::setThreadName("DummyThread2");
    rootids = RequestContext::getRootIdsFromAllThreads();
    base.terminateLoopSoon();
  });

  auto th = std::thread([&]() { base.loopForever(); });
  th.join();

  std::sort(rootids.begin(), rootids.end(), [](const auto& a, const auto& b) {
    auto aname = folly::getThreadName(a.first);
    auto bname = folly::getThreadName(b.first);
    return (aname ? *aname : "zzz") < (bname ? *bname : "zzz");
  });

  EXPECT_EQ(*folly::getThreadName(rootids[0].first), "DummyThread");
  EXPECT_FALSE(folly::getThreadName(rootids[1].first));
}
