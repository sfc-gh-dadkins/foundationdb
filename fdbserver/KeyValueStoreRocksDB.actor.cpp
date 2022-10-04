/*
 * KeyValueStoreRocksDB.actor.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2022 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifdef SSD_ROCKSDB_EXPERIMENTAL

#include <rocksdb/c.h>
#include <rocksdb/cache.h>
#include <rocksdb/db.h>
#include <rocksdb/env.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/listener.h>
#include <rocksdb/metadata.h>
#include <rocksdb/options.h>
#include <rocksdb/perf_context.h>
#include <rocksdb/rate_limiter.h>
#include <rocksdb/slice.h>
#include <rocksdb/slice_transform.h>
#include <rocksdb/sst_file_reader.h>
#include <rocksdb/sst_file_writer.h>
#include <rocksdb/statistics.h>
#include <rocksdb/table.h>
#include <rocksdb/types.h>
#include <rocksdb/utilities/checkpoint.h>
#include <rocksdb/utilities/table_properties_collectors.h>
#include <rocksdb/version.h>

#if defined __has_include
#if __has_include(<liburing.h>)
#include <liburing.h>
#endif
#endif
#include "fdbclient/SystemData.h"
#include "fdbserver/CoroFlow.h"
#include "flow/ActorCollection.h"
#include "flow/flow.h"
#include "flow/IThreadPool.h"
#include "flow/ThreadHelper.actor.h"
#include "flow/Histogram.h"

#include <memory>
#include <tuple>
#include <vector>

#endif // SSD_ROCKSDB_EXPERIMENTAL

#include "fdbserver/IKeyValueStore.h"
#include "fdbserver/RocksDBCheckpointUtils.actor.h"

#include "flow/actorcompiler.h" // has to be last include

#ifdef SSD_ROCKSDB_EXPERIMENTAL

// Enforcing rocksdb version to be 6.27.3 or greater.
static_assert(ROCKSDB_MAJOR >= 6, "Unsupported rocksdb version. Update the rocksdb to 6.27.3 version");
static_assert(ROCKSDB_MAJOR == 6 ? ROCKSDB_MINOR >= 27 : true,
              "Unsupported rocksdb version. Update the rocksdb to 6.27.3 version");
static_assert((ROCKSDB_MAJOR == 6 && ROCKSDB_MINOR == 27) ? ROCKSDB_PATCH >= 3 : true,
              "Unsupported rocksdb version. Update the rocksdb to 6.27.3 version");

namespace {
using rocksdb::BackgroundErrorReason;

class SharedRocksDBState {
public:
	SharedRocksDBState();

	void setClosing() { this->closing = true; }
	bool isClosing() const { return this->closing; }

	rocksdb::DBOptions getDbOptions() const { return this->dbOptions; }
	rocksdb::ColumnFamilyOptions getCfOptions() const { return this->cfOptions; }
	rocksdb::Options getOptions() const { return rocksdb::Options(this->dbOptions, this->cfOptions); }
	rocksdb::ReadOptions& getReadOptions() { return this->readOptions; }

private:
	rocksdb::ColumnFamilyOptions initialCfOptions();
	rocksdb::DBOptions initialDbOptions();
	rocksdb::ReadOptions initialReadOptions();

	bool closing;
	rocksdb::DBOptions dbOptions;
	rocksdb::ColumnFamilyOptions cfOptions;
	rocksdb::ReadOptions readOptions;
};

SharedRocksDBState::SharedRocksDBState()
  : closing(false), dbOptions(initialDbOptions()), cfOptions(initialCfOptions()), readOptions(initialReadOptions()) {}

rocksdb::ColumnFamilyOptions SharedRocksDBState::initialCfOptions() {
	rocksdb::ColumnFamilyOptions options;
	options.level_compaction_dynamic_level_bytes = true;
	options.OptimizeLevelStyleCompaction(SERVER_KNOBS->ROCKSDB_MEMTABLE_BYTES);

	if (SERVER_KNOBS->ROCKSDB_PERIODIC_COMPACTION_SECONDS > 0) {
		options.periodic_compaction_seconds = SERVER_KNOBS->ROCKSDB_PERIODIC_COMPACTION_SECONDS;
	}
	if (SERVER_KNOBS->ROCKSDB_SOFT_PENDING_COMPACT_BYTES_LIMIT > 0) {
		options.soft_pending_compaction_bytes_limit = SERVER_KNOBS->ROCKSDB_SOFT_PENDING_COMPACT_BYTES_LIMIT;
	}
	if (SERVER_KNOBS->ROCKSDB_HARD_PENDING_COMPACT_BYTES_LIMIT > 0) {
		options.hard_pending_compaction_bytes_limit = SERVER_KNOBS->ROCKSDB_HARD_PENDING_COMPACT_BYTES_LIMIT;
	}

	// Compact sstables when there's too much deleted stuff.
	options.table_properties_collector_factories = { rocksdb::NewCompactOnDeletionCollectorFactory(128, 1) };

	rocksdb::BlockBasedTableOptions bbOpts;
	if (SERVER_KNOBS->ROCKSDB_PREFIX_LEN > 0) {
		// Prefix blooms are used during Seek.
		options.prefix_extractor.reset(rocksdb::NewFixedPrefixTransform(SERVER_KNOBS->ROCKSDB_PREFIX_LEN));

		// Also turn on bloom filters in the memtable.
		// TODO: Make a knob for this as well.
		options.memtable_prefix_bloom_size_ratio = 0.1;

		// 5 -- Can be read by RocksDB's versions since 6.6.0. Full and partitioned
		// filters use a generally faster and more accurate Bloom filter
		// implementation, with a different schema.
		// https://github.com/facebook/rocksdb/blob/b77569f18bfc77fb1d8a0b3218f6ecf571bc4988/include/rocksdb/table.h#L391
		bbOpts.format_version = 5;

		// Create and apply a bloom filter using the 10 bits
		// which should yield a ~1% false positive rate:
		// https://github.com/facebook/rocksdb/wiki/RocksDB-Bloom-Filter#full-filters-new-format
		bbOpts.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10));

		// The whole key blooms are only used for point lookups.
		// https://github.com/facebook/rocksdb/wiki/RocksDB-Bloom-Filter#prefix-vs-whole-key
		bbOpts.whole_key_filtering = false;
	}

	if (SERVER_KNOBS->ROCKSDB_BLOCK_CACHE_SIZE > 0) {
		bbOpts.block_cache = rocksdb::NewLRUCache(SERVER_KNOBS->ROCKSDB_BLOCK_CACHE_SIZE);
	}

	if (SERVER_KNOBS->ROCKSDB_BLOCK_SIZE > 0) {
		bbOpts.block_size = SERVER_KNOBS->ROCKSDB_BLOCK_SIZE;
	}

	options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(bbOpts));

	return options;
}

rocksdb::DBOptions SharedRocksDBState::initialDbOptions() {
	rocksdb::DBOptions options;
	options.avoid_unnecessary_blocking_io = true;
	options.create_if_missing = true;
	if (SERVER_KNOBS->ROCKSDB_BACKGROUND_PARALLELISM > 0) {
		options.IncreaseParallelism(SERVER_KNOBS->ROCKSDB_BACKGROUND_PARALLELISM);
	}
	if (SERVER_KNOBS->ROCKSDB_MAX_SUBCOMPACTIONS > 0) {
		options.max_subcompactions = SERVER_KNOBS->ROCKSDB_MAX_SUBCOMPACTIONS;
	}
	if (SERVER_KNOBS->ROCKSDB_COMPACTION_READAHEAD_SIZE > 0) {
		options.compaction_readahead_size = SERVER_KNOBS->ROCKSDB_COMPACTION_READAHEAD_SIZE;
	}

	options.statistics = rocksdb::CreateDBStatistics();
	options.statistics->set_stats_level(rocksdb::kExceptHistogramOrTimers);

	options.db_log_dir = SERVER_KNOBS->LOG_DIRECTORY;
	return options;
}

rocksdb::ReadOptions SharedRocksDBState::initialReadOptions() {
	rocksdb::ReadOptions options;
	options.background_purge_on_iterator_cleanup = true;
	return options;
}

// Returns string representation of RocksDB background error reason.
// Error reason code:
// https://github.com/facebook/rocksdb/blob/12d798ac06bcce36be703b057d5f5f4dab3b270c/include/rocksdb/listener.h#L125
// This function needs to be updated when error code changes.
std::string getErrorReason(BackgroundErrorReason reason) {
	switch (reason) {
	case BackgroundErrorReason::kFlush:
		return format("%d Flush", reason);
	case BackgroundErrorReason::kCompaction:
		return format("%d Compaction", reason);
	case BackgroundErrorReason::kWriteCallback:
		return format("%d WriteCallback", reason);
	case BackgroundErrorReason::kMemTable:
		return format("%d MemTable", reason);
	case BackgroundErrorReason::kManifestWrite:
		return format("%d ManifestWrite", reason);
	case BackgroundErrorReason::kFlushNoWAL:
		return format("%d FlushNoWAL", reason);
	case BackgroundErrorReason::kManifestWriteNoWAL:
		return format("%d ManifestWriteNoWAL", reason);
	default:
		return format("%d Unknown", reason);
	}
}
// Background error handling is tested with Chaos test.
// TODO: Test background error in simulation. RocksDB doesn't use flow IO in simulation, which limits our ability to
// inject IO errors. We could implement rocksdb::FileSystem using flow IO to unblock simulation. Also, trace event is
// not available on background threads because trace event requires setting up special thread locals. Using trace event
// could potentially cause segmentation fault.
class RocksDBErrorListener : public rocksdb::EventListener {
public:
	RocksDBErrorListener(UID id) : id(id){};
	void OnBackgroundError(rocksdb::BackgroundErrorReason reason, rocksdb::Status* bg_error) override {
		TraceEvent(SevError, "RocksDBBGError", id)
		    .detail("Reason", getErrorReason(reason))
		    .detail("RocksDBSeverity", bg_error->severity())
		    .detail("Status", bg_error->ToString());
		std::unique_lock<std::mutex> lock(mutex);
		if (!errorPromise.isValid())
			return;
		// RocksDB generates two types of background errors, IO Error and Corruption
		// Error type and severity map could be found at
		// https://github.com/facebook/rocksdb/blob/2e09a54c4fb82e88bcaa3e7cfa8ccbbbbf3635d5/db/error_handler.cc#L138.
		// All background errors will be treated as storage engine failure. Send the error to storage server.
		if (bg_error->IsIOError()) {
			errorPromise.sendError(io_error());
		} else if (bg_error->IsCorruption()) {
			errorPromise.sendError(file_corrupt());
		} else {
			errorPromise.sendError(unknown_error());
		}
	}
	Future<Void> getFuture() {
		std::unique_lock<std::mutex> lock(mutex);
		return errorPromise.getFuture();
	}
	~RocksDBErrorListener() {
		std::unique_lock<std::mutex> lock(mutex);
		if (!errorPromise.isValid())
			return;
		errorPromise.send(Never());
	}

private:
	ThreadReturnPromise<Void> errorPromise;
	std::mutex mutex;
	UID id;
};
using DB = rocksdb::DB*;
using CF = rocksdb::ColumnFamilyHandle*;

#define PERSIST_PREFIX "\xff\xff"
const KeyRef persistVersion = LiteralStringRef(PERSIST_PREFIX "Version");
const StringRef ROCKSDBSTORAGE_HISTOGRAM_GROUP = "RocksDBStorage"_sr;
const StringRef ROCKSDB_COMMIT_LATENCY_HISTOGRAM = "RocksDBCommitLatency"_sr;
const StringRef ROCKSDB_COMMIT_ACTION_HISTOGRAM = "RocksDBCommitAction"_sr;
const StringRef ROCKSDB_COMMIT_QUEUEWAIT_HISTOGRAM = "RocksDBCommitQueueWait"_sr;
const StringRef ROCKSDB_WRITE_HISTOGRAM = "RocksDBWrite"_sr;
const StringRef ROCKSDB_DELETE_COMPACTRANGE_HISTOGRAM = "RocksDBDeleteCompactRange"_sr;
const StringRef ROCKSDB_READRANGE_LATENCY_HISTOGRAM = "RocksDBReadRangeLatency"_sr;
const StringRef ROCKSDB_READVALUE_LATENCY_HISTOGRAM = "RocksDBReadValueLatency"_sr;
const StringRef ROCKSDB_READPREFIX_LATENCY_HISTOGRAM = "RocksDBReadPrefixLatency"_sr;
const StringRef ROCKSDB_READRANGE_ACTION_HISTOGRAM = "RocksDBReadRangeAction"_sr;
const StringRef ROCKSDB_READVALUE_ACTION_HISTOGRAM = "RocksDBReadValueAction"_sr;
const StringRef ROCKSDB_READPREFIX_ACTION_HISTOGRAM = "RocksDBReadPrefixAction"_sr;
const StringRef ROCKSDB_READRANGE_QUEUEWAIT_HISTOGRAM = "RocksDBReadRangeQueueWait"_sr;
const StringRef ROCKSDB_READVALUE_QUEUEWAIT_HISTOGRAM = "RocksDBReadValueQueueWait"_sr;
const StringRef ROCKSDB_READPREFIX_QUEUEWAIT_HISTOGRAM = "RocksDBReadPrefixQueueWait"_sr;
const StringRef ROCKSDB_READRANGE_NEWITERATOR_HISTOGRAM = "RocksDBReadRangeNewIterator"_sr;
const StringRef ROCKSDB_READVALUE_GET_HISTOGRAM = "RocksDBReadValueGet"_sr;
const StringRef ROCKSDB_READPREFIX_GET_HISTOGRAM = "RocksDBReadPrefixGet"_sr;

rocksdb::ExportImportFilesMetaData getMetaData(const CheckpointMetaData& checkpoint) {
	rocksdb::ExportImportFilesMetaData metaData;
	if (checkpoint.getFormat() != RocksDBColumnFamily) {
		return metaData;
	}

	RocksDBColumnFamilyCheckpoint rocksCF = getRocksCF(checkpoint);
	metaData.db_comparator_name = rocksCF.dbComparatorName;

	for (const LiveFileMetaData& fileMetaData : rocksCF.sstFiles) {
		rocksdb::LiveFileMetaData liveFileMetaData;
		liveFileMetaData.size = fileMetaData.size;
		liveFileMetaData.name = fileMetaData.name;
		liveFileMetaData.file_number = fileMetaData.file_number;
		liveFileMetaData.db_path = fileMetaData.db_path;
		liveFileMetaData.smallest_seqno = fileMetaData.smallest_seqno;
		liveFileMetaData.largest_seqno = fileMetaData.largest_seqno;
		liveFileMetaData.smallestkey = fileMetaData.smallestkey;
		liveFileMetaData.largestkey = fileMetaData.largestkey;
		liveFileMetaData.num_reads_sampled = fileMetaData.num_reads_sampled;
		liveFileMetaData.being_compacted = fileMetaData.being_compacted;
		liveFileMetaData.num_entries = fileMetaData.num_entries;
		liveFileMetaData.num_deletions = fileMetaData.num_deletions;
		liveFileMetaData.temperature = static_cast<rocksdb::Temperature>(fileMetaData.temperature);
		liveFileMetaData.oldest_blob_file_number = fileMetaData.oldest_blob_file_number;
		liveFileMetaData.oldest_ancester_time = fileMetaData.oldest_ancester_time;
		liveFileMetaData.file_creation_time = fileMetaData.file_creation_time;
		liveFileMetaData.file_checksum = fileMetaData.file_checksum;
		liveFileMetaData.file_checksum_func_name = fileMetaData.file_checksum_func_name;
		liveFileMetaData.column_family_name = fileMetaData.column_family_name;
		liveFileMetaData.level = fileMetaData.level;
		metaData.files.push_back(liveFileMetaData);
	}

	return metaData;
}

void populateMetaData(CheckpointMetaData* checkpoint, const rocksdb::ExportImportFilesMetaData& metaData) {
	RocksDBColumnFamilyCheckpoint rocksCF;
	rocksCF.dbComparatorName = metaData.db_comparator_name;
	for (const rocksdb::LiveFileMetaData& fileMetaData : metaData.files) {
		LiveFileMetaData liveFileMetaData;
		liveFileMetaData.size = fileMetaData.size;
		liveFileMetaData.name = fileMetaData.name;
		liveFileMetaData.file_number = fileMetaData.file_number;
		liveFileMetaData.db_path = fileMetaData.db_path;
		liveFileMetaData.smallest_seqno = fileMetaData.smallest_seqno;
		liveFileMetaData.largest_seqno = fileMetaData.largest_seqno;
		liveFileMetaData.smallestkey = fileMetaData.smallestkey;
		liveFileMetaData.largestkey = fileMetaData.largestkey;
		liveFileMetaData.num_reads_sampled = fileMetaData.num_reads_sampled;
		liveFileMetaData.being_compacted = fileMetaData.being_compacted;
		liveFileMetaData.num_entries = fileMetaData.num_entries;
		liveFileMetaData.num_deletions = fileMetaData.num_deletions;
		liveFileMetaData.temperature = static_cast<uint8_t>(fileMetaData.temperature);
		liveFileMetaData.oldest_blob_file_number = fileMetaData.oldest_blob_file_number;
		liveFileMetaData.oldest_ancester_time = fileMetaData.oldest_ancester_time;
		liveFileMetaData.file_creation_time = fileMetaData.file_creation_time;
		liveFileMetaData.file_checksum = fileMetaData.file_checksum;
		liveFileMetaData.file_checksum_func_name = fileMetaData.file_checksum_func_name;
		liveFileMetaData.column_family_name = fileMetaData.column_family_name;
		liveFileMetaData.level = fileMetaData.level;
		rocksCF.sstFiles.push_back(liveFileMetaData);
	}
	checkpoint->setFormat(RocksDBColumnFamily);
	checkpoint->serializedCheckpoint = ObjectWriter::toValue(rocksCF, IncludeVersion());
}

rocksdb::Slice toSlice(StringRef s) {
	return rocksdb::Slice(reinterpret_cast<const char*>(s.begin()), s.size());
}

StringRef toStringRef(rocksdb::Slice s) {
	return StringRef(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

struct Counters {
	CounterCollection cc;
	Counter immediateThrottle;
	Counter failedToAcquire;

	Counters()
	  : cc("RocksDBThrottle"), immediateThrottle("ImmediateThrottle", cc), failedToAcquire("FailedToAcquire", cc) {}
};

struct ReadIterator {
	CF& cf;
	uint64_t index; // incrementing counter to uniquely identify read iterator.
	bool inUse;
	std::shared_ptr<rocksdb::Iterator> iter;
	double creationTime;
	ReadIterator(CF& cf, uint64_t index, DB& db, rocksdb::ReadOptions& options)
	  : cf(cf), index(index), inUse(true), creationTime(now()), iter(db->NewIterator(options, cf)) {}
};

/*
ReadIteratorPool: Collection of iterators. Reuses iterators on non-concurrent multiple read operations,
instead of creating and deleting for every read.

Read: IteratorPool provides an unused iterator if exists or creates and gives a new iterator.
Returns back the iterator after the read is done.

Write: Iterators in the pool are deleted, forcing new iterator creation on next reads. The iterators
which are currently used by the reads can continue using the iterator as it is a shared_ptr. Once
the read is processed, shared_ptr goes out of scope and gets deleted. Eventually the iterator object
gets deleted as the ref count becomes 0.
*/
class ReadIteratorPool {
public:
	ReadIteratorPool(UID id, DB& db, CF& cf, const rocksdb::ReadOptions readOptions)
	  : db(db), cf(cf), index(0), iteratorsReuseCount(0), readRangeOptions(readOptions) {
		readRangeOptions.background_purge_on_iterator_cleanup = true;
		readRangeOptions.auto_prefix_mode = (SERVER_KNOBS->ROCKSDB_PREFIX_LEN > 0);
		TraceEvent("ReadIteratorPool", id)
		    .detail("KnobRocksDBReadRangeReuseIterators", SERVER_KNOBS->ROCKSDB_READ_RANGE_REUSE_ITERATORS)
		    .detail("KnobRocksDBPrefixLen", SERVER_KNOBS->ROCKSDB_PREFIX_LEN);
	}

