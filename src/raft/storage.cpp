#include <glog/logging.h>

#include "global_ctx_manager.h"
#include "storage.h"
#include <algorithm>

namespace raft {

Log::Log()
  : m_metadata(), m_log_size(0) {
}

Log::~Log() {
}

PersistedLog::Page::Page(
    const int start,
    const std::string& dir,
    const bool is_open,
    const int max_file_size)
  : is_open(is_open)
  , byte_offset(0)
  , start_index(start) 
  , end_index(start)
  , dir(dir)
  , filename("open-" + std::to_string(start))
  , log_entries({})
  , max_file_size(max_file_size) {
  std::string file_path = dir + filename;
  std::fstream out(file_path, std::ios::out | std::ios::app | std::ios::binary);
}

PersistedLog::Page::Page(const Page&& page)
  : is_open(page.is_open)
  , byte_offset(page.byte_offset)
  , start_index(page.start_index)
  , end_index(page.end_index)
  , dir(page.dir)
  , filename(page.filename)
  , log_entries(std::move(page.log_entries))
  , max_file_size(page.max_file_size) {
}

PersistedLog::Page& PersistedLog::Page::operator=(const Page&& page) {
  is_open = page.is_open;
  byte_offset = page.byte_offset;
  start_index = page.start_index;
  end_index = page.end_index;
  dir = page.dir;
  filename = page.filename;
  log_entries = std::move(page.log_entries);
  return *this;
} 

PersistedLog::Page::Record::Record(int offset, protocol::log::LogEntry entry)
  : offset(offset), entry(entry) {
}

void PersistedLog::Page::Close() {
  if (!is_open) {
    return;
  }
  is_open = false;

  // Delete the file if there are no entries in the file
  if (start_index == end_index) {
    std::filesystem::remove(dir + filename);
    return;
  }

  auto new_filename = ClosedFilename();
  std::filesystem::rename(dir + filename, dir + new_filename);
  filename = new_filename;
}

int PersistedLog::Page::RemainingSpace() const {
  return max_file_size - byte_offset;
}

bool PersistedLog::Page::WriteLogEntry(std::fstream& file, const protocol::log::LogEntry& new_entry) {
  // Note that the result isn't flushed to disk so that writes can be batched
  bool success = google::protobuf::util::SerializeDelimitedToOstream(new_entry, &file);
  if (success) {
    log_entries.push_back(Page::Record(byte_offset, new_entry));
    byte_offset += new_entry.ByteSizeLong();
    end_index++;
  }
  return success;
}

void PersistedLog::Page::TruncateSuffix(int removal_index) {
  int last_record_index = removal_index - start_index;
  int truncate_offset = log_entries[last_record_index].offset;

  // Resizing file sets data past offset to zeroes
  std::filesystem::resize_file(dir + filename, truncate_offset);

  log_entries.erase(
        log_entries.begin() + last_record_index,
        log_entries.end());
  byte_offset = truncate_offset;
  end_index = removal_index;

  // Closed file must be renamed since end_index has changed
  if (!is_open) {
    std::string new_filename = ClosedFilename();
    std::filesystem::rename(dir + filename, dir + new_filename);
    filename = new_filename;
  }

}

std::string PersistedLog::Page::ClosedFilename() const {
  std::string close_file_format = "%020lu-%020lu";
  auto size = std::snprintf(nullptr, 0, close_file_format.c_str(), start_index, end_index) + 1;
  std::unique_ptr<char[]> buffer(new char[size]);
  std::snprintf(buffer.get(), size, close_file_format.c_str(), start_index, end_index);
  std::string closed_filename = std::string(buffer.get(), buffer.get() + size - 1);
  return closed_filename;
}

PersistedLog::PersistedLog(
    const std::string& parent_dir,
    bool restore,
    const int max_file_size)
  : Log()
  , m_dir(parent_dir)
  , m_max_file_size(max_file_size)
  , m_log_indices()
  , m_io_executor(std::make_shared<core::Strand>()) {
  std::filesystem::create_directories(parent_dir);

  // Restores raft metadata and log entries from disk after recovering from server failure
  if (restore) {
    RestoreState();
  }
  // If there were no log entries to restore a new open page must be created to store new
  // log entries
  if (!m_open_page) {
    m_open_page = std::make_shared<Page>(0, parent_dir, true, max_file_size);
    m_log_indices.insert({0, m_open_page});
  }
}

bool PersistedLog::Metadata(protocol::log::LogMetadata& metadata) const {
  metadata.set_term(m_metadata.term());
  metadata.set_vote(m_metadata.vote());
  metadata.set_version(m_metadata.version());
  return metadata.version() != 0;
}

void PersistedLog::SetMetadata(protocol::log::LogMetadata& metadata) {
  metadata.set_version(m_metadata.version() + 1);
  m_metadata = metadata;
  std::string metadata_path = m_dir + "metadata";
  if (m_metadata.version() % 2 == 0) {
    metadata_path += "1";
  } else {
    metadata_path += "2";
  }
  PersistMetadata(metadata_path);
}

int PersistedLog::LogSize() const {
  return m_log_size;
}

int PersistedLog::LastLogIndex() const {
  return LogSize() - 1;
}

int PersistedLog::LastLogTerm() const {
  if (LogSize() > 0) {
    return Entry(LastLogIndex()).term();
  } else {
    return -1;
  }
}

protocol::log::LogEntry PersistedLog::Entry(const int idx) const {
  if (idx > LastLogIndex() || idx < 0) {
    LOG(FATAL) << "Raft log index out of bounds, index = " << idx << " last_log_index = " << LastLogIndex();
  }

