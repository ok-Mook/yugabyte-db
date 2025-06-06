//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

#include "yb/rocksdb/db/db_test_util.h"

#include "yb/util/sync_point.h"

namespace rocksdb {

class EventListenerTest : public DBTestBase {
 public:
  EventListenerTest() : DBTestBase("/listener_test") {}

  const size_t k110KB = 110 << 10;
};

struct TestPropertiesCollector : public rocksdb::TablePropertiesCollector {
  virtual rocksdb::Status AddUserKey(const rocksdb::Slice& key,
                                     const rocksdb::Slice& value,
                                     rocksdb::EntryType type,
                                     rocksdb::SequenceNumber seq,
                                     uint64_t file_size) override {
    return Status::OK();
  }
  virtual rocksdb::Status Finish(
      rocksdb::UserCollectedProperties* properties) override {
    properties->insert({"0", "1"});
    return Status::OK();
  }

  const char* Name() const override {
    return "TestTablePropertiesCollector";
  }

  rocksdb::UserCollectedProperties GetReadableProperties() const override {
    rocksdb::UserCollectedProperties ret;
    ret["2"] = "3";
    return ret;
  }
};

class TestPropertiesCollectorFactory : public TablePropertiesCollectorFactory {
 public:
  virtual TablePropertiesCollector* CreateTablePropertiesCollector(
      TablePropertiesCollectorFactory::Context context) override {
    return new TestPropertiesCollector;
  }
  const char* Name() const override { return "TestTablePropertiesCollector"; }
};

class TestCompactionListener : public EventListener {
 public:
  void OnCompactionCompleted(DB *db, const CompactionJobInfo& ci) override {
    std::lock_guard lock(mutex_);
    compacted_dbs_.push_back(db);
    ASSERT_GT(ci.input_files.size(), 0U);
    ASSERT_GT(ci.output_files.size(), 0U);
    ASSERT_EQ(db->GetEnv()->GetThreadID(), ci.thread_id);
    ASSERT_GT(ci.thread_id, 0U);

    for (auto fl : {ci.input_files, ci.output_files}) {
      for (auto fn : fl) {
        auto it = ci.table_properties.find(fn);
        ASSERT_NE(it, ci.table_properties.end());
        auto tp = it->second;
        ASSERT_TRUE(tp != nullptr);
        ASSERT_EQ(tp->user_collected_properties.find("0")->second, "1");
      }
    }
  }

  std::vector<DB*> compacted_dbs_;
  std::mutex mutex_;
};

TEST_F(EventListenerTest, OnSingleDBCompactionTest) {
  const int kTestKeySize = 16;
  const int kTestValueSize = 984;
  const int kEntrySize = kTestKeySize + kTestValueSize;
  const int kEntriesPerBuffer = 100;
  const int kNumL0Files = 4;

  Options options;
  options.create_if_missing = true;
  options.write_buffer_size = kEntrySize * kEntriesPerBuffer;
  options.compaction_style = kCompactionStyleLevel;
  options.target_file_size_base = options.write_buffer_size;
  options.max_bytes_for_level_base = options.target_file_size_base * 2;
  options.max_bytes_for_level_multiplier = 2;
  options.compression = kNoCompression;
  options.level0_file_num_compaction_trigger = kNumL0Files;
  options.table_properties_collector_factories.push_back(
      std::make_shared<TestPropertiesCollectorFactory>());

  TestCompactionListener* listener = new TestCompactionListener();
  options.listeners.emplace_back(listener);
  std::vector<std::string> cf_names = {
      "pikachu", "ilya", "muromec", "dobrynia",
      "nikitich", "alyosha", "popovich"};
  CreateAndReopenWithCF(cf_names, options);
  ASSERT_OK(Put(1, "pikachu", std::string(90000, 'p')));
  ASSERT_OK(Put(2, "ilya", std::string(90000, 'i')));
  ASSERT_OK(Put(3, "muromec", std::string(90000, 'm')));
  ASSERT_OK(Put(4, "dobrynia", std::string(90000, 'd')));
  ASSERT_OK(Put(5, "nikitich", std::string(90000, 'n')));
  ASSERT_OK(Put(6, "alyosha", std::string(90000, 'a')));
  ASSERT_OK(Put(7, "popovich", std::string(90000, 'p')));
  for (int i = 1; i < 8; ++i) {
    ASSERT_OK(Flush(i));
    const Slice kRangeStart = "a";
    const Slice kRangeEnd = "z";
    ASSERT_OK(dbfull()->CompactRange(CompactRangeOptions(), handles_[i],
                                     &kRangeStart, &kRangeEnd));
    ASSERT_OK(dbfull()->TEST_WaitForFlushMemTable());
    ASSERT_OK(dbfull()->TEST_WaitForCompact());
  }

  ASSERT_EQ(listener->compacted_dbs_.size(), cf_names.size());
  for (size_t i = 0; i < cf_names.size(); ++i) {
    ASSERT_EQ(listener->compacted_dbs_[i], db_);
  }
}

// This simple Listener can only handle one flush at a time.
class TestFlushListener : public EventListener {
 public:
  explicit TestFlushListener(Env* env)
      : slowdown_count(0), stop_count(0), db_closed(), env_(env) {
    db_closed = false;
  }
  void OnTableFileCreated(
      const TableFileCreationInfo& info) override {
    // remember the info for later checking the FlushJobInfo.
    prev_fc_info_ = info;
    ASSERT_GT(info.db_name.size(), 0U);
    ASSERT_GT(info.cf_name.size(), 0U);
    ASSERT_GT(info.file_path.size(), 0U);
    ASSERT_GT(info.job_id, 0);
    ASSERT_GT(info.table_properties.data_size, 0U);
    ASSERT_GT(info.table_properties.raw_key_size, 0U);
    ASSERT_GT(info.table_properties.raw_value_size, 0U);
    ASSERT_GT(info.table_properties.num_data_blocks, 0U);
    ASSERT_GT(info.table_properties.num_entries, 0U);
  }

