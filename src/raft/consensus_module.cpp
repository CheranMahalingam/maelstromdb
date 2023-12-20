#include <glog/logging.h>

#include "consensus_module.h"
#include "global_ctx_manager.h"
#include "raft_client.h"
#include "storage.h"

namespace raft {

ConsensusModule::ConsensusModule(GlobalCtxManager& ctx)
  : m_ctx(ctx)
  , m_vote("")
  , m_votes_received(0)
  , m_term(0)
  , m_state(RaftState::CANDIDATE)
  , m_last_applied(-1)
  , m_commit_index(-1)
  , m_leader_id("")
  , m_configuration(std::make_unique<ClusterConfiguration>())
  , m_timer_executor(std::make_shared<core::Strand>())
  , m_election_timeout(std::chrono::milliseconds(ELECTION_TIMEOUT))
  , m_election_deadline(clock_type::now())
  , m_session(std::make_shared<SessionCache>(1000))
  , m_store(std::make_shared<InmemoryStore>())
  , m_renew_lease(false)
  , m_lease_holder(false) {
}

void ConsensusModule::StateMachineInit() {
  // Restore raft metadata from disk if restarting node after server failure
  protocol::log::LogMetadata metadata;
  bool ok = m_ctx.LogInstance()->Metadata(metadata);
  if (ok) {
    m_term.store(metadata.term());
    m_vote = metadata.vote();
  }

  m_state_machine = std::make_unique<StateMachine>(m_session, m_store);

  protocol::log::Configuration configuration;
  int log_index;
  std::tie(log_index, ok) = m_ctx.LogInstance()->LatestConfiguration(configuration);
  if (ok) {
    DLOG(INFO) << "Restored cluster configuration from disk with id = " << log_index;
    m_configuration->SetConfiguration(log_index, configuration);
  }

  m_ctx.ClientInstance()->CreateConnections(m_configuration->ServerAddresses());

  for (auto peer:m_configuration->ServerAddresses()) {
    if (peer == m_ctx.address) {
      continue;
    }
    m_next_index[peer] = 0;
    m_match_index[peer] = -1;
  }

  m_election_timer = m_ctx.TimerQueueInstance()->CreateTimer(
      ELECTION_TIMEOUT,
      m_timer_executor,
      std::bind(&ConsensusModule::ElectionCallback, this, Term()));
  m_heartbeat_timer = m_ctx.TimerQueueInstance()->CreateTimer(
      HEARTBEAT_TIMEOUT,
      m_timer_executor,
      std::bind(&ConsensusModule::HeartbeatCallback, this));
  m_lease_timer = m_ctx.TimerQueueInstance()->CreateTimer(
      LEADER_LEASE_TIMEOUT,
      m_timer_executor,
      std::bind(&ConsensusModule::ResetToFollower, this, Term()));
}

void ConsensusModule::InitializeConfiguration() {
  if (Term() != 0 ||
      m_ctx.LogInstance()->LastLogIndex() != -1 ||
      m_configuration->ServerAddresses().size() > 0) {
    PromoteToLeader();
    return;
  }

  PromoteToLeader();

  protocol::log::LogEntry log_entry;
  protocol::log::Configuration configuration;
  protocol::log::Server server;
  server.set_address(m_ctx.address);
  *configuration.add_prev_configuration() = server;
  log_entry.set_term(0);
  log_entry.set_type(protocol::log::CONFIGURATION);
  *log_entry.mutable_configuration() = configuration;
  Append(log_entry);
}

int ConsensusModule::Term() const {
  return m_term.load();
}

ConsensusModule::RaftState ConsensusModule::State() const {
  return m_state.load();
}

int ConsensusModule::CommitIndex() const {
  return m_commit_index.load();
}

std::string ConsensusModule::Vote() const {
  return m_vote;
}

int ConsensusModule::VotesReceived() const {
  return m_votes_received;
}

std::string ConsensusModule::LeaderHint() const {
  return m_leader_id;
}

void ConsensusModule::ElectionCallback(const int term) {
  DLOG(INFO) << "Starting election";

  if (State() != RaftState::CANDIDATE && State() != RaftState::FOLLOWER) {
    DLOG(INFO) << "Consensus module state invalid for election";
    return;
  }

  if (Term() != term) {
    DLOG(INFO) << "Term changed from " << term << " to " << Term();
    return;
  }

  if (CommitIndex() >= m_configuration->Id() &&
      !m_configuration->KnownServer(m_ctx.address)) {
    ScheduleElection(term);
    return;
  }

  m_state.store(RaftState::CANDIDATE);
  m_term++;
  int saved_term = Term();
  // Once a node becomes a CANDIDATE it votes for itself
  m_vote = m_ctx.address;
  m_votes_received = 1;

  StoreState();

  if (m_configuration->ServerAddresses().size() == 1 &&
      m_configuration->KnownServer(m_ctx.address)) {
    PromoteToLeader();
    return;
  }

  int last_log_index = m_ctx.LogInstance()->LastLogIndex();
  int last_log_term = m_ctx.LogInstance()->LastLogTerm();
  for (auto peer_id:m_configuration->ServerAddresses()) {
    if (peer_id == m_ctx.address) {
      continue;
    }
    DLOG(INFO) << "Sending RequestVote rpc to " << peer_id;

    m_ctx.ClientInstance()->RequestVote(
        peer_id,
        saved_term,
        last_log_index,
        last_log_term);
  }

  // Start a new election if the node does not get a majority of votes within time limit
  ScheduleElection(saved_term);
}

void ConsensusModule::HeartbeatCallback() {
  if (State() != RaftState::LEADER) {
    DLOG(INFO) << "Invalid state for sending heartbeat";
    return;
  }

  m_heartbeat_time = clock_type::now();
  m_renew_lease.store(true);
  m_responding_peers = {m_ctx.address};

  int saved_term = Term();
  for (auto peer_id:m_configuration->ServerAddresses()) {
    if (peer_id == m_ctx.address) {
      continue;
    }
    DLOG(INFO) << "Sending AppendEntries rpc to " << peer_id;

    int next = m_next_index[peer_id];
    int prev_log_index = next - 1;
    int prev_log_term = -1;
    if (prev_log_index >= 0) {
      prev_log_term = m_ctx.LogInstance()->Entry(prev_log_index).term();
    }

    auto entries = m_ctx.LogInstance()->Entries(next, m_ctx.LogInstance()->LogSize());
    m_ctx.ClientInstance()->AppendEntries(
        peer_id,
        saved_term,
        prev_log_index,
        prev_log_term,
        entries,
        CommitIndex());
  }

  if (m_configuration->ServerAddresses().size() == 1 && m_configuration->KnownServer(m_ctx.address)) {
    UpdateCommitIndex();
  }

  ScheduleHeartbeat();
}

void ConsensusModule::ScheduleElection(const int term) {
  std::random_device rd; // Obtain a random number from hardware
  std::mt19937 gen(rd()); // Seed the generator
  std::uniform_int_distribution<> distr(ELECTION_TIMEOUT, ELECTION_TIMEOUT + 150);
  int random_timeout = distr(gen);

  DLOG(INFO) << "Election timer created: " << random_timeout << " ms";
  m_election_timer->Reset(
      std::bind(&ConsensusModule::ElectionCallback, this, term),
      random_timeout);
  m_election_timeout = milliseconds(random_timeout);
}

void ConsensusModule::ScheduleHeartbeat() {
  m_heartbeat_timer->Reset();
}

void ConsensusModule::Shutdown() {
  m_election_timer->Cancel();
  m_heartbeat_timer->Cancel();

  m_state.store(RaftState::DEAD);
  LOG(INFO) << "Node shutdown";
}

void ConsensusModule::ResetToFollower(const int term) {
  m_state.store(RaftState::FOLLOWER);
  m_renew_lease.store(false);
  m_lease_holder.store(false);
  m_term.store(term);
  m_vote = "";
  m_votes_received = 0;
  DLOG(INFO) << "Reset to follower, term: " << Term();

  m_heartbeat_timer->Cancel();
  m_lease_timer->Cancel();

  // Since term/vote of node is modified, changes must be persisted to disk
  StoreState();

  // FOLLOWER will start an election if it doesn't receive heartbeat from LEADER
  ScheduleElection(term);
}

void ConsensusModule::PromoteToLeader() {
  m_state.store(RaftState::LEADER);
  m_votes_received = 0;
  m_leader_id = m_ctx.address;
  DLOG(INFO) << "Promoted to leader, term: " << Term();

  m_election_timer->Cancel();

  protocol::log::LogEntry noop_entry;
  noop_entry.set_term(Term());
  noop_entry.set_type(protocol::log::NO_OP);
  Append(noop_entry);

  ScheduleHeartbeat();
}

void ConsensusModule::InjectTimers(
    std::shared_ptr<core::DeadlineTimer> election_timer,
    std::shared_ptr<core::DeadlineTimer> heartbeat_timer,
    std::shared_ptr<core::DeadlineTimer> lease_timer) {
  m_election_timer = election_timer;
  m_heartbeat_timer = heartbeat_timer;
  m_lease_timer = lease_timer;
}

void ConsensusModule::StoreState() const {
  protocol::log::LogMetadata metadata;
  metadata.set_term(Term());
  metadata.set_vote(m_vote);
  m_ctx.LogInstance()->SetMetadata(metadata);
  DLOG(INFO) << "Persisted metadata to disk, term = " << Term() << " vote = " << m_vote;
}

int ConsensusModule::Append(protocol::log::LogEntry& log_entry) {
  int log_index = m_ctx.LogInstance()->Append(log_entry);
  if (log_entry.has_configuration()) {
    m_configuration->InsertNewConfiguration(log_index, log_entry.configuration());
    m_ctx.ClientInstance()->CreateConnections(m_configuration->ServerAddresses());
  }
  return log_index;
}

std::pair<int, int> ConsensusModule::Append(std::vector<protocol::log::LogEntry>& log_entries) {
  auto [log_start, log_end] = m_ctx.LogInstance()->Append(log_entries);
  for (int i = 0; i < log_entries.size(); i++) {
    if (log_entries[i].has_configuration()) {
      int log_index = log_start + i;
      m_configuration->InsertNewConfiguration(log_index, log_entries[i].configuration());
    m_ctx.ClientInstance()->CreateConnections(m_configuration->ServerAddresses());
    }
  }
  return {log_start, log_end};
}

void ConsensusModule::CommitEntries(int start_index, std::vector<protocol::log::LogEntry>& log_entries) {
  for (int i = 0; i < log_entries.size(); i++) {
    switch (log_entries[i].type()) {
      case protocol::log::LogOpCode::NO_OP: {
        break;
      }
      case protocol::log::LogOpCode::DATA: {
        m_write_command_sync.notify_one();
        break;
      }
      case protocol::log::LogOpCode::REGISTER_CLIENT: {
        m_session_sync.notify_one();
        break;
      }
      default: {
      }
    }
  }
}

void ConsensusModule::UpdateCommitIndex() {
  int saved_commit_index = CommitIndex();
  int log_size = m_ctx.LogInstance()->LogSize();
  auto log_entries = m_ctx.LogInstance()->Entries(saved_commit_index + 1, log_size);
  int new_commit_index = saved_commit_index;
  for (int i = saved_commit_index + 1; i < log_size; i++) {
    if (log_entries[i - saved_commit_index - 1].term() == Term()) {
      std::unordered_set<std::string> match_peers = {m_ctx.address};
      for (auto peer:m_configuration->ServerAddresses()) {
        if (m_match_index[peer] >= i) {
          match_peers.insert(peer);
        }
      }

      // Once a majority of nodes have replicated a log entry, it can be committed
      if (m_configuration->CheckQuorum(match_peers)) {
        new_commit_index = i;
      }
    }
  }

  if (new_commit_index != saved_commit_index) {
    m_commit_index.store(new_commit_index);
    DLOG(INFO) << "Leader set commit_index = " << new_commit_index;

    int next_log_index = m_state_machine->LastApplied();
    std::vector<protocol::log::LogEntry> uncommited_entries;
    while (next_log_index < new_commit_index) {
      next_log_index++;

      auto uncommited_entry = m_ctx.LogInstance()->Entry(next_log_index);
      uncommited_entries.push_back(uncommited_entry);
    }

    CommitEntries(saved_commit_index, uncommited_entries);
  }

  if (CommitIndex() >= m_configuration->Id()) {
    m_membership_sync.notify_one();

    if (!m_configuration->KnownServer(m_ctx.address)) {
      DLOG(INFO) << "Committed configuration does not include LEADER, resetting to FOLLOWER...";
      ResetToFollower(Term() + 1);
      return;
    }

    if (m_configuration->State() == ClusterConfiguration::ConfigurationState::JOINT) {
      DLOG(INFO) << "Transitioning to new cluster configuration...";
      protocol::log::LogEntry entry;
      entry.set_term(Term());
      entry.set_type(protocol::log::CONFIGURATION);
      *entry.mutable_configuration()->mutable_prev_configuration() =
        m_configuration->Configuration().next_configuration();
      Append(entry);
    }
  }
}

grpc::Status ConsensusModule::ConstructError(std::string err_msg, protocol::raft::Error::Code code) const {
  protocol::raft::Error err_details;
  err_details.set_statuscode(code);
  if (code == protocol::raft::Error::Code::Error_Code_NOT_LEADER) {
    err_details.set_leaderhint(LeaderHint());
  }
  return grpc::Status(grpc::StatusCode::UNKNOWN, err_msg, err_details.SerializeAsString());
}

std::tuple<protocol::raft::RequestVote_Response, grpc::Status> ConsensusModule::ProcessRequestVoteClientRequest(
    protocol::raft::RequestVote_Request& request) {
  protocol::raft::RequestVote_Response reply;

