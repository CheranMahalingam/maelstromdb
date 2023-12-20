#ifndef RAFT_CLIENT_H
#define RAFT_CLIENT_H

#include <grpcpp/grpcpp.h>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "consensus_module.h"
#include "raft.grpc.pb.h"

namespace raft {

class GlobalCtxManager;

class AsyncClient {
public:
  using stub_map = std::unordered_map<std::string, std::unique_ptr<protocol::raft::RaftService::Stub>>;

public:
  AsyncClient(GlobalCtxManager& ctx);
  virtual ~AsyncClient();

  void CreateConnections(std::unordered_set<std::string> peer_addresses);

  virtual void RequestVote(
      const std::string& peer_id,
      const int term,
      const int last_log_index,
      const int last_log_term) = 0;

  virtual void AppendEntries(
      const std::string& peer_id,
      const int term,
      const int prev_log_index,
      const int prev_log_term,
      const std::vector<protocol::log::LogEntry> entries,
      const int leader_commit) = 0;

  virtual void AsyncCompleteRPC() = 0;

protected:
  GlobalCtxManager& m_ctx;
  stub_map m_stubs;
  grpc::CompletionQueue m_cq;
};

class RaftClientImpl : public AsyncClient {
public:
  enum class ClientCommandID {
    REQUEST_VOTE,
    APPEND_ENTRIES,
    GET_CONFIGURATION,
    SET_CONFIGURATION,
    REGISTER_CLIENT,
    CLIENT_REQUEST,
    CLIENT_QUERY
  };

  struct Tag {
    void* call;
    ClientCommandID id;
  };

public:
  RaftClientImpl(GlobalCtxManager& ctx);

  RaftClientImpl(const RaftClientImpl&) = delete;
  RaftClientImpl& operator=(const RaftClientImpl&) = delete;

  void RequestVote(
      const std::string& peer_id,
      const int term,
      const int last_log_index,
      const int last_log_term) override;

  void AppendEntries(
      const std::string& peer_id,
      const int term,
      const int prev_log_index,
      const int prev_log_term,
      const std::vector<protocol::log::LogEntry> entries,
      const int leader_commit) override;

  void AsyncCompleteRPC() override;

private:
  template <typename RequestType, typename ResponseType>
  struct AsyncClientCall {
    RequestType request;
    ResponseType reply;
    grpc::ClientContext ctx;
    grpc::Status status;
    std::unique_ptr<grpc::ClientAsyncResponseReader<ResponseType>> response_reader;
    std::string peer_address;
  };

  void HandleRequestVoteReply(AsyncClientCall<protocol::raft::RequestVote_Request,
      protocol::raft::RequestVote_Response>* call);

  void HandleAppendEntriesReply(AsyncClientCall<protocol::raft::AppendEntries_Request,
      protocol::raft::AppendEntries_Response>* call);
};

}

#endif