  void OnFlushCompleted(
      DB* db, const FlushJobInfo& info) override {
    flushed_dbs_.push_back(db);
    flushed_column_family_names_.push_back(info.cf_name);
    if (info.triggered_writes_slowdown) {
      slowdown_count++;
    }
    if (info.triggered_writes_stop) {
      stop_count++;
    }
    // verify whether the previously created file matches the flushed file.
    ASSERT_EQ(prev_fc_info_.db_name, db->GetName());
    ASSERT_EQ(prev_fc_info_.cf_name, info.cf_name);
    ASSERT_EQ(prev_fc_info_.job_id, info.job_id);
    ASSERT_EQ(prev_fc_info_.file_path, info.file_path);
    ASSERT_EQ(db->GetEnv()->GetThreadID(), info.thread_id);
    ASSERT_GT(info.thread_id, 0U);
    ASSERT_EQ(info.table_properties.user_collected_properties.find("0")->second,
              "1");
  }

  std::vector<std::string> flushed_column_family_names_;
  std::vector<DB*> flushed_dbs_;
  int slowdown_count;
  int stop_count;
  bool db_closing;
  std::atomic_bool db_closed;
  TableFileCreationInfo prev_fc_info_;

 protected:
  Env* env_;
};

TEST_F(EventListenerTest, OnSingleDBFlushTest) {
  Options options;
  options.write_buffer_size = k110KB;
  TestFlushListener* listener = new TestFlushListener(options.env);
  options.listeners.emplace_back(listener);
  std::vector<std::string> cf_names = {
      "pikachu", "ilya", "muromec", "dobrynia",
      "nikitich", "alyosha", "popovich"};
  options.table_properties_collector_factories.push_back(
      std::make_shared<TestPropertiesCollectorFactory>());
  CreateAndReopenWithCF(cf_names, options);

  ASSERT_OK(Put(1, "pikachu", std::string(90000, 'p')));
  ASSERT_OK(Put(2, "ilya", std::string(90000, 'i')));
  ASSERT_OK(Put(3, "muromec", std::string(90000, 'm')));
  ASSERT_OK(Put(4, "dobrynia", std::string(90000, 'd')));
  ASSERT_OK(Put(5, "nikitich", std::string(90000, 'n')));
  ASSERT_OK(Put(6, "alyosha", std::string(90000, 'a')));
  ASSERT_OK(Put(7, "popovich", std::string(90000, 'p')));
  for (int i = 1; i < 8; ++i) {
    ASSERT_OK(Flush(i));
    ASSERT_OK(dbfull()->TEST_WaitForFlushMemTable());
    ASSERT_EQ(listener->flushed_dbs_.size(), i);
    ASSERT_EQ(listener->flushed_column_family_names_.size(), i);
  }

  // make sure call-back functions are called in the right order
  for (size_t i = 0; i < cf_names.size(); ++i) {
    ASSERT_EQ(listener->flushed_dbs_[i], db_);
    ASSERT_EQ(listener->flushed_column_family_names_[i], cf_names[i]);
  }
}

TEST_F(EventListenerTest, MultiCF) {
  Options options;
  options.write_buffer_size = k110KB;
  TestFlushListener* listener = new TestFlushListener(options.env);
  options.listeners.emplace_back(listener);
  options.table_properties_collector_factories.push_back(
      std::make_shared<TestPropertiesCollectorFactory>());
  std::vector<std::string> cf_names = {
      "pikachu", "ilya", "muromec", "dobrynia",
      "nikitich", "alyosha", "popovich"};
  CreateAndReopenWithCF(cf_names, options);

  ASSERT_OK(Put(1, "pikachu", std::string(90000, 'p')));
  ASSERT_OK(Put(2, "ilya", std::string(90000, 'i')));
  ASSERT_OK(Put(3, "muromec", std::string(90000, 'm')));
  ASSERT_OK(Put(4, "dobrynia", std::string(90000, 'd')));
  ASSERT_OK(Put(5, "nikitich", std::string(90000, 'n')));
  ASSERT_OK(Put(6, "alyosha", std::string(90000, 'a')));
  ASSERT_OK(Put(7, "popovich", std::string(90000, 'p')));
  for (int i = 1; i < 8; ++i) {
    ASSERT_OK(Flush(i));
    ASSERT_EQ(listener->flushed_dbs_.size(), i);
    ASSERT_EQ(listener->flushed_column_family_names_.size(), i);
  }

  // make sure call-back functions are called in the right order
  for (size_t i = 0; i < cf_names.size(); i++) {
    ASSERT_EQ(listener->flushed_dbs_[i], db_);
    ASSERT_EQ(listener->flushed_column_family_names_[i], cf_names[i]);
  }
}

TEST_F(EventListenerTest, MultiDBMultiListeners) {
  Options options;
  options.table_properties_collector_factories.push_back(
      std::make_shared<TestPropertiesCollectorFactory>());
  std::vector<TestFlushListener*> listeners;
  const int kNumDBs = 5;
  const int kNumListeners = 10;
  for (int i = 0; i < kNumListeners; ++i) {
    listeners.emplace_back(new TestFlushListener(options.env));
  }

  std::vector<std::string> cf_names = {
      "pikachu", "ilya", "muromec", "dobrynia",
      "nikitich", "alyosha", "popovich"};

  options.create_if_missing = true;
  for (int i = 0; i < kNumListeners; ++i) {
    options.listeners.emplace_back(listeners[i]);
  }
  DBOptions db_opts(options);
  ColumnFamilyOptions cf_opts(options);

  std::vector<DB*> dbs;
  std::vector<std::vector<ColumnFamilyHandle *>> vec_handles;

  for (int d = 0; d < kNumDBs; ++d) {
    ASSERT_OK(DestroyDB(dbname_ + ToString(d), options));
    DB* db;
    std::vector<ColumnFamilyHandle*> handles;
    ASSERT_OK(DB::Open(options, dbname_ + ToString(d), &db));
    for (size_t c = 0; c < cf_names.size(); ++c) {
      ColumnFamilyHandle* handle;
      ASSERT_OK(db->CreateColumnFamily(cf_opts, cf_names[c], &handle));
      handles.push_back(handle);
    }

    vec_handles.push_back(std::move(handles));
    dbs.push_back(db);
  }

  for (int d = 0; d < kNumDBs; ++d) {
    for (size_t c = 0; c < cf_names.size(); ++c) {
      ASSERT_OK(dbs[d]->Put(WriteOptions(), vec_handles[d][c],
                cf_names[c], cf_names[c]));
    }
  }

  for (size_t c = 0; c < cf_names.size(); ++c) {
    for (int d = 0; d < kNumDBs; ++d) {
      ASSERT_OK(dbs[d]->Flush(FlushOptions(), vec_handles[d][c]));
      ASSERT_OK(reinterpret_cast<DBImpl*>(dbs[d])->TEST_WaitForFlushMemTable());
    }
  }

  for (auto* listener : listeners) {
    int pos = 0;
    for (size_t c = 0; c < cf_names.size(); ++c) {
      for (int d = 0; d < kNumDBs; ++d) {
        ASSERT_EQ(listener->flushed_dbs_[pos], dbs[d]);
        ASSERT_EQ(listener->flushed_column_family_names_[pos], cf_names[c]);
        pos++;
      }
    }
  }


  for (auto handles : vec_handles) {
    for (auto h : handles) {
      delete h;
    }
    handles.clear();
  }
  vec_handles.clear();

  for (auto db : dbs) {
    delete db;
  }
}

TEST_F(EventListenerTest, DisableBGCompaction) {
  Options options;
  TestFlushListener* listener = new TestFlushListener(options.env);
  const int kCompactionTrigger = 1;
  const int kSlowdownTrigger = 5;
  const int kStopTrigger = 100;
  options.level0_file_num_compaction_trigger = kCompactionTrigger;
  options.level0_slowdown_writes_trigger = kSlowdownTrigger;
  options.level0_stop_writes_trigger = kStopTrigger;
  options.max_write_buffer_number = 10;
  options.listeners.emplace_back(listener);
  // BG compaction is disabled.  Number of L0 files will simply keeps
  // increasing in this test.
  options.compaction_style = kCompactionStyleNone;
  options.compression = kNoCompression;
  options.write_buffer_size = 100000;  // Small write buffer
  options.table_properties_collector_factories.push_back(
      std::make_shared<TestPropertiesCollectorFactory>());

  CreateAndReopenWithCF({"pikachu"}, options);
  ColumnFamilyMetaData cf_meta;
  db_->GetColumnFamilyMetaData(handles_[1], &cf_meta);

  // keep writing until writes are forced to stop.
  for (int i = 0; static_cast<int>(cf_meta.file_count) < kSlowdownTrigger * 10;
       ++i) {
    ASSERT_OK(Put(1, ToString(i), std::string(10000, 'x'), WriteOptions()));
    ASSERT_OK(db_->Flush(FlushOptions(), handles_[1]));
    db_->GetColumnFamilyMetaData(handles_[1], &cf_meta);
  }
  ASSERT_GE(listener->slowdown_count, kSlowdownTrigger * 9);
}

class TestCompactionReasonListener : public EventListener {
 public:
  void OnCompactionCompleted(DB* db, const CompactionJobInfo& ci) override {
    std::lock_guard lock(mutex_);
    compaction_reasons_.push_back(ci.compaction_reason);
  }