  if (State() == RaftState::DEAD) {
    return std::make_tuple(reply, grpc::Status::CANCELLED);
  }

  if (clock_type::now() <= m_election_deadline || State() == RaftState::LEADER) {
    DLOG(INFO) << "Rejecting RequestVote RPC from " << request.candidateid() << " since this node recently received a heartbeat";
    reply.set_votegranted(false);
    reply.set_term(Term());
    return std::make_tuple(reply, grpc::Status::OK);
  }

  if (request.term() > Term()) {
    DLOG(INFO) << "Term out of date in RequestVote RPC, changed from " << Term() << " to " << request.term();
    ResetToFollower(request.term());
  }

  auto last_log_index = m_ctx.LogInstance()->LastLogIndex();
  auto last_log_term = m_ctx.LogInstance()->LastLogTerm();

  // Vote can only be granted if node hasn't voted for a different node and entries in raft log
  // must be valid
  if (request.term() == Term() &&
      (m_vote == "" || m_vote == request.candidateid()) &&
      (request.lastlogterm() > last_log_term ||
      (request.lastlogterm() == last_log_term && request.lastlogindex() >= last_log_index))) {
    reply.set_votegranted(true);
    m_vote = request.candidateid();
    StoreState();

    ScheduleElection(request.term());
  } else {
    reply.set_votegranted(false);
  }

