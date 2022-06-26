#ifndef CONCENSUS_MODULE_H
#define CONCENSUS_MODULE_H

#include <grpcpp/grpcpp.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "async_executor.h"
#include "cluster_configuration.h"
#include "logger.h"
#include "raft.grpc.pb.h"
#include "timer.h"

namespace raft {

class GlobalCtxManager;

class ConcensusModule {
public:
  using clock_type = std::chrono::high_resolution_clock;
  using time_point = std::chrono::time_point<clock_type>;
  using milliseconds = std::chrono::milliseconds;

  enum class RaftState {
    /**
     * Indicates that this node should handle all read/write requests.
     * Only 1 node can be a LEADER in a cluster at any time.
     * Every 50ms a LEADER will send a heartbeat message to reset election
     * timers and synchronize entries in the raft log. Resets to a FOLLOWER
     * if term is out of date (can happen if node gets
     * partitioned).
     */
    LEADER,
    /**
     * Indicates that the node is running in the election to become a LEADER.
     * Sends RequestVote RPCs to other nodes and once a majority vote for this
     * node it is promoted to LEADER. Resets to a FOLLOWER if term is out of date
     * or if a heartbeat message is received due to another node becoming a LEADER.
     */
    CANDIDATE,
    /**
     * Stable state running an election timer every 150-300ms. If a heartbeat message
     * is received from the cluster LEADER the timer is reset. Otherwise, a new
     * election is started and the node is promoted to CANDIDATE.
     */
    FOLLOWER,
    /**
     * State achieved when the node is shutdown. All incoming requests
     * will be rejected.
     */
    DEAD
  };

public:
  ConcensusModule(GlobalCtxManager& ctx);

  ConcensusModule(const ConcensusModule&) = delete;
  ConcensusModule& operator=(const ConcensusModule&) = delete;

  /**
   * Initializes concensus module by restoring metadata from disk and starting
   * an election timer.
   *
   * @param delay number of seconds after which the election timer is started.
   *    Used for debugging purposes.
   */
  void StateMachineInit(int delay = 0);

  void InitializeConfiguration(std::vector<std::string>& peer_addresses);

  /**
   * Retrieve raft term of node.
   *
   * @return m_term
   */
  int Term() const;

  /**
   * Retrieve state of node (LEADER, CANDIDATE, FOLLOWER, DEAD).
   *
   * @return m_state
   */
  RaftState State() const;

  /**
   * Set the node's state to FOLLOWER and reset the election timer.
   *
   * @param term the new raft term for the node
   */
  void ResetToFollower(const int term);

  /**
   * Set the node's state to LEADER and start the heartbeat timer.
   */
  void PromoteToLeader();

  /**
   * Handles RequestVote RPC request. If the node has yet to vote and the
   * raft log of the client is ahead of the server then the node grants a vote.
   * previously known log index/term from the CANDIDATE matches the state of
   * the node.
   *
   * @param request the RequestVote RPC sent from the client
   * @returns a RequestVote RPC response indicating whether the a vote was
   *    granted and the current raft term. Contains a status message
   *    indicating whether the node rejected the request due to being DEAD.
   */
  std::tuple<protocol::raft::RequestVote_Response, grpc::Status> ProcessRequestVoteClientRequest(
      protocol::raft::RequestVote_Request& request);

  /**
   * Handles response from servers for the RequestVote RPC. Determines 
   * whether a vote was granted and promotes node to LEADER if it receives
   * a majority of votes.
   *
   * @param request the RequestVote RPC that was sent to the server
   * @param reply the reply message sent from the server
   */
  void ProcessRequestVoteServerResponse(
      protocol::raft::RequestVote_Request& request,
      protocol::raft::RequestVote_Response& reply,
      const std::string& address);

  /**
   * Handles AppendEntries RPC request. Compares raft log indices to append
   * new entries and commits entries that were committed by the LEADER.
   *
   * @param request the AppendEntries RPC that was sent from the LEADER
   * @returns AppendEntries RPC response containing the raft term and a boolean
   *    for whether the previously known log index and term from the LEADER matches
   *    the state of the node. Contains a status message indicating whether the node
   *    rejected the request due to being DEAD.
   */
  std::tuple<protocol::raft::AppendEntries_Response, grpc::Status> ProcessAppendEntriesClientRequest(
      protocol::raft::AppendEntries_Request& request);

  /**
   * Handles response from servers for the AppendEntries RPC. Determines
   * whether a majority of nodes have stored an entry in their raft log and
   * commits those entries. If there was a mismatch in the raft log term between
   * the nodes then the next time the AppendEntries RPC will be sent with an
   * earlier prevLogIndex.
   *
   * @param request the AppendEntries RPC that was sent to the server
   * @param reply the AppendEntries RPC response that was sent from the server
   * @param address the ip address of the server that responded
   */
  void ProcessAppendEntriesServerResponse(
      protocol::raft::AppendEntries_Request& request,
      protocol::raft::AppendEntries_Response& reply,
      const std::string& address);

