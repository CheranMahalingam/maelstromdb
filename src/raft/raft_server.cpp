#include <glog/logging.h>

#include "raft_server.h"
#include "global_ctx_manager.h"

namespace raft {

AsyncServer::AsyncServer(GlobalCtxManager& ctx)
  : m_ctx(ctx) {
}

AsyncServer::~AsyncServer() {
}

AsyncServer::CallData::CallData(
    GlobalCtxManager& ctx,
    protocol::raft::RaftService::AsyncService* service,
    grpc::ServerCompletionQueue* scq)
  : m_ctx(ctx), m_service(service), m_scq(scq), m_status(CallStatus::CREATE) {
}

RaftServerImpl::RaftServerImpl(GlobalCtxManager& ctx)
  : AsyncServer(ctx) {
}

RaftServerImpl::~RaftServerImpl() {
  if (m_server) {
    m_server->Shutdown();
  }
  if (m_scq) {
    m_scq->Shutdown();
  }
}

void RaftServerImpl::ServerInit() {
  grpc::ServerBuilder builder;
  builder.AddListeningPort(m_ctx.address, grpc::InsecureServerCredentials());
  builder.RegisterService(&m_service);
  m_scq = builder.AddCompletionQueue();
  m_server = builder.BuildAndStart();
  DLOG(INFO) << "Server listening on " << m_ctx.address;

  RPCEventLoop();
}

void RaftServerImpl::RPCEventLoop() {
  new RaftServerImpl::RequestVoteData(m_ctx, &m_service, m_scq.get());
  new RaftServerImpl::AppendEntriesData(m_ctx, &m_service, m_scq.get());
  new RaftServerImpl::GetConfigurationData(m_ctx, &m_service, m_scq.get());
  new RaftServerImpl::SetConfigurationData(m_ctx, &m_service, m_scq.get());
  new RaftServerImpl::RegisterClientData(m_ctx, &m_service, m_scq.get());
  new RaftServerImpl::ClientRequestData(m_ctx, &m_service, m_scq.get());
  new RaftServerImpl::ClientQueryData(m_ctx, &m_service, m_scq.get());

  void* tag;
  bool ok;
  while (true) {
    if (m_scq->Next(&tag, &ok) && ok) {
      auto* tag_ptr = static_cast<RaftClientImpl::Tag*>(tag);
      switch (tag_ptr->id) {
        case RaftClientImpl::ClientCommandID::REQUEST_VOTE: {
          static_cast<RaftServerImpl::RequestVoteData*>(tag_ptr->call)->Proceed();
          break;
        }
        case RaftClientImpl::ClientCommandID::APPEND_ENTRIES: {
          static_cast<RaftServerImpl::AppendEntriesData*>(tag_ptr->call)->Proceed();
          break;
        }
        case RaftClientImpl::ClientCommandID::GET_CONFIGURATION: {
          static_cast<RaftServerImpl::GetConfigurationData*>(tag_ptr->call)->Proceed();
          break;
        }
        case RaftClientImpl::ClientCommandID::SET_CONFIGURATION: {
          static_cast<RaftServerImpl::SetConfigurationData*>(tag_ptr->call)->Proceed();
          break;
        }
        case RaftClientImpl::ClientCommandID::REGISTER_CLIENT: {
          static_cast<RaftServerImpl::RegisterClientData*>(tag_ptr->call)->Proceed();
          break;
        }
        case RaftClientImpl::ClientCommandID::CLIENT_REQUEST: {
          static_cast<RaftServerImpl::ClientRequestData*>(tag_ptr->call)->Proceed();
          break;
        }
        case RaftClientImpl::ClientCommandID::CLIENT_QUERY: {
          static_cast<RaftServerImpl::ClientQueryData*>(tag_ptr->call)->Proceed();
          break;
        }
      }    
    } else {
      LOG(WARNING) << "RPC call failed unexpectedly";
    }
  }
}

RaftServerImpl::RequestVoteData::RequestVoteData(
    GlobalCtxManager& ctx,
    protocol::raft::RaftService::AsyncService* service,
    grpc::ServerCompletionQueue* scq)
  : CallData(ctx, service, scq), m_responder(&m_server_ctx) {
  m_tag.id = RaftClientImpl::ClientCommandID::REQUEST_VOTE;
  m_tag.call = this;
  Proceed();
}

void RaftServerImpl::RequestVoteData::Proceed() {
  switch (m_status) {
    case CallStatus::CREATE: {
      m_status = CallStatus::PROCESS;
      m_service->RequestRequestVote(
          &m_server_ctx,
          &m_request,
          &m_responder,
          m_scq,
          m_scq,
          (void*)&m_tag);
      break;
    }
    case CallStatus::PROCESS: {
      DLOG(INFO) << "Processing RequestVote reply...";
      new RequestVoteData(m_ctx, m_service, m_scq);

      auto [m_response, s] = m_ctx.ConsensusInstance()->ProcessRequestVoteClientRequest(m_request);

      m_status = CallStatus::FINISH;
      m_responder.Finish(m_response, s, (void*)&m_tag);
      break;
    }
    case CallStatus::FINISH: {
      delete this;
    }
  }
}

RaftServerImpl::AppendEntriesData::AppendEntriesData(
    GlobalCtxManager& ctx,
    protocol::raft::RaftService::AsyncService* service,
    grpc::ServerCompletionQueue* scq)
  : CallData(ctx, service, scq), m_responder(&m_server_ctx) {
  m_tag.id = RaftClientImpl::ClientCommandID::APPEND_ENTRIES;
  m_tag.call = this;
  Proceed();
}

void RaftServerImpl::AppendEntriesData::Proceed() {
  switch (m_status) {
    case CallStatus::CREATE: {
      m_status = CallStatus::PROCESS;
      m_service->RequestAppendEntries(
          &m_server_ctx,
          &m_request,
          &m_responder,
          m_scq,
          m_scq,
          (void*)&m_tag);
      break;
    }
    case CallStatus::PROCESS: {
      DLOG(INFO) << "Processing AppendEntries reply...";
      new AppendEntriesData(m_ctx, m_service, m_scq);

      auto [m_response, s] = m_ctx.ConsensusInstance()->ProcessAppendEntriesClientRequest(m_request);

      m_status = CallStatus::FINISH;
      m_responder.Finish(m_response, s, (void*)&m_tag);
      break;
    }
    case CallStatus::FINISH: {
      delete this;
    }
  }
}

RaftServerImpl::SetConfigurationData::SetConfigurationData(
    GlobalCtxManager& ctx,
    protocol::raft::RaftService::AsyncService* service,
    grpc::ServerCompletionQueue* scq)
  : CallData(ctx, service, scq), m_responder(&m_server_ctx) {
  m_tag.id = RaftClientImpl::ClientCommandID::SET_CONFIGURATION;
  m_tag.call = this;
  Proceed();
}

void RaftServerImpl::SetConfigurationData::Proceed() {
  switch (m_status) {
    case CallStatus::CREATE: {
      m_status = CallStatus::PROCESS;
      m_service->RequestSetConfiguration(
          &m_server_ctx,
          &m_request,
          &m_responder,
          m_scq,
          m_scq,
          (void*)&m_tag);
      break;
    }
    case CallStatus::PROCESS: {
      DLOG(INFO) << "Processing SetConfiguration reply...";
      new SetConfigurationData(m_ctx, m_service, m_scq);

      auto [m_response, s] = m_ctx.ConsensusInstance()->ProcessSetConfigurationClientRequest(m_request);

      m_status = CallStatus::FINISH;
      m_responder.Finish(m_response, s, (void*)&m_tag);
      break;
    }
    case CallStatus::FINISH: {
      delete this;
    }
  }
}

RaftServerImpl::GetConfigurationData::GetConfigurationData(
    GlobalCtxManager& ctx,
    protocol::raft::RaftService::AsyncService* service,
    grpc::ServerCompletionQueue* scq)
  : CallData(ctx, service, scq), m_responder(&m_server_ctx) {
  m_tag.id = RaftClientImpl::ClientCommandID::GET_CONFIGURATION;
  m_tag.call = this;
  Proceed();
}

void RaftServerImpl::GetConfigurationData::Proceed() {
  switch (m_status) {
    case CallStatus::CREATE: {
      m_status = CallStatus::PROCESS;
      m_service->RequestGetConfiguration(
          &m_server_ctx,
          &m_request,
          &m_responder,
          m_scq,
          m_scq,
          (void*)&m_tag);
      break;
    }
    case CallStatus::PROCESS: {
      DLOG(INFO) << "Processing GetConfiguration reply...";
      new GetConfigurationData(m_ctx, m_service, m_scq);

      auto [m_response, s] = m_ctx.ConsensusInstance()->ProcessGetConfigurationClientRequest();

      m_status = CallStatus::FINISH;
      m_responder.Finish(m_response, s, (void*)&m_tag);
      break;
    }
    case CallStatus::FINISH: {
      delete this;
    }
  }
}

RaftServerImpl::RegisterClientData::RegisterClientData(
    GlobalCtxManager& ctx,
    protocol::raft::RaftService::AsyncService* service,
    grpc::ServerCompletionQueue* scq)
  : CallData(ctx, service, scq), m_responder(&m_server_ctx) {
  m_tag.id = RaftClientImpl::ClientCommandID::REGISTER_CLIENT;
  m_tag.call = this;
  Proceed();
}

void RaftServerImpl::RegisterClientData::Proceed() {
  switch (m_status) {
    case CallStatus::CREATE: {
      m_status = CallStatus::PROCESS;
      m_service->RequestRegisterClient(
          &m_server_ctx,
          &m_request,
          &m_responder,
          m_scq,
          m_scq,
          (void*)&m_tag);
      break;
    }
    case CallStatus::PROCESS: {
      DLOG(INFO) << "Processing RegisterClient reply...";
      new RegisterClientData(m_ctx, m_service, m_scq);

      auto [m_response, s] = m_ctx.ConsensusInstance()->ProcessRegisterClientClientRequest();

      m_status = CallStatus::FINISH;
      m_responder.Finish(m_response, s, (void*)&m_tag);
      break;
    }
    case CallStatus::FINISH: {
      delete this;
    }
  }
}
RaftServerImpl::ClientRequestData::ClientRequestData(
    GlobalCtxManager& ctx,
    protocol::raft::RaftService::AsyncService* service,
    grpc::ServerCompletionQueue* scq)
  : CallData(ctx, service, scq), m_responder(&m_server_ctx) {
  m_tag.id = RaftClientImpl::ClientCommandID::CLIENT_REQUEST;
  m_tag.call = this;
  Proceed();
}

void RaftServerImpl::ClientRequestData::Proceed() {
  switch (m_status) {
    case CallStatus::CREATE: {
      m_status = CallStatus::PROCESS;
      m_service->RequestClientRequest(
          &m_server_ctx,
          &m_request,
          &m_responder,
          m_scq,
          m_scq,
          (void*)&m_tag);
      break;
    }
    case CallStatus::PROCESS: {
      DLOG(INFO) << "Processing ClientRequest reply...";
      new ClientRequestData(m_ctx, m_service, m_scq);

      auto [m_response, s] = m_ctx.ConsensusInstance()->ProcessClientRequestClientRequest(m_request);

      m_status = CallStatus::FINISH;
      m_responder.Finish(m_response, s, (void*)&m_tag);
      break;
    }
    case CallStatus::FINISH: {
      delete this;
    }
  }
}

RaftServerImpl::ClientQueryData::ClientQueryData(
    GlobalCtxManager& ctx,
    protocol::raft::RaftService::AsyncService* service,
    grpc::ServerCompletionQueue* scq)
  : CallData(ctx, service, scq), m_responder(&m_server_ctx) {
  m_tag.id = RaftClientImpl::ClientCommandID::CLIENT_QUERY;
  m_tag.call = this;
  Proceed();
}

void RaftServerImpl::ClientQueryData::Proceed() {
  switch (m_status) {
    case CallStatus::CREATE: {
      m_status = CallStatus::PROCESS;
      m_service->RequestClientQuery(
          &m_server_ctx,
          &m_request,
          &m_responder,
          m_scq,
          m_scq,
          (void*)&m_tag);
      break;
    }
    case CallStatus::PROCESS: {
      DLOG(INFO) << "Processing ClientQuery reply...";
      new ClientQueryData(m_ctx, m_service, m_scq);

      auto [m_response, s] = m_ctx.ConsensusInstance()->ProcessClientQueryClientRequest(m_request);

      m_status = CallStatus::FINISH;
      m_responder.Finish(m_response, s, (void*)&m_tag);
      break;
    }
    case CallStatus::FINISH: {
      delete this;
    }
  }
}

}

