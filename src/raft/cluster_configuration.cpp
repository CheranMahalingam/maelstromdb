#include <glog/logging.h>

#include "cluster_configuration.h"

namespace raft {

ClusterConfiguration::ClusterConfiguration()
  : m_id(-1), m_state(ConfigurationState::EMPTY) {
}

protocol::log::Configuration ClusterConfiguration::Configuration() const {
  return m_current_configuration;
}

protocol::log::Configuration ClusterConfiguration::Configuration(int id) const {
  if (m_configurations.find(id) == m_configurations.end()) {
    LOG(FATAL) << "Cluster configuration at log index = " << id << " does not exist";
  }
  return m_configurations.at(id);
}

void ClusterConfiguration::SetConfiguration(int new_id, const protocol::log::Configuration& configuration) {
  LOG(INFO) << "Updating cluster configuration with id = " << new_id;
  if (configuration.next_configuration().size() == 0) {
    SetState(ConfigurationState::STABLE);
  } else {
    SetState(ConfigurationState::JOINT);
  }
  m_id = new_id;
  m_current_configuration = configuration;
  m_addresses.clear();

  for (auto& server:configuration.prev_configuration()) {
    m_addresses.insert(server.address());
  }
  for (auto& server:configuration.next_configuration()) {
    m_addresses.insert(server.address());
  }
}

int ClusterConfiguration::Id() const {
  return m_id;
}

ClusterConfiguration::ConfigurationState ClusterConfiguration::State() const {
  return m_state;
}

void ClusterConfiguration::SetState(ClusterConfiguration::ConfigurationState new_state) {
  m_state = new_state;
}

void ClusterConfiguration::InsertNewConfiguration(int new_id, const protocol::log::Configuration& new_configuration) {
  m_configurations.insert({new_id, new_configuration});
  auto it = m_configurations.rbegin();
  if (Id() != it->first) {
    SetConfiguration(it->first, it->second);
  }
}

std::unordered_set<std::string> ClusterConfiguration::ServerAddresses() const {
  if (State() == ConfigurationState::SYNC) {
    std::unordered_set<std::string> address_union(m_addresses);
    for (auto& address:m_log_sync->sync_addresses) {
      address_union.insert(address);
    }
    return address_union;
  }
  return m_addresses;
}

bool ClusterConfiguration::KnownServer(std::string address) {
  bool found = m_addresses.find(address) != m_addresses.end();
  if (State() == ConfigurationState::SYNC) {
    found |= m_log_sync->sync_addresses.find(address) != m_log_sync->sync_addresses.end();
  }
  return found;
}

void ClusterConfiguration::TruncateSuffix(int removal_index) {
  m_configurations.erase(m_configurations.upper_bound(removal_index), m_configurations.end());
  auto it = m_configurations.rbegin();
  if (Id() != it->first) {
    SetConfiguration(it->first, it->second);
  }
}

bool ClusterConfiguration::CheckQuorum(std::unordered_set<std::string> peer_votes) {
  if (State() == ConfigurationState::JOINT) {
    int prev_votes = 0;
    int next_votes = 0;
    for (auto old_server:m_current_configuration.prev_configuration()) {
      if (peer_votes.find(old_server.address()) != peer_votes.end()) {
        prev_votes++;
      }
    }
    for (auto new_server:m_current_configuration.next_configuration()) {
      if (peer_votes.find(new_server.address()) != peer_votes.end()) {
        next_votes++;
      }
    }
    bool prev_majority = prev_votes*2 > m_current_configuration.prev_configuration().size();
    bool next_majority = next_votes*2 > m_current_configuration.next_configuration().size();
    return prev_majority && next_majority;
  } else {
    return peer_votes.size()*2 > m_current_configuration.prev_configuration().size();
  }
}

void ClusterConfiguration::StartLogSync(int commit_index, const std::vector<std::string>& new_servers) {
  DLOG(INFO) << "Starting membership change log sync...";
  std::vector<std::string> sync_servers;
  for (auto& server:new_servers) {
    if (!KnownServer(server)) {
      sync_servers.push_back(server);
    }
  }

  m_log_sync.reset(new SyncState(commit_index, sync_servers));
  SetState(ConfigurationState::SYNC);
}

void ClusterConfiguration::CancelLogSync() {
  m_log_sync.reset();
  SetState(ConfigurationState::STABLE);
}

bool ClusterConfiguration::SyncProgress() {
  std::lock_guard<std::mutex> lock(m_log_sync->sync_mutex);
  return m_log_sync->progress;
}

bool ClusterConfiguration::UpdateSyncProgress(std::string address, int new_match_index) {
  if (m_state != ConfigurationState::SYNC) {
    return false;
  }
  std::lock_guard<std::mutex> lock(m_log_sync->sync_mutex);
  auto [prev_index, _] = m_log_sync->state_diff[address];
  m_log_sync->state_diff[address] = {prev_index, new_match_index};

  bool progress = true;
  bool done = true;
  for (auto& it:m_log_sync->state_diff) {
    auto [prev_index, curr_index] = it.second;
    if (prev_index >= curr_index) {
      progress = false;
      done = false;
      break;
    }
    if (curr_index < m_log_sync->sync_index) {
      done = false;
    }
  }
  m_log_sync->progress = progress;
  m_log_sync->done = done;
  return progress;
}

bool ClusterConfiguration::SyncComplete() {
  std::lock_guard<std::mutex> lock(m_log_sync->sync_mutex);
  return m_log_sync->done;
}

ClusterConfiguration::SyncState::SyncState(int commit_index, const std::vector<std::string>& new_servers)
  : sync_index(commit_index), done(false), progress(false) {
  for (auto& address:new_servers) {
    state_diff[address] = {0, 0};
    sync_addresses.insert(address);
  }
}

}

