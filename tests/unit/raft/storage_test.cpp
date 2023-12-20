#include <gtest/gtest.h>
#include <memory>

#include "global_ctx_manager.h"
#include "log.grpc.pb.h"
#include "storage.h"

namespace raft {

class LogTestBase : public ::testing::Test {
protected:
  void SetUp(const int count, const int max_file_size = 1024*8) {
    log = std::make_unique<PersistedLog>(
        std::filesystem::current_path().string() + "/test_log/",
        false,
        max_file_size);
    entry_count = count;

    for (int i = 0; i < entry_count; i++) {
      protocol::log::LogEntry new_entry;
      new_entry.set_term(i);
      new_entry.set_data("test" + std::to_string(i));
      entries.push_back(new_entry);
    }

    log->Append(entries);
  }

  ~LogTestBase() {
    std::filesystem::remove_all(std::filesystem::current_path().string() + "/test_log");
  }

  std::unique_ptr<PersistedLog> log;
  std::vector<protocol::log::LogEntry> entries;
  int entry_count;
};

class MetadataTest : public ::testing::Test {
protected:
  MetadataTest() {
    log = std::make_unique<PersistedLog>(
        std::filesystem::current_path().string() + "/test_log/");
  }

  ~MetadataTest() {
    std::filesystem::remove_all(std::filesystem::current_path().string() + "/test_log");
  }

  std::unique_ptr<PersistedLog> log;
  protocol::log::LogMetadata test_metadata;
};

class AppendTest : public LogTestBase {
protected:
  void SetUp(const int count, const int max_file_size = 1024*8) {
    LogTestBase::SetUp(count, max_file_size);
  }
};

class RestoreLogTest : public LogTestBase {
protected:
  void SetUp(const int count, const int max_file_size = 1024*8) {
    LogTestBase::SetUp(count, max_file_size);
    
    log.reset(new PersistedLog(
          std::filesystem::current_path().string() + "/test_log/",
          true,
          max_file_size));
  }
};

class TruncateTest : public LogTestBase {
protected:
  void SetUp(const int count, const int max_file_size = 1024*8) {
    LogTestBase::SetUp(count, max_file_size);
  }

