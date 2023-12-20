#include <glog/logging.h>

#include "raft_client.h"
#include "global_ctx_manager.h"
#include "raft.pb.h"

namespace raft {

AsyncClient::AsyncClient(GlobalCtxManager& ctx) : m_ctx(ctx) {
}

AsyncClient::~AsyncClient() {
}

void AsyncClient::CreateConnections(std::unordered_set<std::string> peer_addresses) {
  std::vector<std::string> new_addresses;
  std::vector<std::string> removed_addresses;
  for (auto& peer_id:peer_addresses) {
    if (m_stubs.find(peer_id) == m_stubs.end()) {
      new_addresses.push_back(peer_id);
    }
  }
  for (auto& connection:m_stubs) {
    if (peer_addresses.find(connection.first) == peer_addresses.end()) {
      removed_addresses.push_back(connection.first);
    }
  }

  for (auto& address:removed_addresses) {
    m_stubs.erase(address);
  }
  for (auto& address:new_addresses) {
    if (address == m_ctx.address) {
      continue;
    }
    std::shared_ptr<grpc::Channel> chan = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
    m_stubs[address] = protocol::raft::RaftService::NewStub(chan);
  }
}

RaftClientImpl::RaftClientImpl(GlobalCtxManager& ctx)
  : AsyncClient(ctx) {
}

void RaftClientImpl::RequestVote(
    const std::string& peer_id,
    const int term,
    const int last_log_index,
    const int last_log_term) {
  protocol::raft::RequestVote_Request request_args;
  request_args.set_term(term);
  request_args.set_candidateid(m_ctx.address);
  request_args.set_lastlogindex(last_log_index);
  request_args.set_lastlogterm(last_log_term);

  if (!m_stubs[peer_id]) {
    DLOG(INFO) << "Server at " << peer_id << " disconnected";
    return;
  }

  auto* call = new AsyncClientCall<protocol::raft::RequestVote_Request, protocol::raft::RequestVote_Response>;

  call->request = request_args;
  call->peer_address = peer_id;
  call->response_reader = m_stubs[peer_id]->PrepareAsyncRequestVote(&call->ctx, request_args, &m_cq);
  call->response_reader->StartCall();

  auto* tag = new Tag;
  tag->call = (void*)call;
  tag->id = ClientCommandID::REQUEST_VOTE;

  call->response_reader->Finish(&call->reply, &call->status, (void*)tag);
}

void RaftClientImpl::AppendEntries(
    const std::string& peer_id,
    const int term,
    const int prev_log_index,
    const int prev_log_term,
    const std::vector<protocol::log::LogEntry> entries,
    const int leader_commit) {
  protocol::raft::AppendEntries_Request request_args;
  request_args.set_term(term);
  request_args.set_leaderid(m_ctx.address);
  request_args.set_prevlogindex(prev_log_index);
  request_args.set_prevlogterm(prev_log_term);
  request_args.set_leadercommit(leader_commit);

  for (auto& entry:entries) {
    *request_args.add_entries() = entry;
  }

  if (!m_stubs[peer_id]) {
    LOG(WARNING) << "Server at " << peer_id << " disconnected";
    return;
  }

  auto* call = new AsyncClientCall<protocol::raft::AppendEntries_Request, protocol::raft::AppendEntries_Response>;

  call->request = request_args;
  call->peer_address = peer_id;
  call->response_reader = m_stubs[peer_id]->PrepareAsyncAppendEntries(&call->ctx, request_args, &m_cq);
  call->response_reader->StartCall();

  auto* tag = new Tag;
  tag->call = (void*)call;
  tag->id = ClientCommandID::APPEND_ENTRIES;

  call->response_reader->Finish(&call->reply, &call->status, (void*)tag);
}

void RaftClientImpl::AsyncCompleteRPC() {
  void* tag;
  bool ok = false;

  while (m_cq.Next(&tag, &ok)) {
    GPR_ASSERT(ok);

    auto* tag_ptr = static_cast<Tag*>(tag);
    switch (tag_ptr->id) {
      case ClientCommandID::REQUEST_VOTE: {
        auto* call = static_cast<AsyncClientCall<protocol::raft::RequestVote_Request,
          protocol::raft::RequestVote_Response>*>(tag_ptr->call);

        HandleRequestVoteReply(call);

        delete call;
        break;
      }
      case ClientCommandID::APPEND_ENTRIES: {
        auto* call = static_cast<AsyncClientCall<protocol::raft::AppendEntries_Request,
          protocol::raft::AppendEntries_Response>*>(tag_ptr->call);

        HandleAppendEntriesReply(call);

        delete call;
        break;
      }
      default: {
        LOG(ERROR) << "Invalid client ID";
      }
    }

    delete tag_ptr;
  }
}

void RaftClientImpl::HandleRequestVoteReply(AsyncClientCall<protocol::raft::RequestVote_Request,
      protocol::raft::RequestVote_Response>* call) {
  if (!call->status.ok()) {
    LOG(ERROR) << "RequestVote call failed unexpectedly"; 
    return;
  }

  m_ctx.ConsensusInstance()->ProcessRequestVoteServerResponse(call->request, call->reply, call->peer_address);

  DLOG(INFO) << "RequestVote call was received";
}

void RaftClientImpl::HandleAppendEntriesReply(AsyncClientCall<protocol::raft::AppendEntries_Request,
      protocol::raft::AppendEntries_Response>* call) {
  if (!call->status.ok()) {
    LOG(ERROR) << "AppendEntries call failed unexpectedly";
    return;
  }

  m_ctx.ConsensusInstance()->ProcessAppendEntriesServerResponse(call->request, call->reply, call->peer_address);

  DLOG(INFO) << "AppendEntries call was received";
}

}