	// Called on every db commit.
	void update() {
		if (SERVER_KNOBS->ROCKSDB_READ_RANGE_REUSE_ITERATORS) {
			std::lock_guard<std::mutex> lock(mutex);
			iteratorsMap.clear();
		}
	}

	// Called on every read operation.
	ReadIterator getIterator() {
		if (SERVER_KNOBS->ROCKSDB_READ_RANGE_REUSE_ITERATORS) {
			std::lock_guard<std::mutex> lock(mutex);
			for (it = iteratorsMap.begin(); it != iteratorsMap.end(); it++) {
				if (!it->second.inUse) {
					it->second.inUse = true;
					iteratorsReuseCount++;
					return it->second;
				}
			}
			index++;
			ReadIterator iter(cf, index, db, readRangeOptions);
			iteratorsMap.insert({ index, iter });
			return iter;
		} else {
			index++;
			ReadIterator iter(cf, index, db, readRangeOptions);
			return iter;
		}
	}

	// Called on every read operation, after the keys are collected.
	void returnIterator(ReadIterator& iter) {
		if (SERVER_KNOBS->ROCKSDB_READ_RANGE_REUSE_ITERATORS) {
			std::lock_guard<std::mutex> lock(mutex);
			it = iteratorsMap.find(iter.index);
			// iterator found: put the iterator back to the pool(inUse=false).
			// iterator not found: update would have removed the iterator from pool, so nothing to do.
			if (it != iteratorsMap.end()) {
				ASSERT(it->second.inUse);
				it->second.inUse = false;
			}
		}
	}

	// Called for every ROCKSDB_READ_RANGE_ITERATOR_REFRESH_TIME seconds in a loop.
	void refreshIterators() {
		std::lock_guard<std::mutex> lock(mutex);
		it = iteratorsMap.begin();
		while (it != iteratorsMap.end()) {
			if (now() - it->second.creationTime > SERVER_KNOBS->ROCKSDB_READ_RANGE_ITERATOR_REFRESH_TIME) {
				it = iteratorsMap.erase(it);
			} else {
				it++;
			}
		}
	}

	uint64_t numReadIteratorsCreated() { return index; }

	uint64_t numTimesReadIteratorsReused() { return iteratorsReuseCount; }

private:
	std::unordered_map<int, ReadIterator> iteratorsMap;
	std::unordered_map<int, ReadIterator>::iterator it;
	DB& db;
	CF& cf;
	rocksdb::ReadOptions readRangeOptions;
	std::mutex mutex;
	// incrementing counter for every new iterator creation, to uniquely identify the iterator in returnIterator().
	uint64_t index;
	uint64_t iteratorsReuseCount;
};

class PerfContextMetrics {
public:
	PerfContextMetrics();
	void reset();
	void set(int index);
	void log(bool ignoreZeroMetric);

private:
	std::vector<std::tuple<const char*, int, std::vector<uint64_t>>> metrics;
	uint64_t getRocksdbPerfcontextMetric(int metric);
};

PerfContextMetrics::PerfContextMetrics() {
	metrics = {
		{ "UserKeyComparisonCount", rocksdb_user_key_comparison_count, {} },
		{ "BlockCacheHitCount", rocksdb_block_cache_hit_count, {} },
		{ "BlockReadCount", rocksdb_block_read_count, {} },
		{ "BlockReadByte", rocksdb_block_read_byte, {} },
		{ "BlockReadTime", rocksdb_block_read_time, {} },
		{ "BlockChecksumTime", rocksdb_block_checksum_time, {} },
		{ "BlockDecompressTime", rocksdb_block_decompress_time, {} },
		{ "GetReadBytes", rocksdb_get_read_bytes, {} },
		{ "MultigetReadBytes", rocksdb_multiget_read_bytes, {} },
		{ "IterReadBytes", rocksdb_iter_read_bytes, {} },
		{ "InternalKeySkippedCount", rocksdb_internal_key_skipped_count, {} },
		{ "InternalDeleteSkippedCount", rocksdb_internal_delete_skipped_count, {} },
		{ "InternalRecentSkippedCount", rocksdb_internal_recent_skipped_count, {} },
		{ "InternalMergeCount", rocksdb_internal_merge_count, {} },
		{ "GetSnapshotTime", rocksdb_get_snapshot_time, {} },
		{ "GetFromMemtableTime", rocksdb_get_from_memtable_time, {} },
		{ "GetFromMemtableCount", rocksdb_get_from_memtable_count, {} },
		{ "GetPostProcessTime", rocksdb_get_post_process_time, {} },
		{ "GetFromOutputFilesTime", rocksdb_get_from_output_files_time, {} },
		{ "SeekOnMemtableTime", rocksdb_seek_on_memtable_time, {} },
		{ "SeekOnMemtableCount", rocksdb_seek_on_memtable_count, {} },
		{ "NextOnMemtableCount", rocksdb_next_on_memtable_count, {} },
		{ "PrevOnMemtableCount", rocksdb_prev_on_memtable_count, {} },
		{ "SeekChildSeekTime", rocksdb_seek_child_seek_time, {} },
		{ "SeekChildSeekCount", rocksdb_seek_child_seek_count, {} },
		{ "SeekMinHeapTime", rocksdb_seek_min_heap_time, {} },
		{ "SeekMaxHeapTime", rocksdb_seek_max_heap_time, {} },
		{ "SeekInternalSeekTime", rocksdb_seek_internal_seek_time, {} },
		{ "FindNextUserEntryTime", rocksdb_find_next_user_entry_time, {} },
		{ "WriteWalTime", rocksdb_write_wal_time, {} },
		{ "WriteMemtableTime", rocksdb_write_memtable_time, {} },
		{ "WriteDelayTime", rocksdb_write_delay_time, {} },
		{ "WritePreAndPostProcessTime", rocksdb_write_pre_and_post_process_time, {} },
		{ "DbMutexLockNanos", rocksdb_db_mutex_lock_nanos, {} },
		{ "DbConditionWaitNanos", rocksdb_db_condition_wait_nanos, {} },
		{ "MergeOperatorTimeNanos", rocksdb_merge_operator_time_nanos, {} },
		{ "ReadIndexBlockNanos", rocksdb_read_index_block_nanos, {} },
		{ "ReadFilterBlockNanos", rocksdb_read_filter_block_nanos, {} },
		{ "NewTableBlockIterNanos", rocksdb_new_table_block_iter_nanos, {} },
		{ "NewTableIteratorNanos", rocksdb_new_table_iterator_nanos, {} },
		{ "BlockSeekNanos", rocksdb_block_seek_nanos, {} },
		{ "FindTableNanos", rocksdb_find_table_nanos, {} },
		{ "BloomMemtableHitCount", rocksdb_bloom_memtable_hit_count, {} },
		{ "BloomMemtableMissCount", rocksdb_bloom_memtable_miss_count, {} },
		{ "BloomSstHitCount", rocksdb_bloom_sst_hit_count, {} },
		{ "BloomSstMissCount", rocksdb_bloom_sst_miss_count, {} },
		{ "KeyLockWaitTime", rocksdb_key_lock_wait_time, {} },
		{ "KeyLockWaitCount", rocksdb_key_lock_wait_count, {} },
		{ "EnvNewSequentialFileNanos", rocksdb_env_new_sequential_file_nanos, {} },
		{ "EnvNewRandomAccessFileNanos", rocksdb_env_new_random_access_file_nanos, {} },
		{ "EnvNewWritableFileNanos", rocksdb_env_new_writable_file_nanos, {} },
		{ "EnvReuseWritableFileNanos", rocksdb_env_reuse_writable_file_nanos, {} },
		{ "EnvNewRandomRwFileNanos", rocksdb_env_new_random_rw_file_nanos, {} },
		{ "EnvNewDirectoryNanos", rocksdb_env_new_directory_nanos, {} },
		{ "EnvFileExistsNanos", rocksdb_env_file_exists_nanos, {} },
		{ "EnvGetChildrenNanos", rocksdb_env_get_children_nanos, {} },
		{ "EnvGetChildrenFileAttributesNanos", rocksdb_env_get_children_file_attributes_nanos, {} },
		{ "EnvDeleteFileNanos", rocksdb_env_delete_file_nanos, {} },
		{ "EnvCreateDirNanos", rocksdb_env_create_dir_nanos, {} },
		{ "EnvCreateDirIfMissingNanos", rocksdb_env_create_dir_if_missing_nanos, {} },
		{ "EnvDeleteDirNanos", rocksdb_env_delete_dir_nanos, {} },
		{ "EnvGetFileSizeNanos", rocksdb_env_get_file_size_nanos, {} },
		{ "EnvGetFileModificationTimeNanos", rocksdb_env_get_file_modification_time_nanos, {} },
		{ "EnvRenameFileNanos", rocksdb_env_rename_file_nanos, {} },
		{ "EnvLinkFileNanos", rocksdb_env_link_file_nanos, {} },
		{ "EnvLockFileNanos", rocksdb_env_lock_file_nanos, {} },
		{ "EnvUnlockFileNanos", rocksdb_env_unlock_file_nanos, {} },
		{ "EnvNewLoggerNanos", rocksdb_env_new_logger_nanos, {} },
	};
	for (auto& [name, metric, vals] : metrics) { // readers, then writer
		for (int i = 0; i < SERVER_KNOBS->ROCKSDB_READ_PARALLELISM; i++) {
			vals.push_back(0); // add reader
		}
		vals.push_back(0); // add writer
	}
}

void PerfContextMetrics::reset() {
	rocksdb::get_perf_context()->Reset();
}

void PerfContextMetrics::set(int index) {
	for (auto& [name, metric, vals] : metrics) {
		vals[index] = getRocksdbPerfcontextMetric(metric);
	}
}