  // Upper bound gets page with start_index > idx so that previous page in map is correct page
  auto it = m_log_indices.upper_bound(idx);
  it--;
  const auto& page = it->second;
  return page->log_entries[idx - page->start_index].entry;
}

std::vector<protocol::log::LogEntry> PersistedLog::Entries(int start, int end) const {
  if (start > end || end > LastLogIndex() + 1 || start < 0) {
    LOG(FATAL) << "Raft log slice query invalid, start = " << start << " end = " << end << " last_log_index = " << LastLogIndex();
  }

  std::vector<Page::Record> query_records;
  int curr = start;
  while (curr < end) {
    // Upper bound gets page with start_index > idx so that previous page in map is correct page
    auto it = m_log_indices.upper_bound(curr);
    it--;
    const auto& page = it->second;

    // Prevent start - page->start_index from being negative to prevent indexing out of bounds
    start = start >= page->start_index ? start - page->start_index : 0;
    // If end is inside the upper range then entries in [start, end - start_index) must be retrieved
    // If end is outside the upper range then entries in [start, end) are retrieved
    if (page->end_index >= end) {
      query_records.insert(query_records.end(), page->log_entries.begin() + start, page->log_entries.begin() + end - page->start_index);
    } else {
      query_records.insert(query_records.end(), page->log_entries.begin() + start, page->log_entries.end());
    }

    // The end index matches the start index of the next page
    curr = page->end_index;
  }

  std::vector<protocol::log::LogEntry> query_entries;
  for (auto& record:query_records) {
    query_entries.push_back(record.entry);
  }

  return query_entries;
}

std::tuple<int, bool> PersistedLog::LatestConfiguration(protocol::log::Configuration& configuration) const {
  for (auto it = m_log_indices.rbegin(); it != m_log_indices.rend(); it++) {
    int log_index = it->second->start_index;
    for (auto& record:it->second->log_entries) {
      if (record.entry.has_configuration()) {
        *configuration.mutable_prev_configuration() = record.entry.configuration().prev_configuration();
        *configuration.mutable_next_configuration() = record.entry.configuration().next_configuration();
        return std::make_tuple(log_index, true);
      }
      log_index++;
    }
  }
  return std::make_tuple(0, false);
}

int PersistedLog::Append(protocol::log::LogEntry& new_entry) {
  DLOG(INFO) << "Appending " << OPCODE_NAME.at(new_entry.type()) << " entry to raft log...";
  int log_index = LogSize();
  PersistLogEntries({new_entry});
  return log_index;
}

std::pair<int, int> PersistedLog::Append(const std::vector<protocol::log::LogEntry>& new_entries) {
  int start = LogSize();
  PersistLogEntries(new_entries);
  int end = LogSize();
  return {start, end};
}

void PersistedLog::TruncateSuffix(const int removal_index) {
  DLOG(INFO) << "Attempting to truncate log at index = " << removal_index;

  if (removal_index > LastLogIndex()) {
    return;
  }

  // Removal of only a portion of the open file
  if (removal_index > m_open_page->start_index) {
    m_log_size -= m_open_page->end_index - removal_index;
    m_open_page->TruncateSuffix(removal_index);
    CreateOpenFile();
    return;
  }

  // Delete current open file and replace with empty open file
  m_log_indices.erase(m_open_page->start_index);
  m_log_size -= m_open_page->end_index - m_open_page->start_index;
  m_open_page->end_index = m_open_page->start_index;
  m_open_page->byte_offset = 0;
  m_open_page->log_entries = {};
  CreateOpenFile();

  while (!m_log_indices.empty()) {
    auto it = m_log_indices.rbegin();
    auto page = it->second;

    if (page->start_index >= removal_index) {
      // Delete all log entries of closed file
      std::filesystem::remove(m_dir + page->filename);
      m_log_indices.erase(page->start_index);
      m_log_size -= page->end_index - page->start_index;
    } else if (page->end_index > removal_index) {
      // Removal of only a portion of log entries in a closed file
      m_log_size -= page->end_index - removal_index;
      page->TruncateSuffix(removal_index);
      return;
    } else {
      return;
    }
  }
}

std::vector<std::string> PersistedLog::ListDirectoryContents(const std::string& dir) {
  std::vector<std::string> file_list;
  for (const auto& entry:std::filesystem::directory_iterator(dir)) {
    if (std::filesystem::is_regular_file(entry)) {
      file_list.push_back(entry.path().filename());
    }
  }
  return file_list;
}

bool PersistedLog::Empty(const std::string& path) const {
  return std::filesystem::file_size(path) == 0;
}

bool PersistedLog::IsFileOpen(const std::string& filename) const {
  // Only 1 file is ever open and it has `open` as a prefix 
  return filename.substr(0, 4) == "open";
}

void PersistedLog::RestoreState() {
  auto file_list = ListDirectoryContents(m_dir);
  // Sorting files makes unit testing easier
  std::sort(file_list.begin(), file_list.end());
  for (const auto& filename:file_list) {
    std::string file_path = m_dir + filename;
    if (filename == "metadata1" || filename == "metadata2") {
      LoadMetadata(file_path);
      continue;
    }

    auto restored_entries = LoadLogEntries(file_path);
    if (IsFileOpen(filename)) {
      m_open_page = std::make_shared<Page>(LogSize(), m_dir, true, m_max_file_size);
      m_log_indices.insert({m_open_page->start_index, m_open_page});
      m_log_size += restored_entries.size();

      m_open_page->end_index = m_open_page->start_index + restored_entries.size();
      m_open_page->log_entries = std::move(restored_entries);
    } else {
      // Since closed file name is of form `start-end` the start index can be determined using
      // the prefix of the filename
      int dash_index = filename.find('-');
      int start = std::stoi(filename.substr(0, dash_index));
      auto closed_page = std::make_shared<Page>(start, m_dir, false, m_max_file_size);

      m_log_indices.insert({start, closed_page});
      m_log_size += restored_entries.size();

      closed_page->end_index = closed_page->start_index + restored_entries.size();
      closed_page->log_entries = std::move(restored_entries);
    }
  }
}

void PersistedLog::PersistMetadata(const std::string& metadata_path) {
  std::fstream out(metadata_path, std::ios::out | std::ios::trunc | std::ios::binary);

  bool success = google::protobuf::util::SerializeDelimitedToOstream(m_metadata, &out);
  if (!success) {
    LOG(FATAL) << "Unexpected serialization failure when persisting raft metadata to disk";
  }
  out.flush();
}

void PersistedLog::PersistLogEntries(const std::vector<protocol::log::LogEntry>& new_entries) {
  std::string log_path = m_dir + m_open_page->filename;
  std::fstream out(log_path, std::ios::out | std::ios::app | std::ios::binary);

  bool success = true;
  for (auto &entry:new_entries) {
    // If there is no space remaining in current open file open a new file
    if (entry.ByteSizeLong() > m_open_page->RemainingSpace()) {
      CreateOpenFile();
      out.close();

      log_path = m_dir + m_open_page->filename;
      out.open(log_path, std::ios::out | std::ios::app | std::ios::binary);
    }

    success = m_open_page->WriteLogEntry(out, entry);
    if (!success) {
      LOG(FATAL) << "Unexpected serialization failure when persisting raft log to disk";
    }

    m_log_size++;
  }

  // Write all entries to write buffer before flushing to improve throughput
  out.flush();
}

std::vector<PersistedLog::Page::Record> PersistedLog::LoadLogEntries(const std::string& log_path) const {
  std::fstream in(log_path, std::ios::in | std::ios::binary);

  google::protobuf::io::IstreamInputStream log_stream(&in);
  std::vector<Page::Record> log_entries;
  protocol::log::LogEntry temp_log_entry;

  while (google::protobuf::util::ParseDelimitedFromZeroCopyStream(&temp_log_entry, &log_stream, nullptr)) {
    int offset = log_stream.ByteCount() - temp_log_entry.ByteSizeLong() - 1;
    log_entries.push_back(Page::Record(offset, temp_log_entry));
  }

  DLOG(INFO) << "Restored log entries from disk, size = " << log_entries.size();

  return log_entries;
}

void PersistedLog::LoadMetadata(const std::string& metadata_path) {
  std::fstream in(metadata_path, std::ios::in | std::ios::binary);

  google::protobuf::io::IstreamInputStream metadata_stream(&in);
  protocol::log::LogMetadata metadata;

  bool success = google::protobuf::util::ParseDelimitedFromZeroCopyStream(&metadata, &metadata_stream, nullptr);
  if (!success) {
    LOG(FATAL) << "Unable to restore metadata from disk";
  }

  // Retrieve the metadata with the higher version number
  if (metadata.version() < m_metadata.version()) {
    return;
  }

  DLOG(INFO) << "Restored metadata from disk, term = " << metadata.term() << " vote = " << metadata.vote();

  m_metadata = metadata;
}

void PersistedLog::CreateOpenFile() {
  m_open_page->Close();

  m_open_page = std::make_shared<Page>(LogSize(), m_dir, true, m_max_file_size);
  m_log_indices.insert({m_open_page->start_index, m_open_page});
}

}

