#ifndef STORAGE_H 
#define STORAGE_H

#include <google/protobuf/io/zero_copy_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/util/delimited_message_util.h>
#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "async_executor.h"
#include "log.grpc.pb.h"

namespace raft {

class GlobalCtxManager;

class Log {
public:
  const std::unordered_map<protocol::log::LogOpCode, std::string> OPCODE_NAME = {
    {protocol::log::NO_OP, "NO_OP"},
    {protocol::log::CONFIGURATION, "CONFIGURATION"},
    {protocol::log::REGISTER_CLIENT, "REGISTER_CLIENT"},
    {protocol::log::DATA, "DATA"}
  };

public:
  Log();
  virtual ~Log();

  /**
   * Getter for raft metadata.
   *
   * @param[out] metadata the placeholder where the current metadata is written to
   * @returns metadata containing node's previous term and vote. Boolean is used
   *    to indicate whether the metadata is empty.
   */
  virtual bool Metadata(protocol::log::LogMetadata& metadata) const = 0;

  /**
   * Setter for raft metadata.
   *
   * @param metadata contains the current term and vote for the node
   */
  virtual void SetMetadata(protocol::log::LogMetadata& metadata) = 0;

  /**
   * Getter for raft log size.
   *
   * @returns number of entries in raft log
   */
  virtual int LogSize() const = 0;

  /**
   * Gets the index of the last entry in the raft log.
   *
   * @returns index of last log entry. Defaults to -1 when the log is empty.
   */
  virtual int LastLogIndex() const = 0;

  /**
   * Gets the term of the last entry in the raft log.
   *
   * @returns term of last log entry. Defaults to -1 when the log is empty.
   */
  virtual int LastLogTerm() const = 0;

  /**
   * Retrieve entry at specific index in raft log.
   *
   * @param idx the index of raft log entry
   * @returns raft log entry
   * @throws std::out_of_range Thrown if requested index does not exist in log.
   */
  virtual protocol::log::LogEntry Entry(const int idx) const = 0;

  /**
   * Retrieves entries between two indices from raft log.
   *
   * @param start the starting index at which entries are retrieved (inclusive)
   * @param end the ending index at which entries are retrieved (exclusive)
   * @returns list of raft log entries
   * @throws std::out_of_range Thrown if requested range does not exist in log.
   */
  virtual std::vector<protocol::log::LogEntry> Entries(int start, int end) const = 0;

  virtual std::tuple<int, bool> LatestConfiguration(protocol::log::Configuration& configuration) const = 0;

  virtual int Append(protocol::log::LogEntry& new_entry) = 0;

  /**
   * Stores new transactions at the end of the raft log.
   *
   * @param new_entries list of entries that must be appended to log
   */
  virtual std::pair<int, int> Append(const std::vector<protocol::log::LogEntry>& new_entries) = 0;

  /**
   * Removes all entries from raft log at and after a given index.
   *
   * @param removal_index the index in the raft log where truncation begins
   */
  virtual void TruncateSuffix(const int removal_index) = 0;

protected:
  /**
   * Number of entries in raft log.
   */
  int m_log_size;

  /**
   * Term and vote data of node. This is persisted to disk everytime there is a
   * modification so that the node can catch up after recovering from a server
   * failure. If the version number is 0 then the metadata has not been set.
   */
  protocol::log::LogMetadata m_metadata;
};

class PersistedLog : public Log {
public:
  PersistedLog(
      const std::string& parent_dir,
      bool restore=false,
      const int max_file_size = 1024*8);

  bool Metadata(protocol::log::LogMetadata& metadata) const override;
  void SetMetadata(protocol::log::LogMetadata& metadata) override;

  int LogSize() const override;

  int LastLogIndex() const override;
  int LastLogTerm() const override;

  protocol::log::LogEntry Entry(const int idx) const override;
  std::vector<protocol::log::LogEntry> Entries(int start, int end) const override;

  std::tuple<int, bool> LatestConfiguration(protocol::log::Configuration& configuration) const override;

  int Append(protocol::log::LogEntry& new_entry) override;
  std::pair<int, int> Append(const std::vector<protocol::log::LogEntry>& new_entries) override;

  void TruncateSuffix(const int removal_index) override;

  class Page {
  public:
    struct Record {
      Record(int offset, protocol::log::LogEntry entry);

      /**
       * Raft log entry persisted to disk.
       */
      protocol::log::LogEntry entry;

      /**
       * Byte offset in file where the log entry binary data starts.
       */
      int offset;
    };

  public:
    Page(
        const int start,
        const std::string& dir,
        const bool is_open,
        const int max_file_size);

    Page(const Page&& page);
    Page& operator=(const Page&& page);

    /**
     * Closes open pages and renames file to closed file format.
     */
    void Close();