  reply.set_term(Term());
  return std::make_tuple(reply, grpc::Status::OK);
}

void ConsensusModule::ProcessRequestVoteServerResponse(
    protocol::raft::RequestVote_Request& request,
    protocol::raft::RequestVote_Response& reply,
    const std::string& address) {
  if (State() != RaftState::CANDIDATE) {
    DLOG(INFO) << "Node changed state while waiting for RequestVote reply";
    return;
  }

  if (reply.term() > request.term()) {
    DLOG(INFO) << "Term out of date in election reply, changed from " << request.term() << " to " << reply.term();
    ResetToFollower(reply.term());
    return;
  } else if (reply.term() == request.term()) {
    if (reply.votegranted()) {
      m_votes_received++;

      // If CANDIDATE receives majority of votes it becomes the new leader
      if (m_votes_received*2 > m_configuration->ServerAddresses().size()) {
        DLOG(INFO) << "Wins election with " << m_votes_received << " votes";
        PromoteToLeader();
        return;
      }
    }
  }
}

std::tuple<protocol::raft::AppendEntries_Response, grpc::Status> ConsensusModule::ProcessAppendEntriesClientRequest(
    protocol::raft::AppendEntries_Request& request) {
  protocol::raft::AppendEntries_Response reply;

  if (State() == RaftState::DEAD) {
    return std::make_tuple(reply, grpc::Status::CANCELLED);
  }