void PerfContextMetrics::log(bool ignoreZeroMetric) {
	TraceEvent e("RocksDBPerfContextMetrics");
	e.setMaxEventLength(20000);
	for (auto& [name, metric, vals] : metrics) {
		uint64_t s = 0;
		for (auto& v : vals) {
			s = s + v;
		}
		if (ignoreZeroMetric && s == 0)
			continue;
		e.detail("Sum" + (std::string)name, s);
		for (int i = 0; i < SERVER_KNOBS->ROCKSDB_READ_PARALLELISM; i++) {
			if (vals[i] != 0)
				e.detail("RD" + std::to_string(i) + name, vals[i]);
		}
		if (vals[SERVER_KNOBS->ROCKSDB_READ_PARALLELISM] != 0)
			e.detail("WR" + (std::string)name, vals[SERVER_KNOBS->ROCKSDB_READ_PARALLELISM]);
	}
}

uint64_t PerfContextMetrics::getRocksdbPerfcontextMetric(int metric) {
	switch (metric) {
	case rocksdb_user_key_comparison_count:
		return rocksdb::get_perf_context()->user_key_comparison_count;
	case rocksdb_block_cache_hit_count:
		return rocksdb::get_perf_context()->block_cache_hit_count;
	case rocksdb_block_read_count:
		return rocksdb::get_perf_context()->block_read_count;
	case rocksdb_block_read_byte:
		return rocksdb::get_perf_context()->block_read_byte;
	case rocksdb_block_read_time:
		return rocksdb::get_perf_context()->block_read_time;
	case rocksdb_block_checksum_time:
		return rocksdb::get_perf_context()->block_checksum_time;
	case rocksdb_block_decompress_time:
		return rocksdb::get_perf_context()->block_decompress_time;
	case rocksdb_get_read_bytes:
		return rocksdb::get_perf_context()->get_read_bytes;
	case rocksdb_multiget_read_bytes:
		return rocksdb::get_perf_context()->multiget_read_bytes;
	case rocksdb_iter_read_bytes:
		return rocksdb::get_perf_context()->iter_read_bytes;
	case rocksdb_internal_key_skipped_count:
		return rocksdb::get_perf_context()->internal_key_skipped_count;
	case rocksdb_internal_delete_skipped_count:
		return rocksdb::get_perf_context()->internal_delete_skipped_count;
	case rocksdb_internal_recent_skipped_count:
		return rocksdb::get_perf_context()->internal_recent_skipped_count;
	case rocksdb_internal_merge_count:
		return rocksdb::get_perf_context()->internal_merge_count;
	case rocksdb_get_snapshot_time:
		return rocksdb::get_perf_context()->get_snapshot_time;
	case rocksdb_get_from_memtable_time:
		return rocksdb::get_perf_context()->get_from_memtable_time;
	case rocksdb_get_from_memtable_count:
		return rocksdb::get_perf_context()->get_from_memtable_count;
	case rocksdb_get_post_process_time:
		return rocksdb::get_perf_context()->get_post_process_time;
	case rocksdb_get_from_output_files_time:
		return rocksdb::get_perf_context()->get_from_output_files_time;
	case rocksdb_seek_on_memtable_time:
		return rocksdb::get_perf_context()->seek_on_memtable_time;
	case rocksdb_seek_on_memtable_count:
		return rocksdb::get_perf_context()->seek_on_memtable_count;
	case rocksdb_next_on_memtable_count:
		return rocksdb::get_perf_context()->next_on_memtable_count;
	case rocksdb_prev_on_memtable_count:
		return rocksdb::get_perf_context()->prev_on_memtable_count;
	case rocksdb_seek_child_seek_time:
		return rocksdb::get_perf_context()->seek_child_seek_time;
	case rocksdb_seek_child_seek_count:
		return rocksdb::get_perf_context()->seek_child_seek_count;
	case rocksdb_seek_min_heap_time:
		return rocksdb::get_perf_context()->seek_min_heap_time;
	case rocksdb_seek_max_heap_time:
		return rocksdb::get_perf_context()->seek_max_heap_time;
	case rocksdb_seek_internal_seek_time:
		return rocksdb::get_perf_context()->seek_internal_seek_time;
	case rocksdb_find_next_user_entry_time:
		return rocksdb::get_perf_context()->find_next_user_entry_time;
	case rocksdb_write_wal_time:
		return rocksdb::get_perf_context()->write_wal_time;
	case rocksdb_write_memtable_time:
		return rocksdb::get_perf_context()->write_memtable_time;
	case rocksdb_write_delay_time:
		return rocksdb::get_perf_context()->write_delay_time;
	case rocksdb_write_pre_and_post_process_time:
		return rocksdb::get_perf_context()->write_pre_and_post_process_time;
	case rocksdb_db_mutex_lock_nanos:
		return rocksdb::get_perf_context()->db_mutex_lock_nanos;
	case rocksdb_db_condition_wait_nanos:
		return rocksdb::get_perf_context()->db_condition_wait_nanos;
	case rocksdb_merge_operator_time_nanos:
		return rocksdb::get_perf_context()->merge_operator_time_nanos;
	case rocksdb_read_index_block_nanos:
		return rocksdb::get_perf_context()->read_index_block_nanos;
	case rocksdb_read_filter_block_nanos:
		return rocksdb::get_perf_context()->read_filter_block_nanos;
	case rocksdb_new_table_block_iter_nanos:
		return rocksdb::get_perf_context()->new_table_block_iter_nanos;
	case rocksdb_new_table_iterator_nanos:
		return rocksdb::get_perf_context()->new_table_iterator_nanos;
	case rocksdb_block_seek_nanos:
		return rocksdb::get_perf_context()->block_seek_nanos;
	case rocksdb_find_table_nanos:
		return rocksdb::get_perf_context()->find_table_nanos;
	case rocksdb_bloom_memtable_hit_count:
		return rocksdb::get_perf_context()->bloom_memtable_hit_count;
	case rocksdb_bloom_memtable_miss_count:
		return rocksdb::get_perf_context()->bloom_memtable_miss_count;
	case rocksdb_bloom_sst_hit_count:
		return rocksdb::get_perf_context()->bloom_sst_hit_count;
	case rocksdb_bloom_sst_miss_count:
		return rocksdb::get_perf_context()->bloom_sst_miss_count;
	case rocksdb_key_lock_wait_time:
		return rocksdb::get_perf_context()->key_lock_wait_time;
	case rocksdb_key_lock_wait_count:
		return rocksdb::get_perf_context()->key_lock_wait_count;
	case rocksdb_env_new_sequential_file_nanos:
		return rocksdb::get_perf_context()->env_new_sequential_file_nanos;
	case rocksdb_env_new_random_access_file_nanos:
		return rocksdb::get_perf_context()->env_new_random_access_file_nanos;
	case rocksdb_env_new_writable_file_nanos:
		return rocksdb::get_perf_context()->env_new_writable_file_nanos;
	case rocksdb_env_reuse_writable_file_nanos:
		return rocksdb::get_perf_context()->env_reuse_writable_file_nanos;
	case rocksdb_env_new_random_rw_file_nanos:
		return rocksdb::get_perf_context()->env_new_random_rw_file_nanos;
	case rocksdb_env_new_directory_nanos:
		return rocksdb::get_perf_context()->env_new_directory_nanos;
	case rocksdb_env_file_exists_nanos:
		return rocksdb::get_perf_context()->env_file_exists_nanos;
	case rocksdb_env_get_children_nanos:
		return rocksdb::get_perf_context()->env_get_children_nanos;
	case rocksdb_env_get_children_file_attributes_nanos:
		return rocksdb::get_perf_context()->env_get_children_file_attributes_nanos;
	case rocksdb_env_delete_file_nanos:
		return rocksdb::get_perf_context()->env_delete_file_nanos;
	case rocksdb_env_create_dir_nanos:
		return rocksdb::get_perf_context()->env_create_dir_nanos;
	case rocksdb_env_create_dir_if_missing_nanos:
		return rocksdb::get_perf_context()->env_create_dir_if_missing_nanos;
	case rocksdb_env_delete_dir_nanos:
		return rocksdb::get_perf_context()->env_delete_dir_nanos;
	case rocksdb_env_get_file_size_nanos:
		return rocksdb::get_perf_context()->env_get_file_size_nanos;
	case rocksdb_env_get_file_modification_time_nanos:
		return rocksdb::get_perf_context()->env_get_file_modification_time_nanos;
	case rocksdb_env_rename_file_nanos:
		return rocksdb::get_perf_context()->env_rename_file_nanos;
	case rocksdb_env_link_file_nanos:
		return rocksdb::get_perf_context()->env_link_file_nanos;
	case rocksdb_env_lock_file_nanos:
		return rocksdb::get_perf_context()->env_lock_file_nanos;
	case rocksdb_env_unlock_file_nanos:
		return rocksdb::get_perf_context()->env_unlock_file_nanos;
	case rocksdb_env_new_logger_nanos:
		return rocksdb::get_perf_context()->env_new_logger_nanos;
	default:
		break;
	}
	return 0;
}

ACTOR Future<Void> refreshReadIteratorPool(std::shared_ptr<ReadIteratorPool> readIterPool) {
	if (SERVER_KNOBS->ROCKSDB_READ_RANGE_REUSE_ITERATORS) {
		loop {
			wait(delay(SERVER_KNOBS->ROCKSDB_READ_RANGE_ITERATOR_REFRESH_TIME));
			readIterPool->refreshIterators();
		}
	}
	return Void();
}

ACTOR Future<Void> flowLockLogger(UID id, const FlowLock* readLock, const FlowLock* fetchLock) {
	loop {
		wait(delay(SERVER_KNOBS->ROCKSDB_METRICS_DELAY));
		TraceEvent e("RocksDBFlowLock", id);
		e.detail("ReadAvailable", readLock->available());
		e.detail("ReadActivePermits", readLock->activePermits());
		e.detail("ReadWaiters", readLock->waiters());
		e.detail("FetchAvailable", fetchLock->available());
		e.detail("FetchActivePermits", fetchLock->activePermits());
		e.detail("FetchWaiters", fetchLock->waiters());
	}
}

ACTOR Future<Void> rocksDBMetricLogger(UID id,
                                       std::shared_ptr<SharedRocksDBState> sharedState,
                                       std::shared_ptr<rocksdb::Statistics> statistics,
                                       std::shared_ptr<PerfContextMetrics> perfContextMetrics,
                                       rocksdb::DB* db,
                                       std::shared_ptr<ReadIteratorPool> readIterPool,
                                       Counters* counters,
                                       CF cf) {
	state std::vector<std::tuple<const char*, uint32_t, uint64_t>> tickerStats = {
		{ "StallMicros", rocksdb::STALL_MICROS, 0 },
		{ "BytesRead", rocksdb::BYTES_READ, 0 },
		{ "IterBytesRead", rocksdb::ITER_BYTES_READ, 0 },
		{ "BytesWritten", rocksdb::BYTES_WRITTEN, 0 },
		{ "BlockCacheMisses", rocksdb::BLOCK_CACHE_MISS, 0 },
		{ "BlockCacheHits", rocksdb::BLOCK_CACHE_HIT, 0 },
		{ "BloomFilterUseful", rocksdb::BLOOM_FILTER_USEFUL, 0 },
		{ "BloomFilterFullPositive", rocksdb::BLOOM_FILTER_FULL_POSITIVE, 0 },
		{ "BloomFilterTruePositive", rocksdb::BLOOM_FILTER_FULL_TRUE_POSITIVE, 0 },
		{ "BloomFilterMicros", rocksdb::BLOOM_FILTER_MICROS, 0 },
		{ "MemtableHit", rocksdb::MEMTABLE_HIT, 0 },
		{ "MemtableMiss", rocksdb::MEMTABLE_MISS, 0 },
		{ "GetHitL0", rocksdb::GET_HIT_L0, 0 },
		{ "GetHitL1", rocksdb::GET_HIT_L1, 0 },
		{ "GetHitL2AndUp", rocksdb::GET_HIT_L2_AND_UP, 0 },
		{ "CountKeysWritten", rocksdb::NUMBER_KEYS_WRITTEN, 0 },
		{ "CountKeysRead", rocksdb::NUMBER_KEYS_READ, 0 },
		{ "CountDBSeek", rocksdb::NUMBER_DB_SEEK, 0 },
		{ "CountDBNext", rocksdb::NUMBER_DB_NEXT, 0 },
		{ "CountDBPrev", rocksdb::NUMBER_DB_PREV, 0 },
		{ "BloomFilterPrefixChecked", rocksdb::BLOOM_FILTER_PREFIX_CHECKED, 0 },
		{ "BloomFilterPrefixUseful", rocksdb::BLOOM_FILTER_PREFIX_USEFUL, 0 },
		{ "BlockCacheCompressedMiss", rocksdb::BLOCK_CACHE_COMPRESSED_MISS, 0 },
		{ "BlockCacheCompressedHit", rocksdb::BLOCK_CACHE_COMPRESSED_HIT, 0 },
		{ "CountWalFileSyncs", rocksdb::WAL_FILE_SYNCED, 0 },
		{ "CountWalFileBytes", rocksdb::WAL_FILE_BYTES, 0 },
		{ "CompactReadBytes", rocksdb::COMPACT_READ_BYTES, 0 },
		{ "CompactWriteBytes", rocksdb::COMPACT_WRITE_BYTES, 0 },
		{ "FlushWriteBytes", rocksdb::FLUSH_WRITE_BYTES, 0 },
		{ "CountBlocksCompressed", rocksdb::NUMBER_BLOCK_COMPRESSED, 0 },
		{ "CountBlocksDecompressed", rocksdb::NUMBER_BLOCK_DECOMPRESSED, 0 },
		{ "RowCacheHit", rocksdb::ROW_CACHE_HIT, 0 },
		{ "RowCacheMiss", rocksdb::ROW_CACHE_MISS, 0 },
		{ "CountIterSkippedKeys", rocksdb::NUMBER_ITER_SKIP, 0 },

	};

	state std::vector<std::pair<const char*, std::string>> intPropertyStats = {
		{ "NumImmutableMemtables", rocksdb::DB::Properties::kNumImmutableMemTable },
		{ "NumImmutableMemtablesFlushed", rocksdb::DB::Properties::kNumImmutableMemTableFlushed },
		{ "IsMemtableFlushPending", rocksdb::DB::Properties::kMemTableFlushPending },
		{ "NumRunningFlushes", rocksdb::DB::Properties::kNumRunningFlushes },
		{ "IsCompactionPending", rocksdb::DB::Properties::kCompactionPending },
		{ "NumRunningCompactions", rocksdb::DB::Properties::kNumRunningCompactions },
		{ "CumulativeBackgroundErrors", rocksdb::DB::Properties::kBackgroundErrors },
		{ "CurrentSizeActiveMemtable", rocksdb::DB::Properties::kCurSizeActiveMemTable },
		{ "AllMemtablesBytes", rocksdb::DB::Properties::kCurSizeAllMemTables },
		{ "ActiveMemtableBytes", rocksdb::DB::Properties::kSizeAllMemTables },
		{ "CountEntriesActiveMemtable", rocksdb::DB::Properties::kNumEntriesActiveMemTable },
		{ "CountEntriesImmutMemtables", rocksdb::DB::Properties::kNumEntriesImmMemTables },
		{ "CountDeletesActiveMemtable", rocksdb::DB::Properties::kNumDeletesActiveMemTable },
		{ "CountDeletesImmutMemtables", rocksdb::DB::Properties::kNumDeletesImmMemTables },
		{ "EstimatedCountKeys", rocksdb::DB::Properties::kEstimateNumKeys },
		{ "EstimateSstReaderBytes", rocksdb::DB::Properties::kEstimateTableReadersMem },
		{ "CountActiveSnapshots", rocksdb::DB::Properties::kNumSnapshots },
		{ "OldestSnapshotTime", rocksdb::DB::Properties::kOldestSnapshotTime },
		{ "CountLiveVersions", rocksdb::DB::Properties::kNumLiveVersions },
		{ "EstimateLiveDataSize", rocksdb::DB::Properties::kEstimateLiveDataSize },
		{ "BaseLevel", rocksdb::DB::Properties::kBaseLevel },
		{ "EstPendCompactBytes", rocksdb::DB::Properties::kEstimatePendingCompactionBytes },
		{ "BlockCacheUsage", rocksdb::DB::Properties::kBlockCacheUsage },
		{ "BlockCachePinnedUsage", rocksdb::DB::Properties::kBlockCachePinnedUsage },
		{ "LiveSstFilesSize", rocksdb::DB::Properties::kLiveSstFilesSize },
	};

	state std::vector<std::pair<const char*, std::string>> strPropertyStats = {
		{ "LevelStats", rocksdb::DB::Properties::kLevelStats },
	};

	state std::vector<std::pair<const char*, std::string>> levelStrPropertyStats = {
		{ "CompressionRatioAtLevel", rocksdb::DB::Properties::kCompressionRatioAtLevelPrefix },
	};

	state std::unordered_map<std::string, uint64_t> readIteratorPoolStats = {
		{ "NumReadIteratorsCreated", 0 },
		{ "NumTimesReadIteratorsReused", 0 },
	};

	loop {
		wait(delay(SERVER_KNOBS->ROCKSDB_METRICS_DELAY));
		if (sharedState->isClosing()) {
			break;
		}
		TraceEvent e("RocksDBMetrics", id);
		uint64_t stat;
		for (auto& [name, ticker, cum] : tickerStats) {
			stat = statistics->getTickerCount(ticker);
			e.detail(name, stat - cum);
			cum = stat;
		}

		for (const auto& [name, property] : intPropertyStats) {
			stat = 0;
			// GetAggregatedIntProperty gets the aggregated int property from all column families.
			ASSERT(db->GetAggregatedIntProperty(property, &stat));
			e.detail(name, stat);
		}

		std::string propValue;
		for (const auto& [name, property] : strPropertyStats) {
			propValue = "";
			ASSERT(db->GetProperty(cf, property, &propValue));
			e.detail(name, propValue);
		}

		rocksdb::ColumnFamilyMetaData cf_meta_data;
		db->GetColumnFamilyMetaData(cf, &cf_meta_data);
		int numLevels = static_cast<int>(cf_meta_data.levels.size());
		std::string levelProp;
		for (const auto& [name, property] : levelStrPropertyStats) {
			levelProp = "";
			for (int level = 0; level < numLevels; level++) {
				propValue = "";
				ASSERT(db->GetProperty(cf, property + std::to_string(level), &propValue));
				levelProp += std::to_string(level) + ":" + propValue + (level == numLevels - 1 ? "" : ",");
			}
			e.detail(name, levelProp);
		}

		stat = readIterPool->numReadIteratorsCreated();
		e.detail("NumReadIteratorsCreated", stat - readIteratorPoolStats["NumReadIteratorsCreated"]);
		readIteratorPoolStats["NumReadIteratorsCreated"] = stat;

		stat = readIterPool->numTimesReadIteratorsReused();
		e.detail("NumTimesReadIteratorsReused", stat - readIteratorPoolStats["NumTimesReadIteratorsReused"]);
		readIteratorPoolStats["NumTimesReadIteratorsReused"] = stat;

		counters->cc.logToTraceEvent(e);

		if (SERVER_KNOBS->ROCKSDB_PERFCONTEXT_ENABLE) {
			perfContextMetrics->log(true);
		}
	}

	return Void();
}