  std::tuple<protocol::raft::GetConfiguration_Response, grpc::Status> ProcessGetConfigurationClientRequest();

  std::tuple<protocol::raft::SetConfiguration_Response, grpc::Status> ProcessSetConfigurationClientRequest(
      protocol::raft::SetConfiguration_Request& request);

private:
  /**
   * Callback for ScheduleElection when election timer times out. Promotes node to
   * CANDIDATE state and sends RequestVote RPCs to all nodes to get votes.
   * Starts a new election timer in case a CANDIDATE in cluster does not get
   * a majority of votes within timeout.
   *
   * @param term the raft term when the election was originally scheduled
   */
  void ElectionCallback(const int term);

  /**
   * Callback for ScheduleHeartbeat when heartbeat timer times out. Only called by node
   * that is a LEADER. Sends heartbeat message to other nodes to indicate that the cluster
   * is healthy and replicate writes.
   */
  void HeartbeatCallback();

  /**
   * Creates an election timer with a random timeout to trigger an election on expiry.
   * Random timeout reduces chances of multiple nodes requesting votes at the same time
   * so that a node frequently receives a majority without multiple elections.
   *
   * @param term the saved raft term
   */
  void ScheduleElection(const int term);

  /**
   * Creates a heartbeat timer that triggers sending heartbeat messages on expiry. Only
   * called by node in LEADER state.
   */
  void ScheduleHeartbeat();

  /**
   * Persists raft metadata (term, vote) to disk.
   */
  void StoreState() const;

  /**
   * Raft state transitions to DEAD and no longer responds to other nodes.
   */
  void Shutdown();

  int Append(protocol::log::LogEntry& log_entry);
  std::pair<int, int> Append(std::vector<protocol::log::LogEntry>& log_entries);

  void CommitEntries(std::vector<protocol::log::LogEntry>& log_entries);

private:
  /**
   * Global context object that gives state machine access to client, server,
   * and raft log.
   */
  GlobalCtxManager& m_ctx;

  std::unique_ptr<ClusterConfiguration> m_configuration;

  /**
   * Execution handler that runs functions using a managed pool of threads.
   * Currently using Strand (single threaded execution handler).
   */
  std::shared_ptr<core::AsyncExecutor> m_timer_executor;

  /**
   * Asynchronous timer used by CANDIDATE and FOLLOWER nodes to trigger
   * leader election. Expires with a random timeout between 150 and 300ms.
   */
  std::shared_ptr<core::DeadlineTimer> m_election_timer;

  /**
   * Asynchronous timer used by LEADER to send heartbeat messages
   * to other nodes. Expires every 50ms.
   */
  std::shared_ptr<core::DeadlineTimer> m_heartbeat_timer;

  /**
   * The address of the CANDIDATE node that this node voted for. 
   */
  std::string m_vote;

  /**
   * The number of votes the node received as a CANDIDATE. Used to determine
   * whether it received a majority and can be promoted to LEADER.
   */
  int m_votes_received;

  /**
   * The raft term of the node. Used to determine whether a node is out of date.
   */
  std::atomic<int> m_term;

  /**
   * The raft election state of the node.
   */
  std::atomic<RaftState> m_state;

  /**
   * The index of the highest raft log entry that has been applied to the state machine.
   * When m_commit_index > m_last_applied continually increment m_last_applied while
   * running the commands in the log at that index.
   */
  std::atomic<int> m_last_applied;

  /**
   * The last index of the raft log with an entry that has been committed. Once an entry
   * is committed the command can be applied to the state machine.
   */
  std::atomic<int> m_commit_index;

  /**
   * Index of the next log entry to send to each other node. Used to determine the index
   * where new log entries from the LEADER will be replicated.
   */
  std::unordered_map<std::string, int> m_next_index;

  /**
   * Index of the highest log entry that has been replicated on each other node.
   * Used to determine when an entry can be committed (when a majority of nodes
   * have a match index >= N).
   */
  std::unordered_map<std::string, int> m_match_index;

  /**
   * The address of the current LEADER node. Useful as a hint when handling requests
   * to redirect to the LEADER. Currently not implemented.
   */
  std::string m_leader_id;

  /**
   * The randomly generated [150, 300] delay in ms after which an election will begin.
   * Used for calculating the election deadline.
   */
  milliseconds m_election_timeout;

  /**
   * The time at which the node will start an election. Useful for rejecting RequestVote
   * RPCs from nodes that have been removed from the cluster configuration.
   */
  time_point m_election_deadline;
};

}

#endif