  if (request.term() > Term()) {
    DLOG(INFO) << "Term out of date in AppendEntries RPC, changed from " << Term() << "to" << request.term();
    ResetToFollower(request.term());
  }

  bool success = false;
  if (request.term() == Term()) {
    if (State() != RaftState::FOLLOWER) {
      ResetToFollower(request.term());
    } else {
      ScheduleElection(request.term());
      m_election_deadline = clock_type::now() + m_election_timeout;
    }

    // Verify that the two logs agree at prevLogIndex
    if (request.prevlogindex() == -1 ||
        (request.prevlogindex() < m_ctx.LogInstance()->LogSize() &&
         request.prevlogterm() == m_ctx.LogInstance()->Entry(request.prevlogindex()).term())) {
      success = true;
      m_leader_id = request.leaderid();

      int log_insert_index = request.prevlogindex() + 1;
      int new_entries_index = 0;

      while (log_insert_index < m_ctx.LogInstance()->LogSize() &&
          new_entries_index < request.entries().size()) {
        if (m_ctx.LogInstance()->Entry(log_insert_index).term() == request.entries()[new_entries_index].term()) {
          log_insert_index++;
          new_entries_index++;
        } else {
          // If the two logs do not agree at an index, N, all indices >= N are deleted
          m_ctx.LogInstance()->TruncateSuffix(log_insert_index);
          m_configuration->TruncateSuffix(log_insert_index);
          break;
        }
      }

      // Append entries from the request that have not been replicated to the raft log
      if (new_entries_index < request.entries().size()) {
        std::vector<protocol::log::LogEntry> new_entries(request.entries().begin() + new_entries_index, request.entries().end());
        Append(new_entries);
      }

      // Commits log entries that have been committed by the LEADER
      if (request.leadercommit() > CommitIndex()) {
        int saved_commit_index = CommitIndex();
        int new_commit_index = std::min((int)request.leadercommit(), m_ctx.LogInstance()->LogSize());
        m_commit_index.store(new_commit_index);
        DLOG(INFO) << "Setting commit index = " << new_commit_index;

        int next_log_index = m_state_machine->LastApplied();
        std::vector<protocol::log::LogEntry> uncommited_entries;
        while (next_log_index < new_commit_index) {
          next_log_index++;
          auto uncommited_entry = m_ctx.LogInstance()->Entry(next_log_index);
          uncommited_entries.push_back(uncommited_entry);
        }

        CommitEntries(saved_commit_index, uncommited_entries);

        for (int i = 0; i < uncommited_entries.size(); i++) {
          m_state_machine->ApplyCommand(saved_commit_index + i, uncommited_entries[i]);
        }
      }
    }
  }