void logRocksDBError(UID id,
                     const rocksdb::Status& status,
                     const std::string& method,
                     Optional<Severity> sev = Optional<Severity>()) {
	Severity level = sev.present() ? sev.get() : (status.IsTimedOut() ? SevWarn : SevError);
	TraceEvent e(level, "RocksDBError", id);
	e.detail("Error", status.ToString()).detail("Method", method).detail("RocksDBSeverity", status.severity());
	if (status.IsIOError()) {
		e.detail("SubCode", status.subcode());
	}
}

Error statusToError(const rocksdb::Status& s) {
	if (s.IsIOError()) {
		return io_error();
	} else if (s.IsTimedOut()) {
		return transaction_too_old();
	} else {
		return unknown_error();
	}
}

struct RocksDBKeyValueStore : IKeyValueStore {
	struct Writer : IThreadPoolReceiver {
		struct CheckpointAction : TypedAction<Writer, CheckpointAction> {
			CheckpointAction(const CheckpointRequest& request) : request(request) {}

			double getTimeEstimate() const override { return SERVER_KNOBS->COMMIT_TIME_ESTIMATE; }

			const CheckpointRequest request;
			ThreadReturnPromise<CheckpointMetaData> reply;
		};

		struct RestoreAction : TypedAction<Writer, RestoreAction> {
			RestoreAction(const std::string& path, const std::vector<CheckpointMetaData>& checkpoints)
			  : path(path), checkpoints(checkpoints) {}

			double getTimeEstimate() const override { return SERVER_KNOBS->COMMIT_TIME_ESTIMATE; }

			const std::string path;
			const std::vector<CheckpointMetaData> checkpoints;
			ThreadReturnPromise<Void> done;
		};

		explicit Writer(DB& db,
		                CF& cf,
		                UID id,
		                std::shared_ptr<SharedRocksDBState> sharedState,
		                std::shared_ptr<ReadIteratorPool> readIterPool,
		                std::shared_ptr<PerfContextMetrics> perfContextMetrics,
		                int threadIndex,
		                ThreadReturnPromiseStream<std::pair<std::string, double>>* metricPromiseStream)
		  : db(db), cf(cf), id(id), sharedState(sharedState), readIterPool(readIterPool),
		    perfContextMetrics(perfContextMetrics), threadIndex(threadIndex), metricPromiseStream(metricPromiseStream),
		    rateLimiter(SERVER_KNOBS->ROCKSDB_WRITE_RATE_LIMITER_BYTES_PER_SEC > 0
		                    ? rocksdb::NewGenericRateLimiter(
		                          SERVER_KNOBS->ROCKSDB_WRITE_RATE_LIMITER_BYTES_PER_SEC, // rate_bytes_per_sec
		                          100 * 1000, // refill_period_us
		                          10, // fairness
		                          rocksdb::RateLimiter::Mode::kWritesOnly,
		                          SERVER_KNOBS->ROCKSDB_WRITE_RATE_LIMITER_AUTO_TUNE)
		                    : nullptr) {
			if (SERVER_KNOBS->ROCKSDB_PERFCONTEXT_ENABLE) {
				// Enable perf context on the same thread with the db thread
				rocksdb::SetPerfLevel(rocksdb::PerfLevel::kEnableTimeExceptForMutex);
				perfContextMetrics->reset();
			}
		}

		void init() override {}

		struct OpenAction : TypedAction<Writer, OpenAction> {
			std::string path;
			ThreadReturnPromise<Void> done;
			Optional<Future<Void>>& metrics;
			const FlowLock* readLock;
			const FlowLock* fetchLock;
			std::shared_ptr<RocksDBErrorListener> errorListener;
			Counters& counters;
			OpenAction(std::string path,
			           Optional<Future<Void>>& metrics,
			           const FlowLock* readLock,
			           const FlowLock* fetchLock,
			           std::shared_ptr<RocksDBErrorListener> errorListener,
			           Counters& counters)
			  : path(std::move(path)), metrics(metrics), readLock(readLock), fetchLock(fetchLock),
			    errorListener(errorListener), counters(counters) {}

			double getTimeEstimate() const override { return SERVER_KNOBS->COMMIT_TIME_ESTIMATE; }
		};
		void action(OpenAction& a) {
			ASSERT(cf == nullptr);

			std::vector<std::string> columnFamilies;
			rocksdb::DBOptions options = sharedState->getDbOptions();
			rocksdb::Status status = rocksdb::DB::ListColumnFamilies(options, a.path, &columnFamilies);
			if (std::find(columnFamilies.begin(), columnFamilies.end(), "default") == columnFamilies.end()) {
				columnFamilies.push_back("default");
			}

			rocksdb::ColumnFamilyOptions cfOptions = sharedState->getCfOptions();
			std::vector<rocksdb::ColumnFamilyDescriptor> descriptors;
			for (const std::string& name : columnFamilies) {
				descriptors.push_back(rocksdb::ColumnFamilyDescriptor{ name, cfOptions });
			}

			options.listeners.push_back(a.errorListener);
			if (SERVER_KNOBS->ROCKSDB_WRITE_RATE_LIMITER_BYTES_PER_SEC > 0) {
				options.rate_limiter = rateLimiter;
			}

			std::vector<rocksdb::ColumnFamilyHandle*> handles;
			status = rocksdb::DB::Open(options, a.path, descriptors, &handles, &db);
			cfHandles.insert(handles.begin(), handles.end());

			if (!status.ok()) {
				logRocksDBError(id, status, "Open");
				a.done.sendError(statusToError(status));
				return;
			}

			for (rocksdb::ColumnFamilyHandle* handle : handles) {
				if (handle->GetName() == SERVER_KNOBS->DEFAULT_FDB_ROCKSDB_COLUMN_FAMILY) {
					cf = handle;
					break;
				}
			}

			if (cf == nullptr) {
				status = db->CreateColumnFamily(cfOptions, SERVER_KNOBS->DEFAULT_FDB_ROCKSDB_COLUMN_FAMILY, &cf);
				cfHandles.insert(cf);
				if (!status.ok()) {
					logRocksDBError(id, status, "Open");
					a.done.sendError(statusToError(status));
				}
			}

			TraceEvent(SevInfo, "RocksDB", id)
			    .detail("Path", a.path)
			    .detail("Method", "Open")
			    .detail("KnobRocksDBWriteRateLimiterBytesPerSec",
			            SERVER_KNOBS->ROCKSDB_WRITE_RATE_LIMITER_BYTES_PER_SEC)
			    .detail("KnobRocksDBWriteRateLimiterAutoTune", SERVER_KNOBS->ROCKSDB_WRITE_RATE_LIMITER_AUTO_TUNE)
			    .detail("ColumnFamily", cf->GetName());
			if (g_network->isSimulated()) {
				// The current thread and main thread are same when the code runs in simulation.
				// blockUntilReady() is getting the thread into deadlock state, so directly calling
				// the metricsLogger.
				a.metrics =
				    rocksDBMetricLogger(
				        id, sharedState, options.statistics, perfContextMetrics, db, readIterPool, &a.counters, cf) &&
				    flowLockLogger(id, a.readLock, a.fetchLock) && refreshReadIteratorPool(readIterPool);
			} else {
				onMainThread([&] {
					a.metrics = rocksDBMetricLogger(id,
					                                sharedState,
					                                options.statistics,
					                                perfContextMetrics,
					                                db,
					                                readIterPool,
					                                &a.counters,
					                                cf) &&
					            flowLockLogger(id, a.readLock, a.fetchLock) && refreshReadIteratorPool(readIterPool);
					return Future<bool>(true);
				}).blockUntilReady();
			}
			a.done.send(Void());
		}

		struct DeleteVisitor : public rocksdb::WriteBatch::Handler {
			VectorRef<KeyRangeRef>& deletes;
			Arena& arena;

			DeleteVisitor(VectorRef<KeyRangeRef>& deletes, Arena& arena) : deletes(deletes), arena(arena) {}

			rocksdb::Status DeleteRangeCF(uint32_t /*column_family_id*/,
			                              const rocksdb::Slice& begin,
			                              const rocksdb::Slice& end) override {
				KeyRangeRef kr(toStringRef(begin), toStringRef(end));
				deletes.push_back_deep(arena, kr);
				return rocksdb::Status::OK();
			}

			rocksdb::Status PutCF(uint32_t column_family_id,
			                      const rocksdb::Slice& key,
			                      const rocksdb::Slice& value) override {
				return rocksdb::Status::OK();
			}

			rocksdb::Status DeleteCF(uint32_t column_family_id, const rocksdb::Slice& key) override {
				return rocksdb::Status::OK();
			}

			rocksdb::Status SingleDeleteCF(uint32_t column_family_id, const rocksdb::Slice& key) override {
				return rocksdb::Status::OK();
			}

			rocksdb::Status MergeCF(uint32_t column_family_id,
			                        const rocksdb::Slice& key,
			                        const rocksdb::Slice& value) override {
				return rocksdb::Status::OK();
			}
		};

