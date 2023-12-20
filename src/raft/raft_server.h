#ifndef RAFT_SERVER_H
#define RAFT_SERVER_H

#include <grpcpp/grpcpp.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/health_check_service_interface.h>
#include <memory>

#include "consensus_module.h"
#include "grpcpp/ext/proto_server_reflection_plugin.h"
#include "grpcpp/health_check_service_interface.h"
#include "raft_client.h"
#include "raft.grpc.pb.h"

namespace raft {

class GlobalCtxManager;

class AsyncServer {
public:
  AsyncServer(GlobalCtxManager& ctx);
  virtual ~AsyncServer();

  AsyncServer(const AsyncServer&) = delete;
  AsyncServer& operator=(const AsyncServer&) = delete;

  virtual void ServerInit() = 0;

  virtual void RPCEventLoop() = 0;

protected:
  class CallData {
  public:
      CallData(
          GlobalCtxManager& ctx,
          protocol::raft::RaftService::AsyncService* service,
          grpc::ServerCompletionQueue* scq);

      virtual void Proceed() = 0;

  protected:
      enum class CallStatus {
          CREATE,
          PROCESS,
          FINISH
      };
      GlobalCtxManager& m_ctx;
      protocol::raft::RaftService::AsyncService* m_service;
      grpc::ServerCompletionQueue* m_scq;
      grpc::ServerContext m_server_ctx;
      CallStatus m_status;
  };

protected:
  GlobalCtxManager& m_ctx;
  std::unique_ptr<grpc::Server> m_server;
  std::unique_ptr<grpc::ServerCompletionQueue> m_scq;
};

class RaftServerImpl : public AsyncServer {
public:
  RaftServerImpl(GlobalCtxManager& ctx);
  ~RaftServerImpl();

  void ServerInit() override;

  void RPCEventLoop() override;

  class RequestVoteData : public CallData {
  public:
      RequestVoteData(
          GlobalCtxManager& ctx,
          protocol::raft::RaftService::AsyncService* service,
          grpc::ServerCompletionQueue* scq);

      void Proceed() override;

  private:
      protocol::raft::RequestVote_Request m_request;
      protocol::raft::RequestVote_Response m_response;
      grpc::ServerAsyncResponseWriter<protocol::raft::RequestVote_Response> m_responder;
      RaftClientImpl::Tag m_tag;
  };

  class AppendEntriesData : public CallData {
  public:
      AppendEntriesData(
          GlobalCtxManager& ctx,
          protocol::raft::RaftService::AsyncService* service,
          grpc::ServerCompletionQueue* scq);

      void Proceed() override;

  private:
      protocol::raft::AppendEntries_Request m_request;
      protocol::raft::AppendEntries_Response m_response;
      grpc::ServerAsyncResponseWriter<protocol::raft::AppendEntries_Response> m_responder;
      RaftClientImpl::Tag m_tag;
  };

  class SetConfigurationData : public CallData {
  public:
    SetConfigurationData(
        GlobalCtxManager& ctx,
        protocol::raft::RaftService::AsyncService* service,
        grpc::ServerCompletionQueue* scq);

    void Proceed() override;

  private:
    protocol::raft::SetConfiguration_Request m_request;
    protocol::raft::SetConfiguration_Response m_response;
    grpc::ServerAsyncResponseWriter<protocol::raft::SetConfiguration_Response> m_responder;
    RaftClientImpl::Tag m_tag;
  };

  class GetConfigurationData : public CallData {
  public:
    GetConfigurationData(
        GlobalCtxManager& ctx,
        protocol::raft::RaftService::AsyncService* service,
        grpc::ServerCompletionQueue* scq);

    void Proceed() override;

  private:
    protocol::raft::GetConfiguration_Request m_request;
    protocol::raft::GetConfiguration_Response m_response;
    grpc::ServerAsyncResponseWriter<protocol::raft::GetConfiguration_Response> m_responder;
    RaftClientImpl::Tag m_tag;
  };

  class RegisterClientData: public CallData {
  public:
    RegisterClientData(
        GlobalCtxManager& ctx,
        protocol::raft::RaftService::AsyncService* service,
        grpc::ServerCompletionQueue* scq);

    void Proceed() override;

  private:
    protocol::raft::RegisterClient_Request m_request;
    protocol::raft::RegisterClient_Response m_response;
    grpc::ServerAsyncResponseWriter<protocol::raft::RegisterClient_Response> m_responder;
    RaftClientImpl::Tag m_tag;
  };

  class ClientRequestData: public CallData {
  public:
    ClientRequestData(
        GlobalCtxManager& ctx,
        protocol::raft::RaftService::AsyncService* service,
        grpc::ServerCompletionQueue* scq);

    void Proceed() override;

  private:
    protocol::raft::ClientRequest_Request m_request;
    protocol::raft::ClientRequest_Request  m_response;
    grpc::ServerAsyncResponseWriter<protocol::raft::ClientRequest_Response> m_responder;
    RaftClientImpl::Tag m_tag;
  };

  class ClientQueryData: public CallData {
  public:
    ClientQueryData(
        GlobalCtxManager& ctx,
        protocol::raft::RaftService::AsyncService* service,
        grpc::ServerCompletionQueue* scq);

    void Proceed() override;

  private:
    protocol::raft::ClientQuery_Request m_request;
    protocol::raft::ClientQuery_Request  m_response;
    grpc::ServerAsyncResponseWriter<protocol::raft::ClientQuery_Response> m_responder;
    RaftClientImpl::Tag m_tag;
  };

private:
  protocol::raft::RaftService::AsyncService m_service;
};

}

#endif