  // Reschedule election since disk write may take up significant time
  ScheduleElection(request.term());
  m_election_deadline = clock_type::now() + m_election_timeout;

  reply.set_term(Term());
  reply.set_success(success);
  return std::make_tuple(reply, grpc::Status::OK);
}

void ConsensusModule::ProcessAppendEntriesServerResponse(
    protocol::raft::AppendEntries_Request& request,
    protocol::raft::AppendEntries_Response& reply,
    const std::string& address) {
  if (reply.term() > request.term()) {
    DLOG(INFO) << "Term out of date in heartbeat reply, changed from " << request.term() << " to " << reply.term();
    m_leader_id = "";
    ResetToFollower(reply.term());
  }

  if (State() == RaftState::LEADER && reply.term() == Term()) {
    int next = m_next_index[address];
    if (reply.success()) {
      // If heartbeat is successful for majority, reads can be served
      m_responding_peers.insert(address);
      if (m_renew_lease.load() && m_configuration->CheckQuorum(m_responding_peers)) {
        DLOG(INFO) << "Leader lease acquired";
        int commit_delay = std::chrono::duration_cast<milliseconds>(clock_type::now() - m_heartbeat_time).count();
        int lease_expiry = LEADER_LEASE_TIMEOUT - commit_delay;
        m_lease_timer->Reset(lease_expiry);
        m_lease_holder.store(true);
        m_renew_lease.store(false);
      }
      // Update next and match index since all entries in request were replicated on FOLLOWER
      m_next_index[address] = next + request.entries().size();
      m_match_index[address] = next + request.entries().size() - 1;
      DLOG(INFO) << "AppendEntries reply from " << address << "successful: next_index = " << m_next_index[address]
        << "match_index = " << m_match_index[address];

      if (m_configuration->UpdateSyncProgress(address, m_match_index[address])) {
        m_membership_sync.notify_one();
      }

      UpdateCommitIndex();
    } else {
      // If the AppendEntries RPC was unsuccessful the prevLogIndex for the specific node is decremented.
      // This will continue until a raft log entry with a matching term is found.
      m_next_index[address] = next - 1;
      DLOG(INFO) << "AppendEntries reply from " << address << " unsuccessful: next_index = " << next;
    }
  }
}

std::tuple<protocol::raft::GetConfiguration_Response, grpc::Status> ConsensusModule::ProcessGetConfigurationClientRequest() {
  protocol::raft::GetConfiguration_Response reply;
  if (m_state != RaftState::LEADER) {
    grpc::Status err = ConstructError("Peer is not a leader", protocol::raft::Error::Code::Error_Code_NOT_LEADER);
    std::make_tuple(reply, err);
  }

  if (m_configuration->State() != ClusterConfiguration::ConfigurationState::STABLE || CommitIndex() < m_configuration->Id()) {
    grpc::Status err = ConstructError("Peer is not in a stable state", protocol::raft::Error::Code::Error_Code_RETRY);
    std::make_tuple(reply, err);
  }

  reply.set_id(m_configuration->Id());
  *reply.mutable_servers() = m_configuration->Configuration().prev_configuration();
  return std::make_tuple(reply, grpc::Status::OK);
}

std::tuple<protocol::raft::SetConfiguration_Response, grpc::Status> ConsensusModule::ProcessSetConfigurationClientRequest(
    protocol::raft::SetConfiguration_Request& request) {
  protocol::raft::SetConfiguration_Response reply;
  if (State() != RaftState::LEADER) {
    reply.set_ok(false);
    grpc::Status err = ConstructError("Peer is not a leader", protocol::raft::Error::Code::Error_Code_NOT_LEADER);
    return std::make_tuple(reply, err);
  }