		struct CommitAction : TypedAction<Writer, CommitAction> {
			std::unique_ptr<rocksdb::WriteBatch> batchToCommit;
			ThreadReturnPromise<Void> done;
			double startTime;
			bool getHistograms;
			double getTimeEstimate() const override { return SERVER_KNOBS->COMMIT_TIME_ESTIMATE; }
			CommitAction() {
				if (deterministicRandom()->random01() < SERVER_KNOBS->ROCKSDB_HISTOGRAMS_SAMPLE_RATE) {
					getHistograms = true;
					startTime = timer_monotonic();
				} else {
					getHistograms = false;
				}
			}
		};
		void action(CommitAction& a) {
			bool doPerfContextMetrics =
			    SERVER_KNOBS->ROCKSDB_PERFCONTEXT_ENABLE &&
			    (deterministicRandom()->random01() < SERVER_KNOBS->ROCKSDB_PERFCONTEXT_SAMPLE_RATE);
			if (doPerfContextMetrics) {
				perfContextMetrics->reset();
			}
			double commitBeginTime;
			if (a.getHistograms) {
				commitBeginTime = timer_monotonic();
				metricPromiseStream->send(
				    std::make_pair(ROCKSDB_COMMIT_QUEUEWAIT_HISTOGRAM.toString(), commitBeginTime - a.startTime));
			}
			Standalone<VectorRef<KeyRangeRef>> deletes;
			DeleteVisitor dv(deletes, deletes.arena());
			rocksdb::Status s = a.batchToCommit->Iterate(&dv);
			if (!s.ok()) {
				logRocksDBError(id, s, "CommitDeleteVisitor");
				a.done.sendError(statusToError(s));
				return;
			}
			// If there are any range deletes, we should have added them to be deleted.
			ASSERT(!deletes.empty() || !a.batchToCommit->HasDeleteRange());
			rocksdb::WriteOptions options;
			options.sync = !SERVER_KNOBS->ROCKSDB_UNSAFE_AUTO_FSYNC;

			double writeBeginTime = a.getHistograms ? timer_monotonic() : 0;
			if (rateLimiter) {
				// Controls the total write rate of compaction and flush in bytes per second.
				// Request for batchToCommit bytes. If this request cannot be satisfied, the call is blocked.
				rateLimiter->Request(a.batchToCommit->GetDataSize() /* bytes */, rocksdb::Env::IO_HIGH);
			}
			s = db->Write(options, a.batchToCommit.get());
			readIterPool->update();
			if (a.getHistograms) {
				metricPromiseStream->send(
				    std::make_pair(ROCKSDB_WRITE_HISTOGRAM.toString(), timer_monotonic() - writeBeginTime));
			}

			if (!s.ok()) {
				logRocksDBError(id, s, "Commit");
				a.done.sendError(statusToError(s));
			} else {
				a.done.send(Void());

				double compactRangeBeginTime = a.getHistograms ? timer_monotonic() : 0;
				for (const auto& keyRange : deletes) {
					auto begin = toSlice(keyRange.begin);
					auto end = toSlice(keyRange.end);
					ASSERT(db->SuggestCompactRange(cf, &begin, &end).ok());
				}
				if (a.getHistograms) {
					metricPromiseStream->send(std::make_pair(ROCKSDB_DELETE_COMPACTRANGE_HISTOGRAM.toString(),
					                                         timer_monotonic() - compactRangeBeginTime));
				}
			}
			if (a.getHistograms) {
				double currTime = timer_monotonic();
				metricPromiseStream->send(
				    std::make_pair(ROCKSDB_COMMIT_ACTION_HISTOGRAM.toString(), currTime - commitBeginTime));
				metricPromiseStream->send(
				    std::make_pair(ROCKSDB_COMMIT_LATENCY_HISTOGRAM.toString(), currTime - a.startTime));
			}
			if (doPerfContextMetrics) {
				perfContextMetrics->set(threadIndex);
			}
		}

		struct CloseAction : TypedAction<Writer, CloseAction> {
			ThreadReturnPromise<Void> done;
			std::string path;
			bool deleteOnClose;
			CloseAction(std::string path, bool deleteOnClose) : path(path), deleteOnClose(deleteOnClose) {}
			double getTimeEstimate() const override { return SERVER_KNOBS->COMMIT_TIME_ESTIMATE; }
		};
		void action(CloseAction& a) {
			readIterPool.reset();
			if (db == nullptr) {
				a.done.send(Void());
				return;
			}
			for (rocksdb::ColumnFamilyHandle* handle : cfHandles) {
				if (handle != nullptr) {
					db->DestroyColumnFamilyHandle(handle);
				}
			}
			cfHandles.clear();
			auto s = db->Close();
			if (!s.ok()) {
				logRocksDBError(id, s, "Close");
			}
			if (a.deleteOnClose) {
				std::set<std::string> columnFamilies{ "default" };
				columnFamilies.insert(SERVER_KNOBS->DEFAULT_FDB_ROCKSDB_COLUMN_FAMILY);
				std::vector<rocksdb::ColumnFamilyDescriptor> descriptors;
				for (const std::string name : columnFamilies) {
					descriptors.push_back(rocksdb::ColumnFamilyDescriptor{ name, sharedState->getCfOptions() });
				}
				s = rocksdb::DestroyDB(a.path, sharedState->getOptions(), descriptors);
				if (!s.ok()) {
					logRocksDBError(id, s, "Destroy");
				} else {
					TraceEvent("RocksDB", id).detail("Path", a.path).detail("Method", "Destroy");
				}
			}
			TraceEvent("RocksDB", id).detail("Path", a.path).detail("Method", "Close");
			a.done.send(Void());
		}

		void action(CheckpointAction& a);

		void action(RestoreAction& a);

		std::shared_ptr<SharedRocksDBState> sharedState;
		DB& db;
		CF& cf;
		std::unordered_set<rocksdb::ColumnFamilyHandle*> cfHandles;
		UID id;
		std::shared_ptr<rocksdb::RateLimiter> rateLimiter;
		std::shared_ptr<ReadIteratorPool> readIterPool;
		std::shared_ptr<PerfContextMetrics> perfContextMetrics;
		int threadIndex;

		// ThreadReturnPromiseStream pair.first stores the histogram name and
		// pair.second stores the corresponding measured latency (seconds)
		ThreadReturnPromiseStream<std::pair<std::string, double>>* metricPromiseStream;
	};

	struct Reader : IThreadPoolReceiver {
		UID id;
		DB& db;
		CF& cf;
		std::shared_ptr<SharedRocksDBState> sharedState;
		double readValueTimeout;
		double readValuePrefixTimeout;
		double readRangeTimeout;
		std::shared_ptr<ReadIteratorPool> readIterPool;
		std::shared_ptr<PerfContextMetrics> perfContextMetrics;
		int threadIndex;
		ThreadReturnPromiseStream<std::pair<std::string, double>>* metricPromiseStream;
		// ThreadReturnPromiseStream pair.first stores the histogram name and
		// pair.second stores the corresponding measured latency (seconds)

		explicit Reader(UID id,
		                DB& db,
		                CF& cf,
		                std::shared_ptr<SharedRocksDBState> sharedState,
		                std::shared_ptr<ReadIteratorPool> readIterPool,
		                std::shared_ptr<PerfContextMetrics> perfContextMetrics,
		                int threadIndex,
		                ThreadReturnPromiseStream<std::pair<std::string, double>>* metricPromiseStream)
		  : id(id), db(db), cf(cf), sharedState(sharedState), readIterPool(readIterPool),
		    perfContextMetrics(perfContextMetrics), metricPromiseStream(metricPromiseStream), threadIndex(threadIndex) {
			if (g_network->isSimulated()) {
				// In simulation, increasing the read operation timeouts to 5 minutes, as some of the tests have
				// very high load and single read thread cannot process all the load within the timeouts.
				readValueTimeout = 5 * 60;
				readValuePrefixTimeout = 5 * 60;
				readRangeTimeout = 5 * 60;
			} else {
				readValueTimeout = SERVER_KNOBS->ROCKSDB_READ_VALUE_TIMEOUT;
				readValuePrefixTimeout = SERVER_KNOBS->ROCKSDB_READ_VALUE_PREFIX_TIMEOUT;
				readRangeTimeout = SERVER_KNOBS->ROCKSDB_READ_RANGE_TIMEOUT;
			}
			if (SERVER_KNOBS->ROCKSDB_PERFCONTEXT_ENABLE) {
				// Enable perf context on the same thread with the db thread
				rocksdb::SetPerfLevel(rocksdb::PerfLevel::kEnableTimeExceptForMutex);
				perfContextMetrics->reset();
			}
		}

		void init() override {}

		struct ReadValueAction : TypedAction<Reader, ReadValueAction> {
			Key key;
			Optional<UID> debugID;
			double startTime;
			bool getHistograms;
			ThreadReturnPromise<Optional<Value>> result;
			ReadValueAction(KeyRef key, Optional<UID> debugID)
			  : key(key), debugID(debugID), startTime(timer_monotonic()),
			    getHistograms(
			        (deterministicRandom()->random01() < SERVER_KNOBS->ROCKSDB_HISTOGRAMS_SAMPLE_RATE) ? true : false) {
			}
			double getTimeEstimate() const override { return SERVER_KNOBS->READ_VALUE_TIME_ESTIMATE; }
		};
		void action(ReadValueAction& a) {
			ASSERT(cf != nullptr);
			bool doPerfContextMetrics =
			    SERVER_KNOBS->ROCKSDB_PERFCONTEXT_ENABLE &&
			    (deterministicRandom()->random01() < SERVER_KNOBS->ROCKSDB_PERFCONTEXT_SAMPLE_RATE);
			if (doPerfContextMetrics) {
				perfContextMetrics->reset();
			}
			double readBeginTime = timer_monotonic();
			if (a.getHistograms) {
				metricPromiseStream->send(
				    std::make_pair(ROCKSDB_READVALUE_QUEUEWAIT_HISTOGRAM.toString(), readBeginTime - a.startTime));
			}
			Optional<TraceBatch> traceBatch;
			if (a.debugID.present()) {
				traceBatch = { TraceBatch{} };
				traceBatch.get().addEvent("GetValueDebug", a.debugID.get().first(), "Reader.Before");
			}
			if (readBeginTime - a.startTime > readValueTimeout) {
				TraceEvent(SevWarn, "KVSTimeout", id)
				    .detail("Error", "Read value request timedout")
				    .detail("Method", "ReadValueAction")
				    .detail("TimeoutValue", readValueTimeout);
				a.result.sendError(transaction_too_old());
				return;
			}

			rocksdb::PinnableSlice value;
			auto& options = sharedState->getReadOptions();
			uint64_t deadlineMircos =
			    db->GetEnv()->NowMicros() + (readValueTimeout - (readBeginTime - a.startTime)) * 1000000;
			std::chrono::seconds deadlineSeconds(deadlineMircos / 1000000);
			options.deadline = std::chrono::duration_cast<std::chrono::microseconds>(deadlineSeconds);

			double dbGetBeginTime = a.getHistograms ? timer_monotonic() : 0;
			auto s = db->Get(options, cf, toSlice(a.key), &value);
			if (!s.ok() && !s.IsNotFound()) {
				logRocksDBError(id, s, "ReadValue");
				a.result.sendError(statusToError(s));
				return;
			}

			if (a.getHistograms) {
				metricPromiseStream->send(
				    std::make_pair(ROCKSDB_READVALUE_GET_HISTOGRAM.toString(), timer_monotonic() - dbGetBeginTime));
			}

			if (a.debugID.present()) {
				traceBatch.get().addEvent("GetValueDebug", a.debugID.get().first(), "Reader.After");
				traceBatch.get().dump();
			}
			if (s.ok()) {
				a.result.send(Value(toStringRef(value)));
			} else if (s.IsNotFound()) {
				a.result.send(Optional<Value>());
			} else {
				logRocksDBError(id, s, "ReadValue");
				a.result.sendError(statusToError(s));
			}

			if (a.getHistograms) {
				double currTime = timer_monotonic();
				metricPromiseStream->send(
				    std::make_pair(ROCKSDB_READVALUE_ACTION_HISTOGRAM.toString(), currTime - readBeginTime));
				metricPromiseStream->send(
				    std::make_pair(ROCKSDB_READVALUE_LATENCY_HISTOGRAM.toString(), currTime - a.startTime));
			}
			if (doPerfContextMetrics) {
				perfContextMetrics->set(threadIndex);
			}
		}

		struct ReadValuePrefixAction : TypedAction<Reader, ReadValuePrefixAction> {
			Key key;
			int maxLength;
			Optional<UID> debugID;
			double startTime;
			bool getHistograms;
			ThreadReturnPromise<Optional<Value>> result;
			ReadValuePrefixAction(Key key, int maxLength, Optional<UID> debugID)
			  : key(key), maxLength(maxLength), debugID(debugID), startTime(timer_monotonic()),
			    getHistograms(
			        (deterministicRandom()->random01() < SERVER_KNOBS->ROCKSDB_HISTOGRAMS_SAMPLE_RATE) ? true : false) {
			}
			double getTimeEstimate() const override { return SERVER_KNOBS->READ_VALUE_TIME_ESTIMATE; }
		};
		void action(ReadValuePrefixAction& a) {
			bool doPerfContextMetrics =
			    SERVER_KNOBS->ROCKSDB_PERFCONTEXT_ENABLE &&
			    (deterministicRandom()->random01() < SERVER_KNOBS->ROCKSDB_PERFCONTEXT_SAMPLE_RATE);
			if (doPerfContextMetrics) {
				perfContextMetrics->reset();
			}
			double readBeginTime = timer_monotonic();
			if (a.getHistograms) {
				metricPromiseStream->send(
				    std::make_pair(ROCKSDB_READPREFIX_QUEUEWAIT_HISTOGRAM.toString(), readBeginTime - a.startTime));
			}
			Optional<TraceBatch> traceBatch;
			if (a.debugID.present()) {
				traceBatch = { TraceBatch{} };
				traceBatch.get().addEvent("GetValuePrefixDebug",
				                          a.debugID.get().first(),
				                          "Reader.Before"); //.detail("TaskID", g_network->getCurrentTask());
			}
			if (readBeginTime - a.startTime > readValuePrefixTimeout) {
				TraceEvent(SevWarn, "KVSTimeout", id)
				    .detail("Error", "Read value prefix request timedout")
				    .detail("Method", "ReadValuePrefixAction")
				    .detail("TimeoutValue", readValuePrefixTimeout);
				a.result.sendError(transaction_too_old());
				return;
			}

			rocksdb::PinnableSlice value;
			auto& options = sharedState->getReadOptions();
			uint64_t deadlineMircos =
			    db->GetEnv()->NowMicros() + (readValuePrefixTimeout - (readBeginTime - a.startTime)) * 1000000;
			std::chrono::seconds deadlineSeconds(deadlineMircos / 1000000);
			options.deadline = std::chrono::duration_cast<std::chrono::microseconds>(deadlineSeconds);

			double dbGetBeginTime = a.getHistograms ? timer_monotonic() : 0;
			auto s = db->Get(options, cf, toSlice(a.key), &value);
			if (a.getHistograms) {
				metricPromiseStream->send(
				    std::make_pair(ROCKSDB_READPREFIX_GET_HISTOGRAM.toString(), timer_monotonic() - dbGetBeginTime));
			}

			if (a.debugID.present()) {
				traceBatch.get().addEvent("GetValuePrefixDebug",
				                          a.debugID.get().first(),
				                          "Reader.After"); //.detail("TaskID", g_network->getCurrentTask());
				traceBatch.get().dump();
			}
			if (s.ok()) {
				a.result.send(Value(StringRef(reinterpret_cast<const uint8_t*>(value.data()),
				                              std::min(value.size(), size_t(a.maxLength)))));
			} else if (s.IsNotFound()) {
				a.result.send(Optional<Value>());
			} else {
				logRocksDBError(id, s, "ReadValuePrefix");
				a.result.sendError(statusToError(s));
			}
			if (a.getHistograms) {
				double currTime = timer_monotonic();
				metricPromiseStream->send(
				    std::make_pair(ROCKSDB_READPREFIX_ACTION_HISTOGRAM.toString(), currTime - readBeginTime));
				metricPromiseStream->send(
				    std::make_pair(ROCKSDB_READPREFIX_LATENCY_HISTOGRAM.toString(), currTime - a.startTime));
			}
			if (doPerfContextMetrics) {
				perfContextMetrics->set(threadIndex);
			}
		}