    /**
     * Determines the remaining space in a file. Used to determine when the file
     * should be closed.
     *
     * @returns number of bytes remaining in a file
     */
    int RemainingSpace() const;

    /**
     * Appends log entry to the end of the file without flushing to disk.
     *
     * @param file an opened file where log entry is stored
     * @param new_entry the log entry that must be persisted
     * @returns whether the entry was successfully written to disk
     */
    bool WriteLogEntry(std::fstream& file, const protocol::log::LogEntry& new_entry);

    /**
     * Removes all log entries from the page at and after an index, N, from disk and memory.
     * Entries are deleted by truncating the file and the file is renamed to reflect the new
     * start and end indices.
     *
     * @param removal_index the index where log entries begin to be deleted.
     */
    void TruncateSuffix(int removal_index);

  private:
    /**
     * Generates the filename for a closed file. The format is `start_index-end_index` where
     * start_index is the log index of the first entry in the file and end_index is log index + 1
     * of the last entry in the file.
     *
     * @returns formatted name for closed file
     */
    std::string ClosedFilename() const;

  public:
    /**
     * Indicates whether the page is open. New log entries are appended to open files.
     */
    bool is_open;

    /**
     * Position in file where new entries should be appended. Useful for determining
     * the space remaining in a file and truncating log entries.
     */
    int byte_offset;

    /**
     * Raft log index for first entry stored in the file.
     */
    int start_index;

    /**
     * Raft log index + 1 for last entry stored in the file.
     */
    int end_index;

    /**
     * Directory where file is being stored.
     */
    std::string dir;

    /**
     * Name of the file. Open files have the format open-N where N is the start index.
     * Closed files have format M-N where M is the start index and N is the end index.
     */
    std::string filename;

    /**
     * In memory representation of raft log entries stored on file.
     */
    std::vector<Record> log_entries;

    /**
     * Filesize limit. Once there is no space for additional log entries the page is closed
     * and a new file is created to store new entries. Defaults to 1KB.
     */
    const int max_file_size;
  };

private:
  /**
   * Get all files in a directory (does not recursively iterate through folders).
   *
   * @param dir the directory under which files are searched
   * @returns filenames of all files in the directory
   */
  std::vector<std::string> ListDirectoryContents(const std::string& dir);

  /**
   * Indicates whether a file has no data by checking the file size.
   *
   * @param path the absolute path to a file
   * @returns whether the file contains no data
   */
  bool Empty(const std::string& path) const;

  /**
   * Determines whether the file can have log entries appended by checking the
   * prefix of the filename.
   *
   * @param filename the name of the file
   * @returns whether file is in an open state
   */
  bool IsFileOpen(const std::string& filename) const;

  /**
   * Read latest raft metadata from disk after a recovering from a server failure.
   */
  void RestoreState();

  /**
   * Write new raft metadata to disk.
   *
   * @param metadata_path the absolute path to the metadata file
   * @throws std::runtime_error thrown if there was an error serializing metadata to disk
   */
  void PersistMetadata(const std::string& metadata_path);

  /**
   * Persist new raft log entries to disk. Appended to end of open file. If there is not
   * enough space in open file, the file is closed and entries are written to a new file.
   *
   * @param new_entries the log entries that must be persisted
   * @throws std::runtime_error thrown if there was an error serializing a log entry to disk
   */
  void PersistLogEntries(const std::vector<protocol::log::LogEntry>& new_entries);

  /**
   * Read raft metadata from disk.
   *
   * @param metadata_path the absolute path to the metadata file
   * @throws std::runtime_error thrown if metadata was unsuccessfully read from disk
   */
  void LoadMetadata(const std::string& metadata_path);

  /**
   * Read log entries from a file on disk.
   *
   * @param log_path the absolute path to a file containing logs
   * @returns all log entries in a file
   */
  std::vector<Page::Record> LoadLogEntries(const std::string& log_path) const;

  /**
   * Closes currently open file and creates a new open page object.
   */
  void CreateOpenFile();

private:
  /**
   * The page where log entries are written to. If the page becomes full, the page is
   * closed and a new open page is created.
   */
  std::shared_ptr<Page> m_open_page;

  /**
   * An execution manager used to execute methods using a pool of workers.
   */
  std::shared_ptr<core::AsyncExecutor> m_io_executor;

  /**
   * Absolute path to directory where raft log files are stored.
   */
  std::string m_dir;

  /**
   * Maps each files start index (raft log index of first log entry in file) to a page object.
   * Used as in memory representation of entire raft log to query log entries quickly.
   */
  std::map<int, std::shared_ptr<Page>> m_log_indices;

  /**
   * Filesize limit. Once there is no space for additional log entries the page is closed
   * and a new file is created to store new entries. Defaults to 1KB.
   */
  const int m_max_file_size;
};

}

#endif

