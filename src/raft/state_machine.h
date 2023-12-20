#ifndef STATE_MACHINE_H
#define STATE_MACHINE_H

#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "inmemory_store.h"
#include "raft.grpc.pb.h"
#include "session_cache.h"

namespace raft {

class StateMachine {
public:
  StateMachine(std::shared_ptr<SessionCache> sessions, std::shared_ptr<InmemoryStore> store);

  int LastApplied() const;
  void IncrementLastApplied();

  std::string ApplyCommand(int log_index, protocol::log::LogEntry& log_entry);

private:
  std::shared_ptr<SessionCache> m_sessions;
  std::shared_ptr<InmemoryStore> m_store;
  std::atomic<int> m_last_applied;
};

}

#endif