  std::vector<CompactionReason> compaction_reasons_;
  std::mutex mutex_;
};

TEST_F(EventListenerTest, CompactionReasonLevel) {
  Options options;
  options.create_if_missing = true;
  options.memtable_factory.reset(
      new SpecialSkipListFactory(DBTestBase::kNumKeysByGenerateNewRandomFile));

  TestCompactionReasonListener* listener = new TestCompactionReasonListener();
  options.listeners.emplace_back(listener);

  options.level0_file_num_compaction_trigger = 4;
  options.compaction_style = kCompactionStyleLevel;

  DestroyAndReopen(options);
  Random rnd(301);

  // Write 4 files in L0
  for (int i = 0; i < 4; i++) {
    GenerateNewRandomFile(&rnd);
  }
  ASSERT_OK(dbfull()->TEST_WaitForCompact());

  ASSERT_EQ(listener->compaction_reasons_.size(), 1);
  ASSERT_EQ(listener->compaction_reasons_[0],
            CompactionReason::kLevelL0FilesNum);

  DestroyAndReopen(options);

  // Write 3 non-overlapping files in L0
  for (int k = 1; k <= 30; k++) {
    ASSERT_OK(Put(Key(k), Key(k)));
    if (k % 10 == 0) {
      ASSERT_OK(Flush());
    }
  }

  // Do a trivial move from L0 -> L1
  ASSERT_OK(db_->CompactRange(CompactRangeOptions(), nullptr, nullptr));

  options.max_bytes_for_level_base = 1;
  Close();
  listener->compaction_reasons_.clear();
  Reopen(options);

  ASSERT_OK(dbfull()->TEST_WaitForCompact());
  ASSERT_GT(listener->compaction_reasons_.size(), 1);

  for (auto compaction_reason : listener->compaction_reasons_) {
    ASSERT_EQ(compaction_reason, CompactionReason::kLevelMaxLevelSize);
  }

  options.disable_auto_compactions = true;
  Close();
  listener->compaction_reasons_.clear();
  Reopen(options);

  ASSERT_OK(db_->CompactRange(CompactRangeOptions(), nullptr, nullptr));
  ASSERT_GT(listener->compaction_reasons_.size(), 0);
  for (auto compaction_reason : listener->compaction_reasons_) {
    ASSERT_EQ(compaction_reason, CompactionReason::kManualCompaction);
  }
}

TEST_F(EventListenerTest, CompactionReasonUniversal) {
  Options options;
  options.create_if_missing = true;
  options.memtable_factory.reset(
      new SpecialSkipListFactory(DBTestBase::kNumKeysByGenerateNewRandomFile));

  TestCompactionReasonListener* listener = new TestCompactionReasonListener();
  options.listeners.emplace_back(listener);

  options.compaction_style = kCompactionStyleUniversal;

  Random rnd(301);

  options.level0_file_num_compaction_trigger = 8;
  options.compaction_options_universal.max_size_amplification_percent = 100000;
  options.compaction_options_universal.size_ratio = 100000;
  DestroyAndReopen(options);
  listener->compaction_reasons_.clear();

  // Write 8 files in L0
  for (int i = 0; i < 8; i++) {
    GenerateNewRandomFile(&rnd);
  }
  ASSERT_OK(dbfull()->TEST_WaitForCompact());

  ASSERT_GT(listener->compaction_reasons_.size(), 0);
  for (auto compaction_reason : listener->compaction_reasons_) {
    ASSERT_EQ(compaction_reason, CompactionReason::kUniversalSizeRatio);
  }

  options.level0_file_num_compaction_trigger = 8;
  options.compaction_options_universal.max_size_amplification_percent = 1;
  options.compaction_options_universal.size_ratio = 100000;

  DestroyAndReopen(options);
  listener->compaction_reasons_.clear();

  // Write 8 files in L0
  for (int i = 0; i < 8; i++) {
    GenerateNewRandomFile(&rnd);
  }
  ASSERT_OK(dbfull()->TEST_WaitForCompact());

  ASSERT_GT(listener->compaction_reasons_.size(), 0);
  for (auto compaction_reason : listener->compaction_reasons_) {
    ASSERT_EQ(compaction_reason, CompactionReason::kUniversalSizeAmplification);
  }

  options.disable_auto_compactions = true;
  Close();
  listener->compaction_reasons_.clear();
  Reopen(options);

  ASSERT_OK(db_->CompactRange(CompactRangeOptions(), nullptr, nullptr));

  ASSERT_GT(listener->compaction_reasons_.size(), 0);
  for (auto compaction_reason : listener->compaction_reasons_) {
    ASSERT_EQ(compaction_reason, CompactionReason::kManualCompaction);
  }
}

TEST_F(EventListenerTest, CompactionReasonFIFO) {
  Options options;
  options.create_if_missing = true;
  options.memtable_factory.reset(
      new SpecialSkipListFactory(DBTestBase::kNumKeysByGenerateNewRandomFile));

  TestCompactionReasonListener* listener = new TestCompactionReasonListener();
  options.listeners.emplace_back(listener);

  options.level0_file_num_compaction_trigger = 4;
  options.compaction_style = kCompactionStyleFIFO;
  options.compaction_options_fifo.max_table_files_size = 1;

  DestroyAndReopen(options);
  Random rnd(301);

  // Write 4 files in L0
  for (int i = 0; i < 4; i++) {
    GenerateNewRandomFile(&rnd);
  }
  ASSERT_OK(dbfull()->TEST_WaitForCompact());

  ASSERT_GT(listener->compaction_reasons_.size(), 0);
  for (auto compaction_reason : listener->compaction_reasons_) {
    ASSERT_EQ(compaction_reason, CompactionReason::kFIFOMaxSize);
  }
}
}  // namespace rocksdb


int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