  if (m_configuration->Id() != request.oldid()) {
    reply.set_ok(false);
    grpc::Status err = ConstructError("Cluster configuration out of date", protocol::raft::Error::Code::Error_Code_OUT_OF_DATE);
    return std::make_tuple(reply, err);
  }

  if (m_configuration->State() != ClusterConfiguration::ConfigurationState::STABLE) {
    reply.set_ok(false);
    grpc::Status err = ConstructError("Peer is not in a stable state", protocol::raft::Error::Code::Error_Code_RETRY);
    std::make_tuple(reply, err);
  }

  std::mutex m;
  std::unique_lock<std::mutex> lock(m);

  int saved_term = Term();
  protocol::log::Configuration new_configuration;
  for (auto& new_server:request.new_servers()) {
    *new_configuration.add_next_configuration() = new_server;
  }
  *new_configuration.mutable_next_configuration() = {request.new_servers().begin(), request.new_servers().end()};
  *new_configuration.mutable_prev_configuration() = m_configuration->Configuration().prev_configuration();

  std::vector<std::string> new_servers;
  for (auto& server:request.new_servers()) {
    new_servers.push_back(server.address());
  }
  m_configuration->StartLogSync(CommitIndex(), new_servers);
  m_ctx.ClientInstance()->CreateConnections(m_configuration->ServerAddresses());

