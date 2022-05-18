#include "global_ctx_manager.h"
#include "storage.h"

namespace raft {

Log::Log()
  : m_metadata(), m_log_size(0) {
}

Log::~Log() {
}

PersistedLog::Page::Page(
    const std::size_t start,
    const std::string& dir,
    const bool is_open,
    const std::size_t max_file_size)
  : is_open(is_open)
  , byte_offset(0)
  , start_index(start) 
  , end_index(start)
  , dir(dir)
  , filename("open-" + std::to_string(start))
  , log_entries({})
  , max_file_size(max_file_size) {
  std::string file_path = dir + filename;
  std::fstream out(file_path, std::ios::out | std::ios::binary);
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

void PersistedLog::Page::Close() {
  if (!is_open) {
    return;
  }
  is_open = false;

  std::string close_file_format = "%020lu-%020lu";
  auto size = std::snprintf(nullptr, 0, close_file_format.c_str(), start_index, end_index) + 1;
  std::unique_ptr<char[]> buffer(new char[size]);
  std::snprintf(buffer.get(), size, close_file_format.c_str(), start_index, end_index);
  std::string closed_filename = std::string(buffer.get(), buffer.get() + size - 1);
  std::filesystem::rename(dir + filename, dir + closed_filename);
  filename = closed_filename;
}

std::size_t PersistedLog::Page::RemainingSpace() const {
  return max_file_size - byte_offset;
}

bool PersistedLog::Page::WriteLogEntry(std::fstream& file, const protocol::log::LogEntry& new_entry) {
  bool success = google::protobuf::util::SerializeDelimitedToOstream(new_entry, &file);
  if (success) {
    byte_offset += new_entry.ByteSizeLong();
    end_index++;
    log_entries.push_back(new_entry);
  }
  return success;
}

PersistedLog::PersistedLog(
    const std::string& parent_dir,
    const std::size_t max_file_size)
  : Log()
  , m_dir(parent_dir)
  , m_max_file_size(max_file_size)
  , m_log_indices()
  , m_file_executor(std::make_shared<core::Strand>()) {
  std::filesystem::create_directories(parent_dir);

  RestoreState();
  if (!m_open_page) {
    m_open_page = std::make_shared<Page>(0, parent_dir, true, max_file_size);
    m_log_indices.insert({0, m_open_page});
  }
}

std::tuple<protocol::log::LogMetadata, bool> PersistedLog::Metadata() const {
  return m_metadata;
}

void PersistedLog::SetMetadata(const protocol::log::LogMetadata& metadata) {
  m_metadata = std::make_tuple(metadata, true);
  std::string metadata_path = m_dir + "metadata";
  PersistMetadata(metadata_path);
}

std::size_t PersistedLog::LogSize() const {
  return m_log_size;
}

ssize_t PersistedLog::LastLogIndex() const {
  return LogSize() - 1;
}

ssize_t PersistedLog::LastLogTerm() const {
  if (LogSize() > 0) {
    return Entry(LastLogIndex()).term();
  } else {
    return -1;
  }
}

protocol::log::LogEntry PersistedLog::Entry(const std::size_t idx) const {
  if (idx > LastLogIndex()) {
    Logger::Error("Raft log index out of bounds, index =", idx, "last_log_index =", LastLogIndex());
    throw std::out_of_range("Raft log index out of bounds");
  }

  auto it = m_log_indices.upper_bound(idx);
  it--;
  const auto& page = it->second;
  return page->log_entries[idx - page->start_index];
}

std::vector<protocol::log::LogEntry> PersistedLog::Entries(std::size_t start, std::size_t end) const {
  if (start > end || end > LastLogIndex() + 1) {
    Logger::Error("Raft log slice query invalid, start =", start, "end =", end, "last_log_index =", LastLogIndex());
    throw std::out_of_range("Raft log slice query invalid");
  }

  std::vector<protocol::log::LogEntry> query_entries;
  std::size_t curr = start;
  while (curr < end) {
    auto it = m_log_indices.upper_bound(curr);
    it--;
    const auto& page = it->second;

    start = start >= page->start_index ? start - page->start_index : 0;
    if (page->end_index >= end) {
      query_entries.insert(query_entries.end(), page->log_entries.begin() + start, page->log_entries.begin() + end - page->start_index);
    } else {
      query_entries.insert(query_entries.end(), page->log_entries.begin() + start, page->log_entries.end());
    }

    curr = page->end_index;
  }

  return query_entries;
}

bool PersistedLog::Append(const std::vector<protocol::log::LogEntry>& new_entries) {
  try {
    PersistLogEntries(new_entries);
    return true;
  } catch (const std::runtime_error& e) {
    return false;
  }
}

void PersistedLog::TruncateSuffix(const std::size_t removal_index) {
  Logger::Debug("Attempting to truncate log to index =", removal_index - 1);

  if (removal_index > LastLogIndex()) {
    return;
  }

  // Removal of only a portion of the open file
  if (removal_index > m_open_page->start_index) {
    // TODO: Replace current open file with new open file with updated log entries, end index, and byte offset
    // m_open_page->log_entries.erase(
    //     m_open_page->log_entries.begin() + removal_index - m_open_page->start_index,
    //     m_open_page->log_entries.end());
    return;
  }

  // TODO: Delete current open file and replace with empty open file
  m_log_indices.erase(m_open_page->start_index);

  while (!m_log_indices.empty()) {
    auto it = m_log_indices.rbegin();
    auto page = it->second;

    if (page->start_index >= removal_index) {
      // TODO: Handle logic for deleting entire closed page

    } else if (page->end_index >= removal_index) {
      // TODO: Handle logic for truncating closed page from [removal_index, page->end_index)

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
  return filename.substr(0, 4) == "open";
}

void PersistedLog::RestoreState() {
  auto file_list = ListDirectoryContents(m_dir);
  std::sort(file_list.begin(), file_list.end());
  for (const auto& filename:file_list) {
    std::string file_path = m_dir + filename;
    if (filename == "metadata") {
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
      std::size_t dash_index = filename.find('-');
      std::size_t start = std::stoi(filename.substr(0, dash_index));
      auto closed_page = std::make_shared<Page>(start, m_dir, false, m_max_file_size);

      m_log_size += restored_entries.size();
      m_log_indices.insert({start, closed_page});

      closed_page->end_index = closed_page->start_index + restored_entries.size();
      closed_page->log_entries = std::move(restored_entries);
    }
  }
}

void PersistedLog::PersistMetadata(const std::string& metadata_path) {
  std::fstream out(metadata_path, std::ios::out | std::ios::trunc | std::ios::binary);

  bool success = google::protobuf::util::SerializeDelimitedToOstream(std::get<0>(m_metadata), &out);
  if (!success) {
    Logger::Error("Unexpected serialization failure when persisting raft metadata to disk");
    throw std::runtime_error("Unable to serialize raft metadata");
  }
  out.flush();
}

void PersistedLog::PersistLogEntries(const std::vector<protocol::log::LogEntry>& new_entries) {
  std::string log_path = m_dir + m_open_page->filename;
  std::fstream out(log_path, std::ios::out | std::ios::app | std::ios::binary);

  bool success = true;
  for (auto &entry:new_entries) {
    if (entry.ByteSizeLong() > m_open_page->RemainingSpace()) {
      m_open_page->Close();
      out.close();

      m_open_page = std::make_shared<Page>(LogSize(), m_dir, true, m_max_file_size);
      m_log_indices.insert({m_open_page->start_index, m_open_page});

      log_path = m_dir + m_open_page->filename;
      out.open(log_path, std::ios::out | std::ios::app | std::ios::binary);
    }

    success = m_open_page->WriteLogEntry(out, entry);
    if (!success) {
      Logger::Error("Unexpected serialization failure when persisting raft log to disk");
      throw std::runtime_error("Unable to serialize raft log");
    }

    m_log_size++;
  }
  out.flush();
}

std::vector<protocol::log::LogEntry> PersistedLog::LoadLogEntries(const std::string& log_path) const {
  std::fstream in(log_path, std::ios::in | std::ios::binary);

  google::protobuf::io::IstreamInputStream log_stream(&in);
  std::vector<protocol::log::LogEntry> log_entries;
  protocol::log::LogEntry temp_log_entry;

  while (google::protobuf::util::ParseDelimitedFromZeroCopyStream(&temp_log_entry, &log_stream, nullptr)) {
    log_entries.push_back(temp_log_entry);
  }

  Logger::Debug("Restored log entries from disk, size =", log_entries.size());

  return log_entries;
}

protocol::log::LogMetadata PersistedLog::LoadMetadata(const std::string& metadata_path) const {
  std::fstream in(metadata_path, std::ios::in | std::ios::binary);

  google::protobuf::io::IstreamInputStream metadata_stream(&in);
  protocol::log::LogMetadata metadata;

  bool success = google::protobuf::util::ParseDelimitedFromZeroCopyStream(&metadata, &metadata_stream, nullptr);
  if (!success) {
    Logger::Error("Unable to restore metadata from disk");
    throw std::runtime_error("Unable to restore metadata");
  }

  Logger::Debug("Restored metadata from disk, term =", metadata.term(), "vote =", metadata.vote());

  return metadata;
}

}