  std::vector<std::string> PersistedPages() const {
    std::vector<std::string> file_list;
    std::string dir = std::filesystem::current_path().string() + "/test_log/";
    for (const auto& entry:std::filesystem::directory_iterator(dir)) {
      if (std::filesystem::is_regular_file(entry)) {
        file_list.push_back(entry.path().filename());
      }
    }
    std::sort(file_list.begin(), file_list.end());
    return file_list;
  }
};

TEST_F(MetadataTest, HandlesNoMetadata) {
  bool valid = log->Metadata(test_metadata);

  ASSERT_FALSE(valid);
}

TEST_F(MetadataTest, ValidateMetadataInMemory) {
  test_metadata.set_term(4);
  test_metadata.set_vote("peer1");
  log->SetMetadata(test_metadata);

  // Overwrite previous metadata with new term
  test_metadata.set_term(5);
  log->SetMetadata(test_metadata);

  protocol::log::LogMetadata result_metadata;
  bool valid = log->Metadata(result_metadata);

  ASSERT_TRUE(valid);
  EXPECT_EQ(test_metadata.term(), result_metadata.term());
  EXPECT_EQ(test_metadata.vote(), result_metadata.vote());
  EXPECT_EQ(2, result_metadata.version());
}

TEST_F(MetadataTest, ValidateMetadataOnDisk) {
  test_metadata.set_term(4);
  test_metadata.set_vote("peer1");
  log->SetMetadata(test_metadata);

  test_metadata.set_term(5);
  test_metadata.set_vote("peer2");
  log->SetMetadata(test_metadata);

  test_metadata.set_term(6);
  test_metadata.set_vote("peer3");
  log->SetMetadata(test_metadata);

  // Calling constructor causes data to be restored from disk
  log.reset(new PersistedLog(
        std::filesystem::current_path().string() + "/test_log/", true));
  protocol::log::LogMetadata result_metadata;
  bool valid = log->Metadata(result_metadata);

  ASSERT_TRUE(valid);
  EXPECT_EQ(test_metadata.term(), result_metadata.term());
  EXPECT_EQ(test_metadata.vote(), result_metadata.vote());
  EXPECT_EQ(3, result_metadata.version());
}

TEST_F(AppendTest, ValidateSingleFileEntries) {
  SetUp(3);

  // Verify that entries in persisted log map match initially appended entries
  ASSERT_EQ(log->LogSize(), entry_count);
  for (int i = 0; i < entry_count; i++) {
    EXPECT_EQ(log->Entry(i).term(), entries[i].term());
    EXPECT_EQ(log->Entry(i).data(), entries[i].data());
  }
  EXPECT_THROW(log->Entry(3), std::out_of_range);

  // Verify that data remains valid when querying a slice of log entries
  auto expected_slice = std::vector<protocol::log::LogEntry>(entries.begin() + 1, entries.begin() + 3);
  auto result_slice = log->Entries(1, 3);
  ASSERT_EQ(expected_slice.size(), result_slice.size());
  for (int i = 0; i < expected_slice.size(); i++) {
    EXPECT_EQ(result_slice[i].term(), expected_slice[i].term());
    EXPECT_EQ(result_slice[i].data(), expected_slice[i].data());
  }
}

TEST_F(AppendTest, ValidateMultipleFileEntries) {
  SetUp(8, 25);
  
  // Verify that entries in persisted log map match initially appended entries
  ASSERT_EQ(log->LogSize(), entry_count);
  for (int i = 0; i < entry_count; i++) {
    EXPECT_EQ(log->Entry(i).term(), entries[i].term());
    EXPECT_EQ(log->Entry(i).data(), entries[i].data());
  }
  EXPECT_THROW(log->Entry(8), std::out_of_range);

  // Verify that data remains valid when querying a slice of log entries
  auto expected_slice = std::vector<protocol::log::LogEntry>(entries.begin() + 2, entries.begin() + 7);
  auto result_slice = log->Entries(2, 7);
  ASSERT_EQ(expected_slice.size(), result_slice.size());
  for (int i = 0; i < expected_slice.size(); i++) {
    EXPECT_EQ(result_slice[i].term(), expected_slice[i].term());
    EXPECT_EQ(result_slice[i].data(), expected_slice[i].data());
  }
}

TEST_F(RestoreLogTest, HandlesSingleFilePersistence) {
  SetUp(3);

  // Verify that entries persisted to disk have not been altered since insertion
  ASSERT_EQ(log->LogSize(), entries.size());
  for (int i = 0; i < entry_count; i++) {
    EXPECT_EQ(log->Entry(i).term(), entries[i].term());
    EXPECT_EQ(log->Entry(i).data(), entries[i].data());
  }

  // Verify that data remains valid when querying a slice of log entries after restart
  auto expected_slice = std::vector<protocol::log::LogEntry>(entries.begin() + 1, entries.begin() + 3);
  auto result_slice = log->Entries(1, 3);
  ASSERT_EQ(expected_slice.size(), result_slice.size());
  for (int i = 0; i < expected_slice.size(); i++) {
    EXPECT_EQ(result_slice[i].term(), expected_slice[i].term());
    EXPECT_EQ(result_slice[i].data(), expected_slice[i].data());
  }
}

TEST_F(RestoreLogTest, HandlesMultipleFilePersistence) {
  SetUp(8, 25);

  // Verify that entries persisted to disk have not been altered since insertion
  ASSERT_EQ(log->LogSize(), entries.size());
  for (int i = 0; i < entry_count; i++) {
    EXPECT_EQ(log->Entry(i).term(), entries[i].term());
    EXPECT_EQ(log->Entry(i).data(), entries[i].data());
  }

  // Verify that data remains valid when querying a slice of log entries
  auto expected_slice = std::vector<protocol::log::LogEntry>(entries.begin() + 4, entries.begin() + 8);
  auto result_slice = log->Entries(4, 8);
  ASSERT_EQ(expected_slice.size(), result_slice.size());
  for (int i = 0; i < expected_slice.size(); i++) {
    EXPECT_EQ(result_slice[i].term(), expected_slice[i].term());
    EXPECT_EQ(result_slice[i].data(), expected_slice[i].data());
  }
}

TEST_F(TruncateTest, HandlesOpenPageDeletion) {
  SetUp(3);
  log->TruncateSuffix(0);

  // Verify that all entries have been deleted
  ASSERT_EQ(log->LogSize(), 0);

  // Verify that all pages on disk are deleted
  auto disk_pages = PersistedPages();
  ASSERT_EQ(disk_pages.size(), 0);
}


TEST_F(TruncateTest, HandlesOpenPageTruncation) {
  SetUp(3);
  log->TruncateSuffix(1);

  // Verify that entries have been altered after truncation
  ASSERT_EQ(log->LogSize(), 1);
  EXPECT_EQ(log->Entry(0).term(), entries[0].term());
  EXPECT_EQ(log->Entry(0).data(), entries[0].data());

  // Verify that page is closed and new open page is created
  auto disk_pages = PersistedPages();
  std::vector<std::string> expected_files = {
    "00000000000000000000-00000000000000000001",
    "open-1"
  };

  ASSERT_EQ(disk_pages.size(), 2);
  for (int i=0; i < 2; i++) {
    ASSERT_EQ(disk_pages[i], expected_files[i]);
  }
}

TEST_F(TruncateTest, HandlesClosedPageDeletion) {
  SetUp(8, 25);
  log->TruncateSuffix(3);

  // Verify that entries have been altered after truncation
  // Entries [3, 8] should be deleted
  ASSERT_EQ(log->LogSize(), 3);
  for (int i = 0; i < 3; i++) {
    EXPECT_EQ(log->Entry(i).term(), entries[i].term());
    EXPECT_EQ(log->Entry(i).data(), entries[i].data());
  }
  
  auto disk_pages = PersistedPages();
  std::string expected_file = "00000000000000000000-00000000000000000003";

  // The first closed page is the only one that remains since it contains entries [0, 2]
  ASSERT_EQ(disk_pages.size(), 1);
  EXPECT_EQ(disk_pages[0], expected_file);
}

TEST_F(TruncateTest, HandlesClosedPageTruncation) {
  SetUp(8, 25);
  log->TruncateSuffix(4);

  // Verify that entries have been altered after truncation
  // Entries [4, 8] should be deleted
  ASSERT_EQ(log->LogSize(), 4);
  for (int i = 0; i < 4; i++) {
    EXPECT_EQ(log->Entry(i).term(), entries[i].term());
    EXPECT_EQ(log->Entry(i).data(), entries[i].data());
  }

  auto disk_pages = PersistedPages();
  std::vector<std::string> expected_files = {
    "00000000000000000000-00000000000000000003",
    "00000000000000000003-00000000000000000004"
  };

  ASSERT_EQ(disk_pages.size(), 2);
  for (int i=0; i < 2; i++) {
    EXPECT_EQ(disk_pages[i], expected_files[i]);
  }
}

}

