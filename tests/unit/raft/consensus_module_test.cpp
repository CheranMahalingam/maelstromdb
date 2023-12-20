#include <gtest/gtest.h>
#include <memory>

#include "consensus_module.h"
#include "global_ctx_manager.h"
#include "raft_client.h"
#include "raft_server.h"
#include "timer.h"

namespace raft {

class FakeTimer : public core::DeadlineTimer {
public:
  FakeTimer(std::shared_ptr<core::TimerEvent> ctx);

  void Cancel() override;

  void Reset() override;
  void Reset(int delay) override;
  void Reset(std::function<void()>&& callback, int delay) override;

  void Expire();
};

FakeTimer::FakeTimer(std::shared_ptr<core::TimerEvent> ctx)
  : core::DeadlineTimer(ctx) {
}

void FakeTimer::Cancel() {
  m_timer_ctx->Cancel();
}

void FakeTimer::Reset() {
  m_timer_ctx->SetDeadline(m_timer_ctx->Delay());
}

void FakeTimer::Reset(int delay) {
  m_timer_ctx->SetDelay(delay);
  Reset();
}

void FakeTimer::Reset(std::function<void()>&& callback, int delay) {
  m_timer_ctx->SetCallback(std::move(callback));
  Reset(delay);
}

void FakeTimer::Expire() {
  m_timer_ctx->Execute();
}

std::shared_ptr<FakeTimer> BuildFakeTimer(std::function<void()>&& callback) {
  auto timer_queue = std::make_shared<core::TimerQueue>();
  auto ctx = std::make_shared<core::TimerCallbackEvent>(
      INT_MAX, std::make_shared<core::Strand>(), std::move(timer_queue), std::move(callback));
  auto new_timer = std::make_shared<FakeTimer>(ctx);
  return new_timer;
}

class ConsensusModuleTest: public ::testing::Test {
protected:
  void SetUp() override {
    std::string fake_address = "localhost:test";
    ctx = new GlobalCtxManager(fake_address);
    auto cm = ctx->ConsensusInstance();

    cm->StateMachineInit();
    cm->InjectTimers(
        BuildFakeTimer(std::bind(&ConsensusModule::ElectionCallback,
            cm,
            cm->Term())),
        BuildFakeTimer(std::bind(&ConsensusModule::HeartbeatCallback,
            cm)),
        BuildFakeTimer(std::bind(&ConsensusModule::ResetToFollower,
            cm,
            cm->Term())));
  };

  void TearDown() override {
    delete ctx;
  }

  GlobalCtxManager* ctx;
};

TEST_F(ConsensusModuleTest, ResetToFollowerValidState) {
  auto cm = ctx->ConsensusInstance();
  cm->ResetToFollower(5);

  EXPECT_EQ(cm->Term(), 5);
  EXPECT_EQ(cm->State(), ConsensusModule::RaftState::FOLLOWER);
  EXPECT_EQ(cm->Vote(), "");
  EXPECT_EQ(cm->VotesReceived(), 0);
}

}