		struct ReadRangeAction : TypedAction<Reader, ReadRangeAction>, FastAllocated<ReadRangeAction> {
			KeyRange keys;
			int rowLimit, byteLimit;
			double startTime;
			bool getHistograms;
			ThreadReturnPromise<RangeResult> result;
			ReadRangeAction(KeyRange keys, int rowLimit, int byteLimit)
			  : keys(keys), rowLimit(rowLimit), byteLimit(byteLimit), startTime(timer_monotonic()),
			    getHistograms(
			        (deterministicRandom()->random01() < SERVER_KNOBS->ROCKSDB_HISTOGRAMS_SAMPLE_RATE) ? true : false) {
			}
			double getTimeEstimate() const override { return SERVER_KNOBS->READ_RANGE_TIME_ESTIMATE; }
		};
		void action(ReadRangeAction& a) {
			bool doPerfContextMetrics =
			    SERVER_KNOBS->ROCKSDB_PERFCONTEXT_ENABLE &&
			    (deterministicRandom()->random01() < SERVER_KNOBS->ROCKSDB_PERFCONTEXT_SAMPLE_RATE);
			if (doPerfContextMetrics) {
				perfContextMetrics->reset();
			}
			double readBeginTime = timer_monotonic();
			if (a.getHistograms) {
				metricPromiseStream->send(
				    std::make_pair(ROCKSDB_READRANGE_QUEUEWAIT_HISTOGRAM.toString(), readBeginTime - a.startTime));
			}
			if (readBeginTime - a.startTime > readRangeTimeout) {
				TraceEvent(SevWarn, "KVSTimeout", id)
				    .detail("Error", "Read range request timedout")
				    .detail("Method", "ReadRangeAction")
				    .detail("TimeoutValue", readRangeTimeout);
				a.result.sendError(transaction_too_old());
				return;
			}

			RangeResult result;
			if (a.rowLimit == 0 || a.byteLimit == 0) {
				a.result.send(result);
			}
			int accumulatedBytes = 0;
			rocksdb::Status s;
			if (a.rowLimit >= 0) {
				double iterCreationBeginTime = a.getHistograms ? timer_monotonic() : 0;
				ReadIterator readIter = readIterPool->getIterator();
				if (a.getHistograms) {
					metricPromiseStream->send(std::make_pair(ROCKSDB_READRANGE_NEWITERATOR_HISTOGRAM.toString(),
					                                         timer_monotonic() - iterCreationBeginTime));
				}
				auto cursor = readIter.iter;
				cursor->Seek(toSlice(a.keys.begin));
				while (cursor->Valid() && toStringRef(cursor->key()) < a.keys.end) {
					KeyValueRef kv(toStringRef(cursor->key()), toStringRef(cursor->value()));
					accumulatedBytes += sizeof(KeyValueRef) + kv.expectedSize();
					result.push_back_deep(result.arena(), kv);
					// Calling `cursor->Next()` is potentially expensive, so short-circut here just in case.
					if (result.size() >= a.rowLimit || accumulatedBytes >= a.byteLimit) {
						break;
					}
					if (timer_monotonic() - a.startTime > readRangeTimeout) {
						TraceEvent(SevWarn, "KVSTimeout", id)
						    .detail("Error", "Read range request timedout")
						    .detail("Method", "ReadRangeAction")
						    .detail("TimeoutValue", readRangeTimeout);
						a.result.sendError(transaction_too_old());
						return;
					}
					cursor->Next();
				}
				s = cursor->status();
				readIterPool->returnIterator(readIter);
			} else {
				double iterCreationBeginTime = a.getHistograms ? timer_monotonic() : 0;
				ReadIterator readIter = readIterPool->getIterator();
				if (a.getHistograms) {
					metricPromiseStream->send(std::make_pair(ROCKSDB_READRANGE_NEWITERATOR_HISTOGRAM.toString(),
					                                         timer_monotonic() - iterCreationBeginTime));
				}
				auto cursor = readIter.iter;
				cursor->SeekForPrev(toSlice(a.keys.end));
				if (cursor->Valid() && toStringRef(cursor->key()) == a.keys.end) {
					cursor->Prev();
				}
				while (cursor->Valid() && toStringRef(cursor->key()) >= a.keys.begin) {
					KeyValueRef kv(toStringRef(cursor->key()), toStringRef(cursor->value()));
					accumulatedBytes += sizeof(KeyValueRef) + kv.expectedSize();
					result.push_back_deep(result.arena(), kv);
					// Calling `cursor->Prev()` is potentially expensive, so short-circut here just in case.
					if (result.size() >= -a.rowLimit || accumulatedBytes >= a.byteLimit) {
						break;
					}
					if (timer_monotonic() - a.startTime > readRangeTimeout) {
						TraceEvent(SevWarn, "KVSTimeout", id)
						    .detail("Error", "Read range request timedout")
						    .detail("Method", "ReadRangeAction")
						    .detail("TimeoutValue", readRangeTimeout);
						a.result.sendError(transaction_too_old());
						return;
					}
					cursor->Prev();
				}
				s = cursor->status();
				readIterPool->returnIterator(readIter);
			}

			if (!s.ok()) {
				logRocksDBError(id, s, "ReadRange");
				a.result.sendError(statusToError(s));
				return;
			}
			result.more =
			    (result.size() == a.rowLimit) || (result.size() == -a.rowLimit) || (accumulatedBytes >= a.byteLimit);
			if (result.more) {
				result.readThrough = result[result.size() - 1].key;
			}
			a.result.send(result);
			if (a.getHistograms) {
				double currTime = timer_monotonic();
				metricPromiseStream->send(
				    std::make_pair(ROCKSDB_READRANGE_ACTION_HISTOGRAM.toString(), currTime - readBeginTime));
				metricPromiseStream->send(
				    std::make_pair(ROCKSDB_READRANGE_LATENCY_HISTOGRAM.toString(), currTime - a.startTime));
			}
			if (doPerfContextMetrics) {
				perfContextMetrics->set(threadIndex);
			}
		}
	};

	explicit RocksDBKeyValueStore(const std::string& path, UID id)
	  : sharedState(std::make_shared<SharedRocksDBState>()), path(path), id(id),
	    perfContextMetrics(new PerfContextMetrics()),
	    readIterPool(new ReadIteratorPool(id, db, defaultFdbCF, sharedState->getReadOptions())),
	    readSemaphore(SERVER_KNOBS->ROCKSDB_READ_QUEUE_SOFT_MAX),
	    fetchSemaphore(SERVER_KNOBS->ROCKSDB_FETCH_QUEUE_SOFT_MAX),
	    numReadWaiters(SERVER_KNOBS->ROCKSDB_READ_QUEUE_HARD_MAX - SERVER_KNOBS->ROCKSDB_READ_QUEUE_SOFT_MAX),
	    numFetchWaiters(SERVER_KNOBS->ROCKSDB_FETCH_QUEUE_HARD_MAX - SERVER_KNOBS->ROCKSDB_FETCH_QUEUE_SOFT_MAX),
	    errorListener(std::make_shared<RocksDBErrorListener>(id)), errorFuture(errorListener->getFuture()) {
		// In simluation, run the reader/writer threads as Coro threads (i.e. in the network thread. The storage engine
		// is still multi-threaded as background compaction threads are still present. Reads/writes to disk will also
		// block the network thread in a way that would be unacceptable in production but is a necessary evil here. When
		// performing the reads in background threads in simulation, the event loop thinks there is no work to do and
		// advances time faster than 1 sec/sec. By the time the blocking read actually finishes, simulation has advanced
		// time by more than 5 seconds, so every read fails with a transaction_too_old error. Doing blocking IO on the
		// main thread solves this issue. There are almost certainly better fixes, but my goal was to get a less
		// invasive change merged first and work on a more realistic version if/when we think that would provide
		// substantially more confidence in the correctness.
		// TODO: Adapt the simulation framework to not advance time quickly when background reads/writes are occurring.
		if (g_network->isSimulated()) {
			writeThread = CoroThreadPool::createThreadPool();
			readThreads = CoroThreadPool::createThreadPool();
		} else {
			writeThread = createGenericThreadPool();
			readThreads = createGenericThreadPool();
		}
		if (SERVER_KNOBS->ROCKSDB_HISTOGRAMS_SAMPLE_RATE > 0) {
			collection = actorCollection(addActor.getFuture());
			for (int i = 0; i < SERVER_KNOBS->ROCKSDB_READ_PARALLELISM + 1; i++) {
				// ROCKSDB_READ_PARALLELISM readers and 1 writer
				metricPromiseStreams.emplace_back(
				    std::make_unique<ThreadReturnPromiseStream<std::pair<std::string, double>>>());
				addActor.send(updateHistogram(metricPromiseStreams[i]->getFuture()));
			}
		}

		// the writer uses SERVER_KNOBS->ROCKSDB_READ_PARALLELISM as its threadIndex
		// threadIndex is used for metricPromiseStreams and perfContextMetrics
		writeThread->addThread(new Writer(db,
		                                  defaultFdbCF,
		                                  id,
		                                  this->sharedState,
		                                  readIterPool,
		                                  perfContextMetrics,
		                                  SERVER_KNOBS->ROCKSDB_READ_PARALLELISM,
		                                  SERVER_KNOBS->ROCKSDB_HISTOGRAMS_SAMPLE_RATE > 0
		                                      ? metricPromiseStreams[SERVER_KNOBS->ROCKSDB_READ_PARALLELISM].get()
		                                      : nullptr),
		                       "fdb-rocksdb-wr");
		TraceEvent("RocksDBReadThreads", id)
		    .detail("KnobRocksDBReadParallelism", SERVER_KNOBS->ROCKSDB_READ_PARALLELISM);
		for (unsigned i = 0; i < SERVER_KNOBS->ROCKSDB_READ_PARALLELISM; ++i) {
			readThreads->addThread(
			    new Reader(id,
			               db,
			               defaultFdbCF,
			               this->sharedState,
			               readIterPool,
			               perfContextMetrics,
			               i,
			               SERVER_KNOBS->ROCKSDB_HISTOGRAMS_SAMPLE_RATE > 0 ? metricPromiseStreams[i].get() : nullptr),
			    "fdb-rocksdb-re");
		}
	}

	ACTOR Future<Void> errorListenActor(Future<Void> collection) {
		try {
			wait(collection);
			ASSERT(false);
			throw internal_error();
		} catch (Error& e) {
			throw e;
		}
	}

	ACTOR Future<Void> updateHistogram(FutureStream<std::pair<std::string, double>> metricFutureStream) {
		state Reference<Histogram> commitLatencyHistogram = Histogram::getHistogram(
		    ROCKSDBSTORAGE_HISTOGRAM_GROUP, ROCKSDB_COMMIT_LATENCY_HISTOGRAM, Histogram::Unit::microseconds);
		state Reference<Histogram> commitActionHistogram = Histogram::getHistogram(
		    ROCKSDBSTORAGE_HISTOGRAM_GROUP, ROCKSDB_COMMIT_ACTION_HISTOGRAM, Histogram::Unit::microseconds);
		state Reference<Histogram> commitQueueWaitHistogram = Histogram::getHistogram(
		    ROCKSDBSTORAGE_HISTOGRAM_GROUP, ROCKSDB_COMMIT_QUEUEWAIT_HISTOGRAM, Histogram::Unit::microseconds);
		state Reference<Histogram> writeHistogram = Histogram::getHistogram(
		    ROCKSDBSTORAGE_HISTOGRAM_GROUP, ROCKSDB_WRITE_HISTOGRAM, Histogram::Unit::microseconds);
		state Reference<Histogram> deleteCompactRangeHistogram = Histogram::getHistogram(
		    ROCKSDBSTORAGE_HISTOGRAM_GROUP, ROCKSDB_DELETE_COMPACTRANGE_HISTOGRAM, Histogram::Unit::microseconds);
		state Reference<Histogram> readRangeLatencyHistogram = Histogram::getHistogram(
		    ROCKSDBSTORAGE_HISTOGRAM_GROUP, ROCKSDB_READRANGE_LATENCY_HISTOGRAM, Histogram::Unit::microseconds);
		state Reference<Histogram> readValueLatencyHistogram = Histogram::getHistogram(
		    ROCKSDBSTORAGE_HISTOGRAM_GROUP, ROCKSDB_READVALUE_LATENCY_HISTOGRAM, Histogram::Unit::microseconds);
		state Reference<Histogram> readPrefixLatencyHistogram = Histogram::getHistogram(
		    ROCKSDBSTORAGE_HISTOGRAM_GROUP, ROCKSDB_READPREFIX_LATENCY_HISTOGRAM, Histogram::Unit::microseconds);
		state Reference<Histogram> readRangeActionHistogram = Histogram::getHistogram(
		    ROCKSDBSTORAGE_HISTOGRAM_GROUP, ROCKSDB_READRANGE_ACTION_HISTOGRAM, Histogram::Unit::microseconds);
		state Reference<Histogram> readValueActionHistogram = Histogram::getHistogram(
		    ROCKSDBSTORAGE_HISTOGRAM_GROUP, ROCKSDB_READVALUE_ACTION_HISTOGRAM, Histogram::Unit::microseconds);
		state Reference<Histogram> readPrefixActionHistogram = Histogram::getHistogram(
		    ROCKSDBSTORAGE_HISTOGRAM_GROUP, ROCKSDB_READPREFIX_ACTION_HISTOGRAM, Histogram::Unit::microseconds);
		state Reference<Histogram> readRangeQueueWaitHistogram = Histogram::getHistogram(
		    ROCKSDBSTORAGE_HISTOGRAM_GROUP, ROCKSDB_READRANGE_QUEUEWAIT_HISTOGRAM, Histogram::Unit::microseconds);
		state Reference<Histogram> readValueQueueWaitHistogram = Histogram::getHistogram(
		    ROCKSDBSTORAGE_HISTOGRAM_GROUP, ROCKSDB_READVALUE_QUEUEWAIT_HISTOGRAM, Histogram::Unit::microseconds);
		state Reference<Histogram> readPrefixQueueWaitHistogram = Histogram::getHistogram(
		    ROCKSDBSTORAGE_HISTOGRAM_GROUP, ROCKSDB_READPREFIX_QUEUEWAIT_HISTOGRAM, Histogram::Unit::microseconds);
		state Reference<Histogram> readRangeNewIteratorHistogram = Histogram::getHistogram(
		    ROCKSDBSTORAGE_HISTOGRAM_GROUP, ROCKSDB_READRANGE_NEWITERATOR_HISTOGRAM, Histogram::Unit::microseconds);
		state Reference<Histogram> readValueGetHistogram = Histogram::getHistogram(
		    ROCKSDBSTORAGE_HISTOGRAM_GROUP, ROCKSDB_READVALUE_GET_HISTOGRAM, Histogram::Unit::microseconds);
		state Reference<Histogram> readPrefixGetHistogram = Histogram::getHistogram(
		    ROCKSDBSTORAGE_HISTOGRAM_GROUP, ROCKSDB_READPREFIX_GET_HISTOGRAM, Histogram::Unit::microseconds);
		loop {
			choose {
				when(std::pair<std::string, double> measure = waitNext(metricFutureStream)) {
					std::string metricName = measure.first;
					double latency = measure.second;
					if (metricName == ROCKSDB_COMMIT_LATENCY_HISTOGRAM.toString()) {
						commitLatencyHistogram->sampleSeconds(latency);
					} else if (metricName == ROCKSDB_COMMIT_ACTION_HISTOGRAM.toString()) {
						commitActionHistogram->sampleSeconds(latency);
					} else if (metricName == ROCKSDB_COMMIT_QUEUEWAIT_HISTOGRAM.toString()) {
						commitQueueWaitHistogram->sampleSeconds(latency);
					} else if (metricName == ROCKSDB_WRITE_HISTOGRAM.toString()) {
						writeHistogram->sampleSeconds(latency);
					} else if (metricName == ROCKSDB_DELETE_COMPACTRANGE_HISTOGRAM.toString()) {
						deleteCompactRangeHistogram->sampleSeconds(latency);
					} else if (metricName == ROCKSDB_READRANGE_LATENCY_HISTOGRAM.toString()) {
						readRangeLatencyHistogram->sampleSeconds(latency);
					} else if (metricName == ROCKSDB_READVALUE_LATENCY_HISTOGRAM.toString()) {
						readValueLatencyHistogram->sampleSeconds(latency);
					} else if (metricName == ROCKSDB_READPREFIX_LATENCY_HISTOGRAM.toString()) {
						readPrefixLatencyHistogram->sampleSeconds(latency);
					} else if (metricName == ROCKSDB_READRANGE_ACTION_HISTOGRAM.toString()) {
						readRangeActionHistogram->sampleSeconds(latency);
					} else if (metricName == ROCKSDB_READVALUE_ACTION_HISTOGRAM.toString()) {
						readValueActionHistogram->sampleSeconds(latency);
					} else if (metricName == ROCKSDB_READPREFIX_ACTION_HISTOGRAM.toString()) {
						readPrefixActionHistogram->sampleSeconds(latency);
					} else if (metricName == ROCKSDB_READRANGE_QUEUEWAIT_HISTOGRAM.toString()) {
						readRangeQueueWaitHistogram->sampleSeconds(latency);
					} else if (metricName == ROCKSDB_READVALUE_QUEUEWAIT_HISTOGRAM.toString()) {
						readValueQueueWaitHistogram->sampleSeconds(latency);
					} else if (metricName == ROCKSDB_READPREFIX_QUEUEWAIT_HISTOGRAM.toString()) {
						readPrefixQueueWaitHistogram->sampleSeconds(latency);
					} else if (metricName == ROCKSDB_READRANGE_NEWITERATOR_HISTOGRAM.toString()) {
						readRangeNewIteratorHistogram->sampleSeconds(latency);
					} else if (metricName == ROCKSDB_READVALUE_GET_HISTOGRAM.toString()) {
						readValueGetHistogram->sampleSeconds(latency);
					} else if (metricName == ROCKSDB_READPREFIX_GET_HISTOGRAM.toString()) {
						readPrefixGetHistogram->sampleSeconds(latency);
					} else {
						UNREACHABLE();
					}
				}
			}
		}
	}

