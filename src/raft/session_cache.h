#ifndef SESSION_CACHE_H
#define SESSION_CACHE_H

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "raft.grpc.pb.h"

namespace raft {

class SessionCache {
public:
  SessionCache(int size);

  void AddSession(int client_id);

  void CacheResponse(
      int client_id, int sequence_num, protocol::raft::ClientRequest_Response& reply);

  bool GetCachedResponse(
      int client_id, int sequence_num, protocol::raft::ClientRequest_Response& reply);

  bool SessionExists(int client_id);

private:
  class ClientRequestLRUCache {
  public:
    ClientRequestLRUCache(int capacity);

    void CreateNode(int client_id);

    protocol::raft::ClientRequest_Response NodeValue(int client_id, int sequence_num);
    void UpdateNode(
        int client_id, int sequence_num, protocol::raft::ClientRequest_Response& reply);

    bool NodeExists(int client_id);

  private:
    struct LRUNode {
      LRUNode(int id);

      std::unordered_map<int, protocol::raft::ClientRequest_Response> val;
      std::shared_ptr<LRUNode> prev;
      std::shared_ptr<LRUNode> next;
      int id;
    };

    void PushHead(std::shared_ptr<LRUNode> curr);
    void EraseTail();

  private:
    int m_capacity;
    int m_size;
    std::mutex m_lock;
    std::unordered_map<int, std::shared_ptr<LRUNode>> m_cache;
    std::shared_ptr<LRUNode> m_head;
    std::shared_ptr<LRUNode> m_tail;
  };

private:
  ClientRequestLRUCache m_session_cache;
};

}

#endif

