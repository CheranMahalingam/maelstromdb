#include <glog/logging.h>

#include "session_cache.h"

namespace raft {

SessionCache::SessionCache(int size) : m_session_cache(size) {
}

void SessionCache::AddSession(int client_id) {
  DLOG(INFO) << "Adding session with id " << client_id << " to session cache";
  m_session_cache.CreateNode(client_id);
}

void SessionCache::CacheResponse(
    int client_id, int sequence_num, protocol::raft::ClientRequest_Response& reply) {
  m_session_cache.UpdateNode(client_id, sequence_num, reply);
}

bool SessionCache::GetCachedResponse(
    int client_id, int sequence_num, protocol::raft::ClientRequest_Response& reply) {
  bool is_cached = true;
  try {
    reply = m_session_cache.NodeValue(client_id, sequence_num);
  } catch(const std::out_of_range& e) {
    is_cached = false;
  }
  return is_cached;
}

bool SessionCache::SessionExists(int client_id) {
  return m_session_cache.NodeExists(client_id);
}

SessionCache::ClientRequestLRUCache::ClientRequestLRUCache(int capacity)
  : m_capacity(capacity)
  , m_size(0) {
  m_head = std::make_shared<LRUNode>(-1);
  m_tail = std::make_shared<LRUNode>(-1);
  m_head->next = m_tail;
  m_tail->prev = m_head;
}

void SessionCache::ClientRequestLRUCache::CreateNode(int client_id) {
  std::lock_guard<std::mutex> guard(m_lock);
  if (m_cache.find(client_id) != m_cache.end()) {
    throw std::out_of_range("Session with id already exists");
  }

  m_cache[client_id] = std::make_shared<LRUNode>(client_id);
  PushHead(m_cache[client_id]);
}

protocol::raft::ClientRequest_Response SessionCache::ClientRequestLRUCache::NodeValue(
    int client_id, int sequence_num) {
  std::lock_guard<std::mutex> guard(m_lock);
  if (m_cache.find(client_id) == m_cache.end()) {
    throw std::out_of_range("Session with id does not exist");
  }
  auto curr = m_cache[client_id];
  if (curr->val.find(sequence_num) == curr->val.end()) {
    throw std::out_of_range("Session with given sequence_num does not exist");
  }

  // Reconnect previous and next nodes
  curr->prev->next = curr->next;
  curr->next->prev = curr->prev;
  curr->prev = nullptr;
  curr->next = nullptr;
  m_size--;

  PushHead(curr);
  return curr->val[sequence_num];
}

void SessionCache::ClientRequestLRUCache::UpdateNode(
    int client_id, int sequence_num, protocol::raft::ClientRequest_Response& reply) {
  std::lock_guard<std::mutex> guard(m_lock);
  if (m_cache.find(client_id) == m_cache.end()) {
    throw std::out_of_range("Session with id does not exist");
  }

  m_cache[client_id]->val[sequence_num] = reply;
  auto curr = m_cache[client_id];

  // Reconnect previous and next nodes
  curr->prev->next = curr->next;
  curr->next->prev = curr->prev;
  curr->prev = nullptr;
  curr->next = nullptr;
  m_size--;

  PushHead(curr);
}

bool SessionCache::ClientRequestLRUCache::NodeExists(int client_id) {
  std::lock_guard<std::mutex> guard(m_lock);
  return m_cache.find(client_id) != m_cache.end();
}

void SessionCache::ClientRequestLRUCache::PushHead(std::shared_ptr<LRUNode> curr) {
  if (m_size == 0) {
    m_head->next = curr;
    m_tail->prev = curr;
    curr->prev = m_head;
    curr->next = m_tail;
  } else {
    curr->next = m_head->next;
    curr->prev = m_head;
    m_head->next->prev = curr;
    m_head->next = curr;
  }
  m_size++;

  if (m_size > m_capacity) {
    EraseTail();
  }
}

void SessionCache::ClientRequestLRUCache::EraseTail() {
  auto erase_node = m_tail->prev;
  m_tail->prev->prev->next = m_tail;
  m_tail->prev = m_tail->prev->prev;
  erase_node->next = nullptr;
  erase_node->prev = nullptr;
  m_size--;

  auto it = m_cache.find(erase_node->id);
  m_cache.erase(it);
}

SessionCache::ClientRequestLRUCache::LRUNode::LRUNode(int id)
  : id(id), prev(nullptr), next(nullptr) {
}

}