	Future<Void> getError() const override { return errorFuture; }

	ACTOR static void doClose(RocksDBKeyValueStore* self, bool deleteOnClose) {
		self->sharedState->setClosing();

		// The metrics future retains a reference to the DB, so stop it before we delete it.
		self->metrics.reset();

		wait(self->readThreads->stop());
		self->readIterPool.reset();
		auto a = new Writer::CloseAction(self->path, deleteOnClose);
		auto f = a->done.getFuture();
		self->writeThread->post(a);
		wait(f);
		wait(self->writeThread->stop());
		if (self->closePromise.canBeSet()) {
			self->closePromise.send(Void());
		}
		if (self->db != nullptr) {
			delete self->db;
		}
		delete self;
	}

	Future<Void> onClosed() const override { return closePromise.getFuture(); }

	void dispose() override { doClose(this, true); }

	void close() override { doClose(this, false); }

	KeyValueStoreType getType() const override {
		if (SERVER_KNOBS->ENABLE_SHARDED_ROCKSDB)
			// KVSRocks pretends as KVSShardedRocksDB
			// TODO: to remove when the ShardedRocksDB KVS implementation is added in the future
			return KeyValueStoreType(KeyValueStoreType::SSD_SHARDED_ROCKSDB);
		else
			return KeyValueStoreType(KeyValueStoreType::SSD_ROCKSDB_V1);
	}

	Future<Void> init() override {
		if (openFuture.isValid()) {
			return openFuture;
		}
		auto a = std::make_unique<Writer::OpenAction>(
		    path, metrics, &readSemaphore, &fetchSemaphore, errorListener, counters);
		openFuture = a->done.getFuture();
		writeThread->post(a.release());
		return openFuture;
	}

	void set(KeyValueRef kv, const Arena*) override {
		if (writeBatch == nullptr) {
			writeBatch.reset(new rocksdb::WriteBatch());
		}
		ASSERT(defaultFdbCF != nullptr);
		writeBatch->Put(defaultFdbCF, toSlice(kv.key), toSlice(kv.value));
	}

	void clear(KeyRangeRef keyRange, const Arena*) override {
		if (writeBatch == nullptr) {
			writeBatch.reset(new rocksdb::WriteBatch());
		}

		ASSERT(defaultFdbCF != nullptr);

		if (keyRange.singleKeyRange()) {
			writeBatch->Delete(defaultFdbCF, toSlice(keyRange.begin));
		} else {
			writeBatch->DeleteRange(defaultFdbCF, toSlice(keyRange.begin), toSlice(keyRange.end));
		}
	}

	// Checks and waits for few seconds if rocskdb is overloaded.
	ACTOR Future<Void> checkRocksdbState(RocksDBKeyValueStore* self) {
		state uint64_t estPendCompactBytes;
		state int count = SERVER_KNOBS->ROCKSDB_CAN_COMMIT_DELAY_TIMES_ON_OVERLOAD;
		self->db->GetAggregatedIntProperty(rocksdb::DB::Properties::kEstimatePendingCompactionBytes,
		                                   &estPendCompactBytes);
		while (count && estPendCompactBytes > SERVER_KNOBS->ROCKSDB_CAN_COMMIT_COMPACT_BYTES_LIMIT) {
			wait(delay(SERVER_KNOBS->ROCKSDB_CAN_COMMIT_DELAY_ON_OVERLOAD));
			count--;
			self->db->GetAggregatedIntProperty(rocksdb::DB::Properties::kEstimatePendingCompactionBytes,
			                                   &estPendCompactBytes);
		}

		return Void();
	}

	Future<Void> canCommit() override { return checkRocksdbState(this); }

	Future<Void> commit(bool) override {
		// If there is nothing to write, don't write.
		if (writeBatch == nullptr) {
			return Void();
		}
		auto a = new Writer::CommitAction();
		a->batchToCommit = std::move(writeBatch);
		auto res = a->done.getFuture();
		writeThread->post(a);
		return res;
	}

	void checkWaiters(const FlowLock& semaphore, int maxWaiters) {
		if (semaphore.waiters() > maxWaiters) {
			++counters.immediateThrottle;
			throw server_overloaded();
		}
	}

	// We don't throttle eager reads and reads to the FF keyspace because FDB struggles when those reads fail.
	// Thus far, they have been low enough volume to not cause an issue.
	static bool shouldThrottle(ReadType type, KeyRef key) {
		return type != ReadType::EAGER && !(key.startsWith(systemKeys.begin));
	}

	ACTOR template <class Action>
	static Future<Optional<Value>> read(Action* action, FlowLock* semaphore, IThreadPool* pool, Counter* counter) {
		state std::unique_ptr<Action> a(action);
		state Optional<Void> slot = wait(timeout(semaphore->take(), SERVER_KNOBS->ROCKSDB_READ_QUEUE_WAIT));
		if (!slot.present()) {
			++(*counter);
			throw server_overloaded();
		}

		state FlowLock::Releaser release(*semaphore);

		auto fut = a->result.getFuture();
		pool->post(a.release());
		Optional<Value> result = wait(fut);

		return result;
	}

	Future<Optional<Value>> readValue(KeyRef key, Optional<ReadOptions> options) override {
		ReadType type = ReadType::NORMAL;
		Optional<UID> debugID;

		if (options.present()) {
			type = options.get().type;
			debugID = options.get().debugID;
		}

		if (!shouldThrottle(type, key)) {
			auto a = new Reader::ReadValueAction(key, debugID);
			auto res = a->result.getFuture();
			readThreads->post(a);
			return res;
		}

		auto& semaphore = (type == ReadType::FETCH) ? fetchSemaphore : readSemaphore;
		int maxWaiters = (type == ReadType::FETCH) ? numFetchWaiters : numReadWaiters;

		checkWaiters(semaphore, maxWaiters);
		auto a = std::make_unique<Reader::ReadValueAction>(key, debugID);
		return read(a.release(), &semaphore, readThreads.getPtr(), &counters.failedToAcquire);
	}

	Future<Optional<Value>> readValuePrefix(KeyRef key, int maxLength, Optional<ReadOptions> options) override {
		ReadType type = ReadType::NORMAL;
		Optional<UID> debugID;

		if (options.present()) {
			type = options.get().type;
			debugID = options.get().debugID;
		}

		if (!shouldThrottle(type, key)) {
			auto a = new Reader::ReadValuePrefixAction(key, maxLength, debugID);
			auto res = a->result.getFuture();
			readThreads->post(a);
			return res;
		}

		auto& semaphore = (type == ReadType::FETCH) ? fetchSemaphore : readSemaphore;
		int maxWaiters = (type == ReadType::FETCH) ? numFetchWaiters : numReadWaiters;

		checkWaiters(semaphore, maxWaiters);
		auto a = std::make_unique<Reader::ReadValuePrefixAction>(key, maxLength, debugID);
		return read(a.release(), &semaphore, readThreads.getPtr(), &counters.failedToAcquire);
	}

	ACTOR static Future<Standalone<RangeResultRef>> read(Reader::ReadRangeAction* action,
	                                                     FlowLock* semaphore,
	                                                     IThreadPool* pool,
	                                                     Counter* counter) {
		state std::unique_ptr<Reader::ReadRangeAction> a(action);
		state Optional<Void> slot = wait(timeout(semaphore->take(), SERVER_KNOBS->ROCKSDB_READ_QUEUE_WAIT));
		if (!slot.present()) {
			++(*counter);
			throw server_overloaded();
		}

		state FlowLock::Releaser release(*semaphore);

		auto fut = a->result.getFuture();
		pool->post(a.release());
		Standalone<RangeResultRef> result = wait(fut);

		return result;
	}

	Future<RangeResult> readRange(KeyRangeRef keys,
	                              int rowLimit,
	                              int byteLimit,
	                              Optional<ReadOptions> options) override {
		ReadType type = ReadType::NORMAL;

		if (options.present()) {
			type = options.get().type;
		}

		if (!shouldThrottle(type, keys.begin)) {
			auto a = new Reader::ReadRangeAction(keys, rowLimit, byteLimit);
			auto res = a->result.getFuture();
			readThreads->post(a);
			return res;
		}

		auto& semaphore = (type == ReadType::FETCH) ? fetchSemaphore : readSemaphore;
		int maxWaiters = (type == ReadType::FETCH) ? numFetchWaiters : numReadWaiters;

		checkWaiters(semaphore, maxWaiters);
		auto a = std::make_unique<Reader::ReadRangeAction>(keys, rowLimit, byteLimit);
		return read(a.release(), &semaphore, readThreads.getPtr(), &counters.failedToAcquire);
	}

	StorageBytes getStorageBytes() const override {
		uint64_t live = 0;
		ASSERT(db->GetAggregatedIntProperty(rocksdb::DB::Properties::kLiveSstFilesSize, &live));

		int64_t free;
		int64_t total;
		g_network->getDiskBytes(path, free, total);

		return StorageBytes(free, total, live, free);
	}

	Future<CheckpointMetaData> checkpoint(const CheckpointRequest& request) override {
		auto a = new Writer::CheckpointAction(request);

		auto res = a->reply.getFuture();
		writeThread->post(a);
		return res;
	}

	Future<Void> restore(const std::vector<CheckpointMetaData>& checkpoints) override {
		auto a = new Writer::RestoreAction(path, checkpoints);
		auto res = a->done.getFuture();
		writeThread->post(a);
		return res;
	}

	// Delete a checkpoint.
	Future<Void> deleteCheckpoint(const CheckpointMetaData& checkpoint) override {
		if (checkpoint.format == RocksDBColumnFamily) {
			RocksDBColumnFamilyCheckpoint rocksCF;
			ObjectReader reader(checkpoint.serializedCheckpoint.begin(), IncludeVersion());
			reader.deserialize(rocksCF);

			std::unordered_set<std::string> dirs;
			for (const LiveFileMetaData& file : rocksCF.sstFiles) {
				dirs.insert(file.db_path);
			}
			for (const std::string dir : dirs) {
				platform::eraseDirectoryRecursive(dir);
				TraceEvent("DeleteCheckpointRemovedDir", id)
				    .detail("CheckpointID", checkpoint.checkpointID)
				    .detail("Dir", dir);
			}
		} else if (checkpoint.format == RocksDB) {
			throw not_implemented();
		} else {
			throw internal_error();
		}
		return Void();
	}

	DB db = nullptr;
	std::shared_ptr<SharedRocksDBState> sharedState;
	std::shared_ptr<PerfContextMetrics> perfContextMetrics;
	std::string path;
	rocksdb::ColumnFamilyHandle* defaultFdbCF = nullptr;
	UID id;
	Reference<IThreadPool> writeThread;
	Reference<IThreadPool> readThreads;
	std::shared_ptr<RocksDBErrorListener> errorListener;
	Future<Void> errorFuture;
	Promise<Void> closePromise;
	Future<Void> openFuture;
	std::unique_ptr<rocksdb::WriteBatch> writeBatch;
	Optional<Future<Void>> metrics;
	FlowLock readSemaphore;
	int numReadWaiters;
	FlowLock fetchSemaphore;
	int numFetchWaiters;
	std::shared_ptr<ReadIteratorPool> readIterPool;
	std::vector<std::unique_ptr<ThreadReturnPromiseStream<std::pair<std::string, double>>>> metricPromiseStreams;
	// ThreadReturnPromiseStream pair.first stores the histogram name and
	// pair.second stores the corresponding measured latency (seconds)
	Future<Void> actorErrorListener;
	Future<Void> collection;
	PromiseStream<Future<Void>> addActor;
	Counters counters;
};