  while (!m_configuration->SyncComplete()) {
    if (Term() != saved_term) {
      m_configuration->CancelLogSync();
      grpc::Status err = ConstructError("Peer is not a leader", protocol::raft::Error::Code::Error_Code_NOT_LEADER);
      reply.set_ok(false);
      return std::make_tuple(reply, err);
    }

    bool progress = m_membership_sync.wait_for(lock, m_election_timeout, [this] {
      return m_configuration->SyncProgress();
    });

    if (!progress) {
      m_configuration->CancelLogSync();
      grpc::Status err = ConstructError("Peer log sync timed out", protocol::raft::Error::Code::Error_Code_TIMEOUT);
      reply.set_ok(false);
      return std::make_tuple(reply, err);
    }
  }
  DLOG(INFO) << "Log syncing with new servers complete";

  protocol::log::LogEntry configuration_entry;
  configuration_entry.set_term(Term());
  configuration_entry.set_type(protocol::log::LogOpCode::CONFIGURATION);
  *configuration_entry.mutable_configuration() = new_configuration;

  int joint_id = Append(configuration_entry);

  m_membership_sync.wait(lock, [this, joint_id, saved_term] {
    return (m_configuration->Id() > joint_id &&
        CommitIndex() >= m_configuration->Id()) ||
        Term() != saved_term;
  });

  if (m_configuration->Id() > joint_id && CommitIndex() >= m_configuration->Id()) {
    DLOG(INFO) << "Configuration log entry committed successfully";
    reply.set_ok(true);
    return std::make_tuple(reply, grpc::Status::OK);
  } else {
    grpc::Status err = ConstructError("Peer is not a leader", protocol::raft::Error::Code::Error_Code_NOT_LEADER);
    reply.set_ok(false);
    return std::make_tuple(reply, err);
  }
}

std::tuple<protocol::raft::RegisterClient_Response, grpc::Status> ConsensusModule::ProcessRegisterClientClientRequest() {
  protocol::raft::RegisterClient_Response reply;
  if (State() != RaftState::LEADER) {
    reply.set_status(false);
    grpc::Status err = ConstructError("Peer is not a leader", protocol::raft::Error::Code::Error_Code_NOT_LEADER);
    return std::make_tuple(reply, err);
  }

  std::mutex m;
  std::unique_lock<std::mutex> lock(m);

  int saved_term = Term();
  protocol::log::LogEntry session_entry;
  session_entry.set_term(saved_term);
  session_entry.set_type(protocol::log::LogOpCode::REGISTER_CLIENT);
  int session_id = Append(session_entry);

  m_session_sync.wait(lock, [this, session_id, saved_term] {
      return CommitIndex() >= session_id || Term() != saved_term;
  });

  if (Term() != saved_term) {
    grpc::Status err = ConstructError("Peer is not a leader", protocol::raft::Error::Code::Error_Code_NOT_LEADER);
    reply.set_status(false);
    m_state_machine->IncrementLastApplied();
    return std::make_tuple(reply, err);
  }

  m_state_machine->ApplyCommand(session_id, session_entry);

  reply.set_clientid(session_id);
  reply.set_status(true);
  return std::make_tuple(reply, grpc::Status::OK);
}

std::tuple<protocol::raft::ClientRequest_Response, grpc::Status> ConsensusModule::ProcessClientRequestClientRequest(
    protocol::raft::ClientRequest_Request& request) {
  protocol::raft::ClientRequest_Response reply;
  if (State() != RaftState::LEADER) {
    reply.set_status(false);
    grpc::Status err = ConstructError("Peer is not a leader", protocol::raft::Error::Code::Error_Code_NOT_LEADER);
    return std::make_tuple(reply, err);
  }