void RocksDBKeyValueStore::Writer::action(CheckpointAction& a) {
	TraceEvent("RocksDBServeCheckpointBegin", id)
	    .detail("MinVersion", a.request.version)
	    .detail("Range", a.request.range.toString())
	    .detail("Format", static_cast<int>(a.request.format))
	    .detail("CheckpointDir", a.request.checkpointDir);

	rocksdb::Checkpoint* checkpoint = nullptr;
	rocksdb::Status s = rocksdb::Checkpoint::Create(db, &checkpoint);
	if (!s.ok()) {
		logRocksDBError(id, s, "Checkpoint");
		a.reply.sendError(statusToError(s));
		return;
	}

	rocksdb::PinnableSlice value;
	rocksdb::ReadOptions& readOptions = sharedState->getReadOptions();
	s = db->Get(readOptions, cf, toSlice(persistVersion), &value);

	if (!s.ok() && !s.IsNotFound()) {
		logRocksDBError(id, s, "Checkpoint");
		a.reply.sendError(statusToError(s));
		return;
	}

	const Version version =
	    s.IsNotFound() ? latestVersion : BinaryReader::fromStringRef<Version>(toStringRef(value), Unversioned());

	ASSERT(a.request.version == version || a.request.version == latestVersion);
	TraceEvent(SevDebug, "RocksDBServeCheckpointVersion", id)
	    .detail("CheckpointVersion", a.request.version)
	    .detail("PersistVersion", version);

	// TODO: set the range as the actual shard range.
	CheckpointMetaData res(version, a.request.range, a.request.format, a.request.checkpointID);
	const std::string& checkpointDir = abspath(a.request.checkpointDir);

	if (a.request.format == RocksDBColumnFamily) {
		rocksdb::ExportImportFilesMetaData* pMetadata;
		platform::eraseDirectoryRecursive(checkpointDir);
		s = checkpoint->ExportColumnFamily(cf, checkpointDir, &pMetadata);
		if (!s.ok()) {
			logRocksDBError(id, s, "ExportColumnFamily");
			a.reply.sendError(statusToError(s));
			return;
		}

		populateMetaData(&res, *pMetadata);
		delete pMetadata;
		TraceEvent("RocksDBServeCheckpointSuccess", id)
		    .detail("CheckpointMetaData", res.toString())
		    .detail("RocksDBCF", getRocksCF(res).toString());
	} else if (a.request.format == RocksDB) {
		platform::eraseDirectoryRecursive(checkpointDir);
		uint64_t debugCheckpointSeq = -1;
		s = checkpoint->CreateCheckpoint(checkpointDir, /*log_size_for_flush=*/0, &debugCheckpointSeq);
		if (!s.ok()) {
			logRocksDBError(id, s, "Checkpoint");
			a.reply.sendError(statusToError(s));
			return;
		}

		RocksDBCheckpoint rcp;
		rcp.checkpointDir = checkpointDir;
		rcp.sstFiles = platform::listFiles(checkpointDir, ".sst");
		res.serializedCheckpoint = ObjectWriter::toValue(rcp, IncludeVersion());
		TraceEvent("RocksDBCheckpointCreated", id)
		    .detail("CheckpointVersion", a.request.version)
		    .detail("RocksSequenceNumber", debugCheckpointSeq)
		    .detail("CheckpointDir", checkpointDir);
	} else {
		if (checkpoint != nullptr) {
			delete checkpoint;
		}
		throw not_implemented();
	}

	if (checkpoint != nullptr) {
		delete checkpoint;
	}
	res.setState(CheckpointMetaData::Complete);
	a.reply.send(res);
}

void RocksDBKeyValueStore::Writer::action(RestoreAction& a) {
	TraceEvent("RocksDBRestoreBegin", id).detail("Path", a.path).detail("Checkpoints", describe(a.checkpoints));

	ASSERT(db != nullptr);
	ASSERT(!a.checkpoints.empty());

	const CheckpointFormat format = a.checkpoints[0].getFormat();
	for (int i = 1; i < a.checkpoints.size(); ++i) {
		if (a.checkpoints[i].getFormat() != format) {
			throw invalid_checkpoint_format();
		}
	}

	rocksdb::Status status;
	if (format == RocksDBColumnFamily) {
		ASSERT_EQ(a.checkpoints.size(), 1);
		TraceEvent("RocksDBServeRestoreCF", id)
		    .detail("Path", a.path)
		    .detail("Checkpoint", a.checkpoints[0].toString())
		    .detail("RocksDBCF", getRocksCF(a.checkpoints[0]).toString());

		if (cf != nullptr) {
			ASSERT(db->DropColumnFamily(cf).ok());
			db->DestroyColumnFamilyHandle(cf);
			cfHandles.erase(cf);
		}

		rocksdb::ExportImportFilesMetaData metaData = getMetaData(a.checkpoints[0]);
		rocksdb::ImportColumnFamilyOptions importOptions;
		importOptions.move_files = true;
		status = db->CreateColumnFamilyWithImport(
		    sharedState->getCfOptions(), SERVER_KNOBS->DEFAULT_FDB_ROCKSDB_COLUMN_FAMILY, importOptions, metaData, &cf);
		cfHandles.insert(cf);

		if (!status.ok()) {
			logRocksDBError(id, status, "Restore");
			a.done.sendError(statusToError(status));
		} else {
			TraceEvent(SevInfo, "RocksDBRestoreCFSuccess", id)
			    .detail("Path", a.path)
			    .detail("Checkpoint", a.checkpoints[0].toString());
			a.done.send(Void());
		}
	} else if (format == RocksDB) {
		if (cf == nullptr) {
			status = db->CreateColumnFamily(
			    sharedState->getCfOptions(), SERVER_KNOBS->DEFAULT_FDB_ROCKSDB_COLUMN_FAMILY, &cf);
			cfHandles.insert(cf);
			TraceEvent("RocksDBServeRestoreRange", id)
			    .detail("Path", a.path)
			    .detail("Checkpoint", describe(a.checkpoints));
			if (!status.ok()) {
				logRocksDBError(id, status, "CreateColumnFamily");
				a.done.sendError(statusToError(status));
				return;
			}
		}

		std::vector<std::string> sstFiles;
		for (const auto& checkpoint : a.checkpoints) {
			const RocksDBCheckpoint rocksCheckpoint = getRocksCheckpoint(checkpoint);
			for (const auto& file : rocksCheckpoint.fetchedFiles) {
				TraceEvent("RocksDBRestoreFile", id)
				    .detail("Checkpoint", rocksCheckpoint.toString())
				    .detail("File", file.toString());
				sstFiles.push_back(file.path);
			}
		}

		if (!sstFiles.empty()) {
			rocksdb::IngestExternalFileOptions ingestOptions;
			ingestOptions.move_files = true;
			ingestOptions.write_global_seqno = false;
			ingestOptions.verify_checksums_before_ingest = true;
			status = db->IngestExternalFile(cf, sstFiles, ingestOptions);
			if (!status.ok()) {
				logRocksDBError(id, status, "IngestExternalFile", SevWarnAlways);
				a.done.sendError(statusToError(status));
				return;
			}
		} else {
			TraceEvent(SevDebug, "RocksDBServeRestoreEmptyRange", id)
			    .detail("Path", a.path)
			    .detail("Checkpoint", describe(a.checkpoints));
		}
		TraceEvent("RocksDBServeRestoreEnd", id).detail("Path", a.path).detail("Checkpoint", describe(a.checkpoints));
		a.done.send(Void());
	} else {
		throw not_implemented();
	}
}

} // namespace

#endif // SSD_ROCKSDB_EXPERIMENTAL

IKeyValueStore* keyValueStoreRocksDB(std::string const& path,
                                     UID logID,
                                     KeyValueStoreType storeType,
                                     bool checkChecksums,
                                     bool checkIntegrity) {
#ifdef SSD_ROCKSDB_EXPERIMENTAL
	return new RocksDBKeyValueStore(path, logID);
#else
	TraceEvent(SevError, "RocksDBEngineInitFailure", logID).detail("Reason", "Built without RocksDB");
	ASSERT(false);
	return nullptr;
#endif // SSD_ROCKSDB_EXPERIMENTAL
}

#ifdef SSD_ROCKSDB_EXPERIMENTAL
#include "flow/UnitTest.h"

namespace {

TEST_CASE("noSim/fdbserver/KeyValueStoreRocksDB/RocksDBBasic") {
	state const std::string rocksDBTestDir = "rocksdb-kvstore-basic-test-db";
	platform::eraseDirectoryRecursive(rocksDBTestDir);

	state IKeyValueStore* kvStore = new RocksDBKeyValueStore(rocksDBTestDir, deterministicRandom()->randomUniqueID());
	wait(kvStore->init());

	state StringRef foo = "foo"_sr;
	state StringRef bar = "ibar"_sr;
	kvStore->set({ foo, foo });
	kvStore->set({ keyAfter(foo), keyAfter(foo) });
	kvStore->set({ bar, bar });
	kvStore->set({ keyAfter(bar), keyAfter(bar) });
	wait(kvStore->commit(false));

	{
		Optional<Value> val = wait(kvStore->readValue(foo));
		ASSERT(foo == val.get());
	}

	// Test single key deletion.
	kvStore->clear(singleKeyRange(foo));
	wait(kvStore->commit(false));

	{
		Optional<Value> val = wait(kvStore->readValue(foo));
		ASSERT(!val.present());
	}

	{
		Optional<Value> val = wait(kvStore->readValue(keyAfter(foo)));
		ASSERT(keyAfter(foo) == val.get());
	}

	// Test range deletion.
	kvStore->clear(KeyRangeRef(keyAfter(foo), keyAfter(bar)));
	wait(kvStore->commit(false));

	{
		Optional<Value> val = wait(kvStore->readValue(bar));
		ASSERT(!val.present());
	}

	{
		Optional<Value> val = wait(kvStore->readValue(keyAfter(bar)));
		ASSERT(keyAfter(bar) == val.get());
	}

	Future<Void> closed = kvStore->onClosed();
	kvStore->dispose();
	wait(closed);

	platform::eraseDirectoryRecursive(rocksDBTestDir);
	return Void();
}

TEST_CASE("noSim/fdbserver/KeyValueStoreRocksDB/RocksDBReopen") {
	state const std::string rocksDBTestDir = "rocksdb-kvstore-reopen-test-db";
	platform::eraseDirectoryRecursive(rocksDBTestDir);

	state IKeyValueStore* kvStore = new RocksDBKeyValueStore(rocksDBTestDir, deterministicRandom()->randomUniqueID());
	wait(kvStore->init());

	kvStore->set({ "foo"_sr, "bar"_sr });
	wait(kvStore->commit(false));

	Optional<Value> val = wait(kvStore->readValue("foo"_sr));
	ASSERT(Optional<Value>("bar"_sr) == val);

	Future<Void> closed = kvStore->onClosed();
	kvStore->close();
	wait(closed);

	kvStore = new RocksDBKeyValueStore(rocksDBTestDir, deterministicRandom()->randomUniqueID());
	wait(kvStore->init());
	// Confirm that `init()` is idempotent.
	wait(kvStore->init());

	Optional<Value> val = wait(kvStore->readValue("foo"_sr));
	ASSERT(Optional<Value>("bar"_sr) == val);

	Future<Void> closed = kvStore->onClosed();
	kvStore->dispose();
	wait(closed);

	platform::eraseDirectoryRecursive(rocksDBTestDir);
	return Void();
}

TEST_CASE("noSim/fdbserver/KeyValueStoreRocksDB/CheckpointRestoreColumnFamily") {
	state std::string cwd = platform::getWorkingDirectory() + "/";
	state std::string rocksDBTestDir = "rocksdb-kvstore-br-test-db";
	platform::eraseDirectoryRecursive(rocksDBTestDir);

	state IKeyValueStore* kvStore = new RocksDBKeyValueStore(rocksDBTestDir, deterministicRandom()->randomUniqueID());
	wait(kvStore->init());

	kvStore->set({ "foo"_sr, "bar"_sr });
	wait(kvStore->commit(false));

	Optional<Value> val = wait(kvStore->readValue("foo"_sr));
	ASSERT(Optional<Value>("bar"_sr) == val);

	state std::string rocksDBRestoreDir = "rocksdb-kvstore-br-restore-db";
	platform::eraseDirectoryRecursive(rocksDBRestoreDir);

	state IKeyValueStore* kvStoreCopy =
	    new RocksDBKeyValueStore(rocksDBRestoreDir, deterministicRandom()->randomUniqueID());
	wait(kvStoreCopy->init());

	platform::eraseDirectoryRecursive("checkpoint");
	state std::string checkpointDir = cwd + "checkpoint";

	CheckpointRequest request(
	    latestVersion, allKeys, RocksDBColumnFamily, deterministicRandom()->randomUniqueID(), checkpointDir);
	CheckpointMetaData metaData = wait(kvStore->checkpoint(request));

	std::vector<CheckpointMetaData> checkpoints;
	checkpoints.push_back(metaData);
	wait(kvStoreCopy->restore(checkpoints));

	Optional<Value> val = wait(kvStoreCopy->readValue("foo"_sr));
	ASSERT(Optional<Value>("bar"_sr) == val);

	std::vector<Future<Void>> closes;
	closes.push_back(kvStore->onClosed());
	closes.push_back(kvStoreCopy->onClosed());
	kvStore->dispose();
	kvStoreCopy->dispose();
	wait(waitForAll(closes));

	platform::eraseDirectoryRecursive(rocksDBTestDir);
	platform::eraseDirectoryRecursive(rocksDBRestoreDir);

	return Void();
}

TEST_CASE("noSim/fdbserver/KeyValueStoreRocksDB/CheckpointRestoreKeyValues") {
	state std::string cwd = platform::getWorkingDirectory() + "/";
	state std::string rocksDBTestDir = "rocksdb-kvstore-brsst-test-db";
	platform::eraseDirectoryRecursive(rocksDBTestDir);
	state IKeyValueStore* kvStore = new RocksDBKeyValueStore(rocksDBTestDir, deterministicRandom()->randomUniqueID());
	wait(kvStore->init());

	kvStore->set({ "foo"_sr, "bar"_sr });
	wait(kvStore->commit(false));
	Optional<Value> val = wait(kvStore->readValue("foo"_sr));
	ASSERT(Optional<Value>("bar"_sr) == val);

	platform::eraseDirectoryRecursive("checkpoint");
	std::string checkpointDir = cwd + "checkpoint";

	CheckpointRequest request(latestVersion, allKeys, RocksDB, deterministicRandom()->randomUniqueID(), checkpointDir);
	CheckpointMetaData metaData = wait(kvStore->checkpoint(request));

	state ICheckpointReader* cpReader = newCheckpointReader(metaData, deterministicRandom()->randomUniqueID());
	wait(cpReader->init(BinaryWriter::toValue(KeyRangeRef("foo"_sr, "foobar"_sr), IncludeVersion())));
	loop {
		try {
			state RangeResult res =
			    wait(cpReader->nextKeyValues(CLIENT_KNOBS->REPLY_BYTE_LIMIT, CLIENT_KNOBS->REPLY_BYTE_LIMIT));
			state int i = 0;
			for (; i < res.size(); ++i) {
				Optional<Value> val = wait(kvStore->readValue(res[i].key));
				ASSERT(val.present() && val.get() == res[i].value);
			}
		} catch (Error& e) {
			if (e.code() == error_code_end_of_stream) {
				break;
			} else {
				TraceEvent(SevError, "TestFailed").error(e);
			}
		}
	}

	std::vector<Future<Void>> closes;
	closes.push_back(cpReader->close());
	closes.push_back(kvStore->onClosed());
	kvStore->dispose();
	wait(waitForAll(closes));

	platform::eraseDirectoryRecursive(rocksDBTestDir);

	return Void();
}

} // namespace

#endif // SSD_ROCKSDB_EXPERIMENTAL