  std::mutex m;
  std::unique_lock<std::mutex> lock(m);

  int saved_term = Term();
  protocol::log::LogEntry write_entry;
  write_entry.set_term(saved_term);
  write_entry.set_type(protocol::log::LogOpCode::DATA);
  write_entry.set_data(request.command());
  int write_id = Append(write_entry);

  m_write_command_sync.wait(lock, [this, write_id, saved_term] {
      return CommitIndex() >= write_id || Term() != saved_term;
  });

  if (Term() != saved_term) {
    reply.set_status(false);
    grpc::Status err = ConstructError("Peer is not a leader", protocol::raft::Error::Code::Error_Code_NOT_LEADER);
    m_state_machine->IncrementLastApplied();
    return std::make_tuple(reply, err);
  }

  if (!m_session->SessionExists(request.clientid())) {
    reply.set_status(false);
    grpc::Status err = ConstructError("Client session has expired", protocol::raft::Error::Code::Error_Code_SESSION_EXPIRED);
    m_state_machine->IncrementLastApplied();
    return std::make_tuple(reply, err);
  } else if (m_session->GetCachedResponse(request.clientid(), request.sequencenum(), reply)) {
    m_state_machine->IncrementLastApplied();
    return std::make_tuple(reply, grpc::Status::OK);
  }

  std::string response = m_state_machine->ApplyCommand(write_id, write_entry);
  reply.set_response(response);
  reply.set_status(true);
  m_session->CacheResponse(request.clientid(), request.sequencenum(), reply);
  return std::make_tuple(reply, grpc::Status::OK);
}

std::tuple<protocol::raft::ClientQuery_Response, grpc::Status> ConsensusModule::ProcessClientQueryClientRequest(
    protocol::raft::ClientQuery_Request& request) {
  protocol::raft::ClientQuery_Response reply;
  if (State() != RaftState::LEADER) {
    reply.set_status(false);
    grpc::Status err = ConstructError("Peer is not a leader", protocol::raft::Error::Code::Error_Code_NOT_LEADER);
    return std::make_tuple(reply, err);
  }

  if (!m_lease_holder.load()) {
    reply.set_status(false);
    grpc::Status err = ConstructError("Leader does not own lease to serve reads", protocol::raft::Error::Code::Error_Code_LEASE_EXPIRED);
    return std::make_tuple(reply, err);
  }

  try {
    std::string response = m_store->Read(request.query());
    reply.set_response(response);
  } catch (std::out_of_range) {
    reply.set_status(false);
    grpc::Status err = ConstructError("Key does not exist", protocol::raft::Error::Code::Error_Code_UNEXPECTED_ERROR);
    return std::make_tuple(reply, err);
  }

  reply.set_status(true);
  return std::make_tuple(reply, grpc::Status::OK);
}

}
