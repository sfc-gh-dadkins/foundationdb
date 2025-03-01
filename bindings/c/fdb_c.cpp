/*
 * fdb_c.cpp
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

#include "fdbclient/FDBTypes.h"
#include "flow/ProtocolVersion.h"
#include <cstdint>
#define FDB_API_VERSION 730
#define FDB_INCLUDE_LEGACY_TYPES

#include "fdbclient/MultiVersionTransaction.h"
#include "fdbclient/MultiVersionAssignmentVars.h"
#include "foundationdb/fdb_c.h"
#include "foundationdb/fdb_c_internal.h"

int g_api_version = 0;

/*
 * Our clients might share these ThreadSafe types between threads. It is therefore
 * unsafe to call addRef on them.
 *
 * type mapping:
 *   FDBFuture -> ThreadSingleAssignmentVarBase
 *   FDBResult -> ThreadSingleAssignmentVarBase
 *   FDBDatabase -> IDatabase
 *   FDBTenant -> ITenant
 *   FDBTransaction -> ITransaction
 */
#define TSAVB(f) ((ThreadSingleAssignmentVarBase*)(f))
#define TSAV(T, f) ((ThreadSingleAssignmentVar<T>*)(f))

#define DB(d) ((IDatabase*)d)
#define TENANT(t) ((ITenant*)t)
#define TXN(t) ((ITransaction*)t)

// Legacy (pre API version 610)
#define CLUSTER(c) ((char*)c)

/*
 * While we could just use the MultiVersionApi instance directly, this #define allows us to swap in any other IClientApi
 * instance (e.g. from ThreadSafeApi)
 */
#define API ((IClientApi*)MultiVersionApi::api)

/* This must be true so that we can return the data pointer of a
   Standalone<RangeResultRef> as an array of FDBKeyValue. */
static_assert(sizeof(FDBKeyValue) == sizeof(KeyValueRef), "FDBKeyValue / KeyValueRef size mismatch");

#define TSAV_ERROR(type, error) ((FDBFuture*)(ThreadFuture<type>(error())).extractPtr())

extern "C" DLLEXPORT const char* fdb_get_error(fdb_error_t code) {
	return Error::fromUnvalidatedCode(code).what();
}

extern "C" DLLEXPORT fdb_bool_t fdb_error_predicate(int predicate_test, fdb_error_t code) {
	if (predicate_test == FDBErrorPredicates::RETRYABLE) {
		return fdb_error_predicate(FDBErrorPredicates::MAYBE_COMMITTED, code) ||
		       fdb_error_predicate(FDBErrorPredicates::RETRYABLE_NOT_COMMITTED, code);
	}
	if (predicate_test == FDBErrorPredicates::MAYBE_COMMITTED) {
		return code == error_code_commit_unknown_result || code == error_code_cluster_version_changed;
	}
	if (predicate_test == FDBErrorPredicates::RETRYABLE_NOT_COMMITTED) {
		return code == error_code_not_committed || code == error_code_transaction_too_old ||
		       code == error_code_future_version || code == error_code_database_locked ||
		       code == error_code_grv_proxy_memory_limit_exceeded ||
		       code == error_code_commit_proxy_memory_limit_exceeded ||
		       code == error_code_batch_transaction_throttled || code == error_code_process_behind ||
		       code == error_code_tag_throttled || code == error_code_unknown_tenant;
	}
	return false;
}

#define RETURN_FUTURE_ON_ERROR(return_type, code_to_run)                                                               \
	try {                                                                                                              \
		code_to_run                                                                                                    \
	} catch (Error & e) {                                                                                              \
		if (e.code() <= 0)                                                                                             \
			return ((FDBFuture*)(ThreadFuture<return_type>(internal_error())).extractPtr());                           \
		else                                                                                                           \
			return ((FDBFuture*)(ThreadFuture<return_type>(e)).extractPtr());                                          \
	} catch (...) {                                                                                                    \
		return ((FDBFuture*)(ThreadFuture<return_type>(unknown_error())).extractPtr());                                \
	}

#define RETURN_RESULT_ON_ERROR(return_type, code_to_run)                                                               \
	try {                                                                                                              \
		code_to_run                                                                                                    \
	} catch (Error & e) {                                                                                              \
		if (e.code() <= 0)                                                                                             \
			return ((FDBResult*)(ThreadResult<return_type>(internal_error())).extractPtr());                           \
		else                                                                                                           \
			return ((FDBResult*)(ThreadResult<return_type>(e)).extractPtr());                                          \
	} catch (...) {                                                                                                    \
		return ((FDBResult*)(ThreadResult<return_type>(unknown_error())).extractPtr());                                \
	}

#define RETURN_ON_ERROR(code_to_run)                                                                                   \
	try {                                                                                                              \
		code_to_run                                                                                                    \
	} catch (Error & e) {                                                                                              \
		if (e.code() <= 0)                                                                                             \
			return internal_error().code();                                                                            \
		else                                                                                                           \
			return e.code();                                                                                           \
	} catch (...) {                                                                                                    \
		return error_code_unknown_error;                                                                               \
	}

#define CATCH_AND_RETURN(code_to_run)                                                                                  \
	RETURN_ON_ERROR(code_to_run);                                                                                      \
	return error_code_success;

#define CATCH_AND_DIE(code_to_run)                                                                                     \
	try {                                                                                                              \
		code_to_run                                                                                                    \
	} catch (Error & e) {                                                                                              \
		fprintf(stderr, "Unexpected FDB error %d\n", e.code());                                                        \
		abort();                                                                                                       \
	} catch (...) {                                                                                                    \
		fprintf(stderr, "Unexpected FDB unknown error\n");                                                             \
		abort();                                                                                                       \
	}

extern "C" DLLEXPORT fdb_error_t fdb_network_set_option(FDBNetworkOption option,
                                                        uint8_t const* value,
                                                        int value_length) {
	CATCH_AND_RETURN(API->setNetworkOption((FDBNetworkOptions::Option)option,
	                                       value ? StringRef(value, value_length) : Optional<StringRef>()););
}

fdb_error_t fdb_setup_network_impl() {
	CATCH_AND_RETURN(API->setupNetwork(););
}

fdb_error_t fdb_setup_network_v13(const char* localAddress) {
	fdb_error_t errorCode =
	    fdb_network_set_option(FDB_NET_OPTION_LOCAL_ADDRESS, (uint8_t const*)localAddress, strlen(localAddress));
	if (errorCode != 0)
		return errorCode;

	return fdb_setup_network_impl();
}

extern "C" DLLEXPORT fdb_error_t fdb_run_network() {
	CATCH_AND_RETURN(API->runNetwork(););
}

extern "C" DLLEXPORT fdb_error_t fdb_stop_network() {
	CATCH_AND_RETURN(API->stopNetwork(););
}

extern "C" DLLEXPORT fdb_error_t fdb_add_network_thread_completion_hook(void (*hook)(void*), void* hook_parameter) {
	CATCH_AND_RETURN(API->addNetworkThreadCompletionHook(hook, hook_parameter););
}

extern "C" DLLEXPORT void fdb_future_cancel(FDBFuture* f) {
	CATCH_AND_DIE(TSAVB(f)->addref(); TSAVB(f)->cancel(););
}

extern "C" DLLEXPORT void fdb_future_release_memory(FDBFuture* f) {
	CATCH_AND_DIE(TSAVB(f)->releaseMemory(););
}

extern "C" DLLEXPORT void fdb_future_destroy(FDBFuture* f) {
	CATCH_AND_DIE(TSAVB(f)->cancel(););
}

extern "C" DLLEXPORT fdb_error_t fdb_future_block_until_ready(FDBFuture* f) {
	CATCH_AND_RETURN(TSAVB(f)->blockUntilReadyCheckOnMainThread(););
}

fdb_bool_t fdb_future_is_error_v22(FDBFuture* f) {
	return TSAVB(f)->isError();
}

extern "C" DLLEXPORT fdb_bool_t fdb_future_is_ready(FDBFuture* f) {
	return TSAVB(f)->isReady();
}

class CAPICallback final : public ThreadCallback {
public:
	CAPICallback(void (*callbackf)(FDBFuture*, void*), FDBFuture* f, void* userdata)
	  : callbackf(callbackf), f(f), userdata(userdata) {}

	bool canFire(int notMadeActive) const override { return true; }
	void fire(const Void& unused, int& userParam) override {
		(*callbackf)(f, userdata);
		delete this;
	}
	void error(const Error&, int& userParam) override {
		(*callbackf)(f, userdata);
		delete this;
	}

private:
	void (*callbackf)(FDBFuture*, void*);
	FDBFuture* f;
	void* userdata;
};

extern "C" DLLEXPORT fdb_error_t fdb_future_set_callback(FDBFuture* f,
                                                         void (*callbackf)(FDBFuture*, void*),
                                                         void* userdata) {
	CAPICallback* cb = new CAPICallback(callbackf, f, userdata);
	int ignore;
	CATCH_AND_RETURN(TSAVB(f)->callOrSetAsCallback(cb, ignore, 0););
}

fdb_error_t fdb_future_get_error_impl(FDBFuture* f) {
	return TSAVB(f)->getErrorCode();
}

fdb_error_t fdb_future_get_error_v22(FDBFuture* f, const char** description) {
	if (!(TSAVB(f)->isError()))
		return error_code_future_not_error;
	if (description)
		*description = TSAVB(f)->error.what();
	return TSAVB(f)->error.code();
}

fdb_error_t fdb_future_get_version_v619(FDBFuture* f, int64_t* out_version) {
	CATCH_AND_RETURN(*out_version = TSAV(Version, f)->get(););
}

extern "C" DLLEXPORT fdb_error_t fdb_future_get_bool(FDBFuture* f, fdb_bool_t* out_value) {
	CATCH_AND_RETURN(*out_value = TSAV(bool, f)->get(););
}

extern "C" DLLEXPORT fdb_error_t fdb_future_get_int64(FDBFuture* f, int64_t* out_value) {
	CATCH_AND_RETURN(*out_value = TSAV(int64_t, f)->get(););
}

extern "C" DLLEXPORT fdb_error_t fdb_future_get_uint64(FDBFuture* f, uint64_t* out) {
	CATCH_AND_RETURN(*out = TSAV(uint64_t, f)->get(););
}

extern "C" DLLEXPORT fdb_error_t fdb_future_get_key(FDBFuture* f, uint8_t const** out_key, int* out_key_length) {
	CATCH_AND_RETURN(KeyRef key = TSAV(Key, f)->get(); *out_key = key.begin(); *out_key_length = key.size(););
}

fdb_error_t fdb_future_get_cluster_v609(FDBFuture* f, FDBCluster** out_cluster) {
	CATCH_AND_RETURN(*out_cluster = (FDBCluster*)((TSAV(char*, f)->get())););
}

fdb_error_t fdb_future_get_database_v609(FDBFuture* f, FDBDatabase** out_database) {
	CATCH_AND_RETURN(*out_database = (FDBDatabase*)((TSAV(Reference<IDatabase>, f)->get()).extractPtr()););
}

extern "C" DLLEXPORT fdb_error_t fdb_future_get_value(FDBFuture* f,
                                                      fdb_bool_t* out_present,
                                                      uint8_t const** out_value,
                                                      int* out_value_length) {
	CATCH_AND_RETURN(Optional<Value> v = TSAV(Optional<Value>, f)->get(); *out_present = v.present();
	                 if (*out_present) {
		                 *out_value = v.get().begin();
		                 *out_value_length = v.get().size();
	                 });
}

fdb_error_t fdb_future_get_keyvalue_array_impl(FDBFuture* f,
                                               FDBKeyValue const** out_kv,
                                               int* out_count,
                                               fdb_bool_t* out_more) {
	CATCH_AND_RETURN(Standalone<RangeResultRef> rrr = TSAV(Standalone<RangeResultRef>, f)->get();
	                 *out_kv = (FDBKeyValue*)rrr.begin();
	                 *out_count = rrr.size();
	                 *out_more = rrr.more;);
}

fdb_error_t fdb_future_get_keyvalue_array_v13(FDBFuture* f, FDBKeyValue const** out_kv, int* out_count) {
	CATCH_AND_RETURN(Standalone<RangeResultRef> rrr = TSAV(Standalone<RangeResultRef>, f)->get();
	                 *out_kv = (FDBKeyValue*)rrr.begin();
	                 *out_count = rrr.size(););
}

extern "C" DLLEXPORT fdb_error_t fdb_future_get_mappedkeyvalue_array(FDBFuture* f,
                                                                     FDBMappedKeyValue const** out_kvm,
                                                                     int* out_count,
                                                                     fdb_bool_t* out_more) {
	CATCH_AND_RETURN(Standalone<MappedRangeResultRef> rrr = TSAV(Standalone<MappedRangeResultRef>, f)->get();
	                 *out_kvm = (FDBMappedKeyValue*)rrr.begin();
	                 *out_count = rrr.size();
	                 *out_more = rrr.more;);
}

extern "C" DLLEXPORT fdb_error_t fdb_future_get_shared_state(FDBFuture* f, DatabaseSharedState** outPtr) {
	CATCH_AND_RETURN(*outPtr = (DatabaseSharedState*)((TSAV(DatabaseSharedState*, f)->get())););
}

extern "C" DLLEXPORT fdb_error_t fdb_future_get_string_array(FDBFuture* f, const char*** out_strings, int* out_count) {
	CATCH_AND_RETURN(Standalone<VectorRef<const char*>> na = TSAV(Standalone<VectorRef<const char*>>, f)->get();
	                 *out_strings = (const char**)na.begin();
	                 *out_count = na.size(););
}

extern "C" DLLEXPORT fdb_error_t fdb_future_get_keyrange_array(FDBFuture* f,
                                                               FDBKeyRange const** out_ranges,
                                                               int* out_count) {
	CATCH_AND_RETURN(Standalone<VectorRef<KeyRangeRef>> na = TSAV(Standalone<VectorRef<KeyRangeRef>>, f)->get();
	                 *out_ranges = (FDBKeyRange*)na.begin();
	                 *out_count = na.size(););
}

extern "C" DLLEXPORT fdb_error_t fdb_future_get_key_array(FDBFuture* f, FDBKey const** out_key_array, int* out_count) {
	CATCH_AND_RETURN(Standalone<VectorRef<KeyRef>> na = TSAV(Standalone<VectorRef<KeyRef>>, f)->get();
	                 *out_key_array = (FDBKey*)na.begin();
	                 *out_count = na.size(););
}

extern "C" DLLEXPORT fdb_error_t fdb_future_get_granule_summary_array(FDBFuture* f,
                                                                      FDBGranuleSummary const** out_ranges,
                                                                      int* out_count) {
	CATCH_AND_RETURN(Standalone<VectorRef<BlobGranuleSummaryRef>> na =
	                     TSAV(Standalone<VectorRef<BlobGranuleSummaryRef>>, f)->get();
	                 *out_ranges = (FDBGranuleSummary*)na.begin();
	                 *out_count = na.size(););
}

extern "C" DLLEXPORT void fdb_result_destroy(FDBResult* r) {
	CATCH_AND_DIE(TSAVB(r)->cancel(););
}

fdb_error_t fdb_result_get_keyvalue_array(FDBResult* r,
                                          FDBKeyValue const** out_kv,
                                          int* out_count,
                                          fdb_bool_t* out_more) {
	CATCH_AND_RETURN(RangeResult rr = TSAV(RangeResult, r)->get(); *out_kv = (FDBKeyValue*)rr.begin();
	                 *out_count = rr.size();
	                 *out_more = rr.more;);
}

FDBFuture* fdb_create_cluster_v609(const char* cluster_file_path) {
	char* path;
	if (cluster_file_path) {
		path = new char[strlen(cluster_file_path) + 1];
		strcpy(path, cluster_file_path);
	} else {
		path = new char[1];
		path[0] = '\0';
	}
	return (FDBFuture*)ThreadFuture<char*>(path).extractPtr();
}

fdb_error_t fdb_cluster_set_option_v609(FDBCluster* c,
                                        FDBClusterOption option,
                                        uint8_t const* value,
                                        int value_length) {
	// There are no cluster options
	return error_code_success;
}

void fdb_cluster_destroy_v609(FDBCluster* c) {
	CATCH_AND_DIE(delete[] CLUSTER(c););
}

// This exists so that fdb_cluster_create_database doesn't need to call the public symbol fdb_create_database.
// If it does and this is an external client loaded though the multi-version API, then it may inadvertently call
// the version of the function in the primary library if it was loaded into the global symbols.
fdb_error_t fdb_create_database_impl(const char* cluster_file_path, FDBDatabase** out_database) {
	CATCH_AND_RETURN(*out_database =
	                     (FDBDatabase*)API->createDatabase(cluster_file_path ? cluster_file_path : "").extractPtr(););
}

FDBFuture* fdb_cluster_create_database_v609(FDBCluster* c, uint8_t const* db_name, int db_name_length) {
	if (strncmp((const char*)db_name, "DB", db_name_length) != 0) {
		return (FDBFuture*)ThreadFuture<Reference<IDatabase>>(invalid_database_name()).extractPtr();
	}

	FDBDatabase* db;
	fdb_error_t err = fdb_create_database_impl(CLUSTER(c), &db);
	if (err) {
		return (FDBFuture*)ThreadFuture<Reference<IDatabase>>(Error(err)).extractPtr();
	}

	return (FDBFuture*)ThreadFuture<Reference<IDatabase>>(Reference<IDatabase>(DB(db))).extractPtr();
}

extern "C" DLLEXPORT fdb_error_t fdb_create_database(const char* cluster_file_path, FDBDatabase** out_database) {
	return fdb_create_database_impl(cluster_file_path, out_database);
}

extern "C" DLLEXPORT fdb_error_t fdb_create_database_from_connection_string(const char* connection_string,
                                                                            FDBDatabase** out_database) {
	CATCH_AND_RETURN(*out_database =
	                     (FDBDatabase*)API->createDatabaseFromConnectionString(connection_string).extractPtr(););
}

extern "C" DLLEXPORT fdb_error_t fdb_database_set_option(FDBDatabase* d,
                                                         FDBDatabaseOption option,
                                                         uint8_t const* value,
                                                         int value_length) {
	CATCH_AND_RETURN(DB(d)->setOption((FDBDatabaseOptions::Option)option,
	                                  value ? StringRef(value, value_length) : Optional<StringRef>()););
}

extern "C" DLLEXPORT void fdb_database_destroy(FDBDatabase* d) {
	CATCH_AND_DIE(DB(d)->delref(););
}

extern "C" DLLEXPORT fdb_error_t fdb_database_open_tenant(FDBDatabase* d,
                                                          uint8_t const* tenant_name,
                                                          int tenant_name_length,
                                                          FDBTenant** out_tenant) {
	CATCH_AND_RETURN(*out_tenant =
	                     (FDBTenant*)DB(d)->openTenant(TenantNameRef(tenant_name, tenant_name_length)).extractPtr(););
}

extern "C" DLLEXPORT fdb_error_t fdb_database_create_transaction(FDBDatabase* d, FDBTransaction** out_transaction) {
	CATCH_AND_RETURN(Reference<ITransaction> tr = DB(d)->createTransaction();
	                 if (g_api_version <= 15) tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
	                 *out_transaction = (FDBTransaction*)tr.extractPtr(););
}

extern "C" DLLEXPORT FDBFuture* fdb_database_reboot_worker(FDBDatabase* db,
                                                           uint8_t const* address,
                                                           int address_length,
                                                           fdb_bool_t check,
                                                           int duration) {
	return (FDBFuture*)(DB(db)->rebootWorker(StringRef(address, address_length), check, duration).extractPtr());
}

extern "C" DLLEXPORT FDBFuture* fdb_database_force_recovery_with_data_loss(FDBDatabase* db,
                                                                           uint8_t const* dcid,
                                                                           int dcid_length) {
	return (FDBFuture*)(DB(db)->forceRecoveryWithDataLoss(StringRef(dcid, dcid_length)).extractPtr());
}

extern "C" DLLEXPORT FDBFuture* fdb_database_create_snapshot(FDBDatabase* db,
                                                             uint8_t const* uid,
                                                             int uid_length,
                                                             uint8_t const* snap_command,
                                                             int snap_command_length) {
	return (FDBFuture*)(DB(db)
	                        ->createSnapshot(StringRef(uid, uid_length), StringRef(snap_command, snap_command_length))
	                        .extractPtr());
}

extern "C" DLLEXPORT FDBFuture* fdb_database_create_shared_state(FDBDatabase* db) {
	return (FDBFuture*)(DB(db)->createSharedState().extractPtr());
}

extern "C" DLLEXPORT void fdb_database_set_shared_state(FDBDatabase* db, DatabaseSharedState* p) {
	try {
		DB(db)->setSharedState(p);
	} catch (...) {
	}
}

// Get network thread busyness (updated every 1s)
// A value of 0 indicates that the client is more or less idle
// A value of 1 (or more) indicates that the client is saturated
extern "C" DLLEXPORT double fdb_database_get_main_thread_busyness(FDBDatabase* d) {
	return DB(d)->getMainThreadBusyness();
}

// Returns the protocol version reported by the coordinator this client is connected to
// If an expected version is non-zero, the future won't return until the protocol version is different than expected
// Note: this will never return if the server is running a protocol from FDB 5.0 or older
extern "C" DLLEXPORT FDBFuture* fdb_database_get_server_protocol(FDBDatabase* db, uint64_t expected_version) {
	Optional<ProtocolVersion> expected;
	if (expected_version > 0) {
		expected = ProtocolVersion(expected_version);
	}

	return (
	    FDBFuture*)(mapThreadFuture<ProtocolVersion,
	                                uint64_t>(DB(db)->getServerProtocol(expected), [](ErrorOr<ProtocolVersion> result) {
		                return result.map<uint64_t>([](ProtocolVersion pv) { return pv.versionWithFlags(); });
	                }).extractPtr());
}

extern "C" DLLEXPORT FDBFuture* fdb_database_purge_blob_granules(FDBDatabase* db,
                                                                 uint8_t const* begin_key_name,
                                                                 int begin_key_name_length,
                                                                 uint8_t const* end_key_name,
                                                                 int end_key_name_length,
                                                                 int64_t purge_version,
                                                                 fdb_bool_t force) {
	return (FDBFuture*)(DB(db)
	                        ->purgeBlobGranules(KeyRangeRef(StringRef(begin_key_name, begin_key_name_length),
	                                                        StringRef(end_key_name, end_key_name_length)),
	                                            purge_version,
	                                            force)
	                        .extractPtr());
}
extern "C" DLLEXPORT FDBFuture* fdb_database_wait_purge_granules_complete(FDBDatabase* db,
                                                                          uint8_t const* purge_key_name,
                                                                          int purge_key_name_length) {
	return (
	    FDBFuture*)(DB(db)->waitPurgeGranulesComplete(StringRef(purge_key_name, purge_key_name_length)).extractPtr());
}

extern "C" DLLEXPORT FDBFuture* fdb_database_blobbify_range(FDBDatabase* db,
                                                            uint8_t const* begin_key_name,
                                                            int begin_key_name_length,
                                                            uint8_t const* end_key_name,
                                                            int end_key_name_length) {
	return (FDBFuture*)(DB(db)
	                        ->blobbifyRange(KeyRangeRef(StringRef(begin_key_name, begin_key_name_length),
	                                                    StringRef(end_key_name, end_key_name_length)))
	                        .extractPtr());
}

extern "C" DLLEXPORT FDBFuture* fdb_database_unblobbify_range(FDBDatabase* db,
                                                              uint8_t const* begin_key_name,
                                                              int begin_key_name_length,
                                                              uint8_t const* end_key_name,
                                                              int end_key_name_length) {
	return (FDBFuture*)(DB(db)
	                        ->unblobbifyRange(KeyRangeRef(StringRef(begin_key_name, begin_key_name_length),
	                                                      StringRef(end_key_name, end_key_name_length)))
	                        .extractPtr());
}

extern "C" DLLEXPORT FDBFuture* fdb_database_list_blobbified_ranges(FDBDatabase* db,
                                                                    uint8_t const* begin_key_name,
                                                                    int begin_key_name_length,
                                                                    uint8_t const* end_key_name,
                                                                    int end_key_name_length,
                                                                    int rangeLimit) {
	return (FDBFuture*)(DB(db)
	                        ->listBlobbifiedRanges(KeyRangeRef(StringRef(begin_key_name, begin_key_name_length),
	                                                           StringRef(end_key_name, end_key_name_length)),
	                                               rangeLimit)
	                        .extractPtr());
}

extern "C" DLLEXPORT WARN_UNUSED_RESULT FDBFuture* fdb_database_verify_blob_range(FDBDatabase* db,
                                                                                  uint8_t const* begin_key_name,
                                                                                  int begin_key_name_length,
                                                                                  uint8_t const* end_key_name,
                                                                                  int end_key_name_length,
                                                                                  int64_t version) {
	Optional<Version> rv;
	if (version != latestVersion) {
		rv = version;
	}
	return (FDBFuture*)(DB(db)
	                        ->verifyBlobRange(KeyRangeRef(StringRef(begin_key_name, begin_key_name_length),
	                                                      StringRef(end_key_name, end_key_name_length)),
	                                          rv)
	                        .extractPtr());
}

extern "C" DLLEXPORT fdb_error_t fdb_tenant_create_transaction(FDBTenant* tenant, FDBTransaction** out_transaction) {
	CATCH_AND_RETURN(*out_transaction = (FDBTransaction*)TENANT(tenant)->createTransaction().extractPtr(););
}

extern "C" DLLEXPORT FDBFuture* fdb_tenant_purge_blob_granules(FDBTenant* tenant,
                                                               uint8_t const* begin_key_name,
                                                               int begin_key_name_length,
                                                               uint8_t const* end_key_name,
                                                               int end_key_name_length,
                                                               int64_t purge_version,
                                                               fdb_bool_t force) {
	return (FDBFuture*)(TENANT(tenant)
	                        ->purgeBlobGranules(KeyRangeRef(StringRef(begin_key_name, begin_key_name_length),
	                                                        StringRef(end_key_name, end_key_name_length)),
	                                            purge_version,
	                                            force)
	                        .extractPtr());
}
extern "C" DLLEXPORT FDBFuture* fdb_tenant_wait_purge_granules_complete(FDBTenant* tenant,
                                                                        uint8_t const* purge_key_name,
                                                                        int purge_key_name_length) {
	return (FDBFuture*)(TENANT(tenant)
	                        ->waitPurgeGranulesComplete(StringRef(purge_key_name, purge_key_name_length))
	                        .extractPtr());
}

extern "C" DLLEXPORT FDBFuture* fdb_tenant_blobbify_range(FDBTenant* tenant,
                                                          uint8_t const* begin_key_name,
                                                          int begin_key_name_length,
                                                          uint8_t const* end_key_name,
                                                          int end_key_name_length) {
	return (FDBFuture*)(TENANT(tenant)
	                        ->blobbifyRange(KeyRangeRef(StringRef(begin_key_name, begin_key_name_length),
	                                                    StringRef(end_key_name, end_key_name_length)))
	                        .extractPtr());
}

extern "C" DLLEXPORT FDBFuture* fdb_tenant_unblobbify_range(FDBTenant* tenant,
                                                            uint8_t const* begin_key_name,
                                                            int begin_key_name_length,
                                                            uint8_t const* end_key_name,
                                                            int end_key_name_length) {
	return (FDBFuture*)(TENANT(tenant)
	                        ->unblobbifyRange(KeyRangeRef(StringRef(begin_key_name, begin_key_name_length),
	                                                      StringRef(end_key_name, end_key_name_length)))
	                        .extractPtr());
}

extern "C" DLLEXPORT FDBFuture* fdb_tenant_list_blobbified_ranges(FDBTenant* tenant,
                                                                  uint8_t const* begin_key_name,
                                                                  int begin_key_name_length,
                                                                  uint8_t const* end_key_name,
                                                                  int end_key_name_length,
                                                                  int rangeLimit) {
	return (FDBFuture*)(TENANT(tenant)
	                        ->listBlobbifiedRanges(KeyRangeRef(StringRef(begin_key_name, begin_key_name_length),
	                                                           StringRef(end_key_name, end_key_name_length)),
	                                               rangeLimit)
	                        .extractPtr());
}

extern "C" DLLEXPORT WARN_UNUSED_RESULT FDBFuture* fdb_tenant_verify_blob_range(FDBTenant* tenant,
                                                                                uint8_t const* begin_key_name,
                                                                                int begin_key_name_length,
                                                                                uint8_t const* end_key_name,
                                                                                int end_key_name_length,
                                                                                int64_t version) {
	Optional<Version> rv;
	if (version != latestVersion) {
		rv = version;
	}
	return (FDBFuture*)(TENANT(tenant)
	                        ->verifyBlobRange(KeyRangeRef(StringRef(begin_key_name, begin_key_name_length),
	                                                      StringRef(end_key_name, end_key_name_length)),
	                                          rv)
	                        .extractPtr());
}

extern "C" DLLEXPORT void fdb_tenant_destroy(FDBTenant* tenant) {
	try {
		TENANT(tenant)->delref();
	} catch (...) {
	}
}

extern "C" DLLEXPORT void fdb_transaction_destroy(FDBTransaction* tr) {
	try {
		TXN(tr)->delref();
	} catch (...) {
	}
}

extern "C" DLLEXPORT void fdb_transaction_cancel(FDBTransaction* tr) {
	CATCH_AND_DIE(TXN(tr)->cancel(););
}

extern "C" DLLEXPORT void fdb_transaction_set_read_version(FDBTransaction* tr, int64_t version) {
	CATCH_AND_DIE(TXN(tr)->setVersion(version););
}

extern "C" DLLEXPORT FDBFuture* fdb_transaction_get_read_version(FDBTransaction* tr) {
	return (FDBFuture*)(TXN(tr)->getReadVersion().extractPtr());
}

FDBFuture* fdb_transaction_get_impl(FDBTransaction* tr,
                                    uint8_t const* key_name,
                                    int key_name_length,
                                    fdb_bool_t snapshot) {
	return (FDBFuture*)(TXN(tr)->get(KeyRef(key_name, key_name_length), snapshot).extractPtr());
}

FDBFuture* fdb_transaction_get_v13(FDBTransaction* tr, uint8_t const* key_name, int key_name_length) {
	return fdb_transaction_get_impl(tr, key_name, key_name_length, 0);
}

FDBFuture* fdb_transaction_get_key_impl(FDBTransaction* tr,
                                        uint8_t const* key_name,
                                        int key_name_length,
                                        fdb_bool_t or_equal,
                                        int offset,
                                        fdb_bool_t snapshot) {
	return (FDBFuture*)(TXN(tr)
	                        ->getKey(KeySelectorRef(KeyRef(key_name, key_name_length), or_equal, offset), snapshot)
	                        .extractPtr());
}

FDBFuture* fdb_transaction_get_key_v13(FDBTransaction* tr,
                                       uint8_t const* key_name,
                                       int key_name_length,
                                       fdb_bool_t or_equal,
                                       int offset) {
	return fdb_transaction_get_key_impl(tr, key_name, key_name_length, or_equal, offset, false);
}

extern "C" DLLEXPORT FDBFuture* fdb_transaction_get_addresses_for_key(FDBTransaction* tr,
                                                                      uint8_t const* key_name,
                                                                      int key_name_length) {
	return (FDBFuture*)(TXN(tr)->getAddressesForKey(KeyRef(key_name, key_name_length)).extractPtr());
}

// Set to the actual limit, target_bytes, and reverse.
FDBFuture* validate_and_update_parameters(int& limit,
                                          int& target_bytes,
                                          FDBStreamingMode mode,
                                          int iteration,
                                          fdb_bool_t& reverse) {
	/* This method may be called with a runtime API version of 13, in
	   which negative row limits are a reverse range read */
	if (g_api_version <= 13 && limit < 0) {
		limit = -limit;
		reverse = true;
	}

	/* Zero at the C API maps to "infinity" at lower levels */
	if (!limit)
		limit = GetRangeLimits::ROW_LIMIT_UNLIMITED;
	if (!target_bytes)
		target_bytes = GetRangeLimits::BYTE_LIMIT_UNLIMITED;

	/* Unlimited/unlimited with mode _EXACT isn't permitted */
	if (limit == GetRangeLimits::ROW_LIMIT_UNLIMITED && target_bytes == GetRangeLimits::BYTE_LIMIT_UNLIMITED &&
	    mode == FDB_STREAMING_MODE_EXACT)
		return TSAV_ERROR(Standalone<RangeResultRef>, exact_mode_without_limits);

	/* _ITERATOR mode maps to one of the known streaming modes
	   depending on iteration */
	const int mode_bytes_array[] = { GetRangeLimits::BYTE_LIMIT_UNLIMITED, 256, 1000, 4096, 80000 };

	/* The progression used for FDB_STREAMING_MODE_ITERATOR.
	   Goes 1.5 * previous. */
	static const int iteration_progression[] = { 4096, 6144, 9216, 13824, 20736, 31104, 46656, 69984, 80000, 120000 };

	/* length(iteration_progression) */
	static const int max_iteration = sizeof(iteration_progression) / sizeof(int);

	if (mode == FDB_STREAMING_MODE_WANT_ALL)
		mode = FDB_STREAMING_MODE_SERIAL;

	int mode_bytes;
	if (mode == FDB_STREAMING_MODE_ITERATOR) {
		if (iteration <= 0)
			return TSAV_ERROR(Standalone<RangeResultRef>, client_invalid_operation);

		iteration = std::min(iteration, max_iteration);
		mode_bytes = iteration_progression[iteration - 1];
	} else if (mode >= 0 && mode <= FDB_STREAMING_MODE_SERIAL)
		mode_bytes = mode_bytes_array[mode];
	else
		return TSAV_ERROR(Standalone<RangeResultRef>, client_invalid_operation);

	if (target_bytes == GetRangeLimits::BYTE_LIMIT_UNLIMITED)
		target_bytes = mode_bytes;
	else if (mode_bytes != GetRangeLimits::BYTE_LIMIT_UNLIMITED)
		target_bytes = std::min(target_bytes, mode_bytes);

	return nullptr;
}

FDBFuture* fdb_transaction_get_range_impl(FDBTransaction* tr,
                                          uint8_t const* begin_key_name,
                                          int begin_key_name_length,
                                          fdb_bool_t begin_or_equal,
                                          int begin_offset,
                                          uint8_t const* end_key_name,
                                          int end_key_name_length,
                                          fdb_bool_t end_or_equal,
                                          int end_offset,
                                          int limit,
                                          int target_bytes,
                                          FDBStreamingMode mode,
                                          int iteration,
                                          fdb_bool_t snapshot,
                                          fdb_bool_t reverse) {
	FDBFuture* r = validate_and_update_parameters(limit, target_bytes, mode, iteration, reverse);
	if (r != nullptr)
		return r;
	return (
	    FDBFuture*)(TXN(tr)
	                    ->getRange(
	                        KeySelectorRef(KeyRef(begin_key_name, begin_key_name_length), begin_or_equal, begin_offset),
	                        KeySelectorRef(KeyRef(end_key_name, end_key_name_length), end_or_equal, end_offset),
	                        GetRangeLimits(limit, target_bytes),
	                        snapshot,
	                        reverse)
	                    .extractPtr());
}

extern "C" DLLEXPORT FDBFuture* fdb_transaction_get_mapped_range(FDBTransaction* tr,
                                                                 uint8_t const* begin_key_name,
                                                                 int begin_key_name_length,
                                                                 fdb_bool_t begin_or_equal,
                                                                 int begin_offset,
                                                                 uint8_t const* end_key_name,
                                                                 int end_key_name_length,
                                                                 fdb_bool_t end_or_equal,
                                                                 int end_offset,
                                                                 uint8_t const* mapper_name,
                                                                 int mapper_name_length,
                                                                 int limit,
                                                                 int target_bytes,
                                                                 FDBStreamingMode mode,
                                                                 int iteration,
                                                                 int matchIndex,
                                                                 fdb_bool_t snapshot,
                                                                 fdb_bool_t reverse) {
	FDBFuture* r = validate_and_update_parameters(limit, target_bytes, mode, iteration, reverse);
	if (r != nullptr)
		return r;
	return (
	    FDBFuture*)(TXN(tr)
	                    ->getMappedRange(
	                        KeySelectorRef(KeyRef(begin_key_name, begin_key_name_length), begin_or_equal, begin_offset),
	                        KeySelectorRef(KeyRef(end_key_name, end_key_name_length), end_or_equal, end_offset),
	                        StringRef(mapper_name, mapper_name_length),
	                        GetRangeLimits(limit, target_bytes),
	                        matchIndex,
	                        snapshot,
	                        reverse)
	                    .extractPtr());
}

FDBFuture* fdb_transaction_get_range_selector_v13(FDBTransaction* tr,
                                                  uint8_t const* begin_key_name,
                                                  int begin_key_name_length,
                                                  fdb_bool_t begin_or_equal,
                                                  int begin_offset,
                                                  uint8_t const* end_key_name,
                                                  int end_key_name_length,
                                                  fdb_bool_t end_or_equal,
                                                  int end_offset,
                                                  int limit) {
	return fdb_transaction_get_range_impl(tr,
	                                      begin_key_name,
	                                      begin_key_name_length,
	                                      begin_or_equal,
	                                      begin_offset,
	                                      end_key_name,
	                                      end_key_name_length,
	                                      end_or_equal,
	                                      end_offset,
	                                      limit,
	                                      0,
	                                      FDB_STREAMING_MODE_EXACT,
	                                      0,
	                                      false,
	                                      false);
}

FDBFuture* fdb_transaction_get_range_v13(FDBTransaction* tr,
                                         uint8_t const* begin_key_name,
                                         int begin_key_name_length,
                                         uint8_t const* end_key_name,
                                         int end_key_name_length,
                                         int limit) {
	return fdb_transaction_get_range_selector_v13(
	    tr,
	    FDB_KEYSEL_FIRST_GREATER_OR_EQUAL(begin_key_name, begin_key_name_length),
	    FDB_KEYSEL_FIRST_GREATER_OR_EQUAL(end_key_name, end_key_name_length),
	    limit);
}

extern "C" DLLEXPORT void fdb_transaction_set(FDBTransaction* tr,
                                              uint8_t const* key_name,
                                              int key_name_length,
                                              uint8_t const* value,
                                              int value_length) {
	CATCH_AND_DIE(TXN(tr)->set(KeyRef(key_name, key_name_length), ValueRef(value, value_length)););
}

extern "C" DLLEXPORT void fdb_transaction_atomic_op(FDBTransaction* tr,
                                                    uint8_t const* key_name,
                                                    int key_name_length,
                                                    uint8_t const* param,
                                                    int param_length,
                                                    FDBMutationType operation_type) {
	CATCH_AND_DIE(TXN(tr)->atomicOp(
	    KeyRef(key_name, key_name_length), ValueRef(param, param_length), (FDBMutationTypes::Option)operation_type););
}

extern "C" DLLEXPORT void fdb_transaction_clear(FDBTransaction* tr, uint8_t const* key_name, int key_name_length) {
	CATCH_AND_DIE(TXN(tr)->clear(KeyRef(key_name, key_name_length)););
}

extern "C" DLLEXPORT void fdb_transaction_clear_range(FDBTransaction* tr,
                                                      uint8_t const* begin_key_name,
                                                      int begin_key_name_length,
                                                      uint8_t const* end_key_name,
                                                      int end_key_name_length) {
	CATCH_AND_DIE(
	    TXN(tr)->clear(KeyRef(begin_key_name, begin_key_name_length), KeyRef(end_key_name, end_key_name_length)););
}

extern "C" DLLEXPORT FDBFuture* fdb_transaction_watch(FDBTransaction* tr,
                                                      uint8_t const* key_name,
                                                      int key_name_length) {
	return (FDBFuture*)(TXN(tr)->watch(KeyRef(key_name, key_name_length)).extractPtr());
}

extern "C" DLLEXPORT FDBFuture* fdb_transaction_commit(FDBTransaction* tr) {
	return (FDBFuture*)(TXN(tr)->commit().extractPtr());
}

extern "C" DLLEXPORT fdb_error_t fdb_transaction_get_committed_version(FDBTransaction* tr, int64_t* out_version) {
	CATCH_AND_RETURN(*out_version = TXN(tr)->getCommittedVersion(););
}

extern "C" DLLEXPORT FDBFuture* fdb_transaction_get_total_cost(FDBTransaction* tr) {
	return (FDBFuture*)TXN(tr)->getTotalCost().extractPtr();
}

extern "C" DLLEXPORT FDBFuture* fdb_transaction_get_approximate_size(FDBTransaction* tr) {
	return (FDBFuture*)TXN(tr)->getApproximateSize().extractPtr();
}

extern "C" DLLEXPORT FDBFuture* fdb_transaction_get_versionstamp(FDBTransaction* tr) {
	return (FDBFuture*)(TXN(tr)->getVersionstamp().extractPtr());
}

fdb_error_t fdb_transaction_set_option_impl(FDBTransaction* tr,
                                            FDBTransactionOption option,
                                            uint8_t const* value,
                                            int value_length) {
	CATCH_AND_RETURN(TXN(tr)->setOption((FDBTransactionOptions::Option)option,
	                                    value ? StringRef(value, value_length) : Optional<StringRef>()););
}

void fdb_transaction_set_option_v13(FDBTransaction* tr, FDBTransactionOption option) {
	fdb_transaction_set_option_impl(tr, option, nullptr, 0);
}

extern "C" DLLEXPORT FDBFuture* fdb_transaction_on_error(FDBTransaction* tr, fdb_error_t error) {
	return (FDBFuture*)(TXN(tr)->onError(Error::fromUnvalidatedCode(error)).extractPtr());
}

extern "C" DLLEXPORT void fdb_transaction_reset(FDBTransaction* tr) {
	CATCH_AND_DIE(TXN(tr)->reset(););
}

extern "C" DLLEXPORT fdb_error_t fdb_transaction_add_conflict_range(FDBTransaction* tr,
                                                                    uint8_t const* begin_key_name,
                                                                    int begin_key_name_length,
                                                                    uint8_t const* end_key_name,
                                                                    int end_key_name_length,
                                                                    FDBConflictRangeType type) {
	CATCH_AND_RETURN(
	    KeyRangeRef range(KeyRef(begin_key_name, begin_key_name_length), KeyRef(end_key_name, end_key_name_length));
	    if (type == FDBConflictRangeType::FDB_CONFLICT_RANGE_TYPE_READ) TXN(tr)->addReadConflictRange(range);
	    else if (type == FDBConflictRangeType::FDB_CONFLICT_RANGE_TYPE_WRITE) TXN(tr)->addWriteConflictRange(range);
	    else return error_code_client_invalid_operation;);
}

extern "C" DLLEXPORT FDBFuture* fdb_transaction_get_estimated_range_size_bytes(FDBTransaction* tr,
                                                                               uint8_t const* begin_key_name,
                                                                               int begin_key_name_length,
                                                                               uint8_t const* end_key_name,
                                                                               int end_key_name_length) {
	RETURN_FUTURE_ON_ERROR(
	    int64_t,
	    KeyRangeRef range(KeyRef(begin_key_name, begin_key_name_length), KeyRef(end_key_name, end_key_name_length));
	    return (FDBFuture*)(TXN(tr)->getEstimatedRangeSizeBytes(range).extractPtr()););
}

extern "C" DLLEXPORT FDBFuture* fdb_transaction_get_range_split_points(FDBTransaction* tr,
                                                                       uint8_t const* begin_key_name,
                                                                       int begin_key_name_length,
                                                                       uint8_t const* end_key_name,
                                                                       int end_key_name_length,
                                                                       int64_t chunk_size) {
	RETURN_FUTURE_ON_ERROR(
	    Standalone<VectorRef<KeyRef>>,
	    KeyRangeRef range(KeyRef(begin_key_name, begin_key_name_length), KeyRef(end_key_name, end_key_name_length));
	    return (FDBFuture*)(TXN(tr)->getRangeSplitPoints(range, chunk_size).extractPtr()););
}

extern "C" DLLEXPORT FDBFuture* fdb_transaction_get_blob_granule_ranges(FDBTransaction* tr,
                                                                        uint8_t const* begin_key_name,
                                                                        int begin_key_name_length,
                                                                        uint8_t const* end_key_name,
                                                                        int end_key_name_length,
                                                                        int rangeLimit) {
	RETURN_FUTURE_ON_ERROR(
	    Standalone<VectorRef<KeyRangeRef>>,
	    KeyRangeRef range(KeyRef(begin_key_name, begin_key_name_length), KeyRef(end_key_name, end_key_name_length));
	    return (FDBFuture*)(TXN(tr)->getBlobGranuleRanges(range, rangeLimit).extractPtr()););
}

extern "C" DLLEXPORT FDBResult* fdb_transaction_read_blob_granules(FDBTransaction* tr,
                                                                   uint8_t const* begin_key_name,
                                                                   int begin_key_name_length,
                                                                   uint8_t const* end_key_name,
                                                                   int end_key_name_length,
                                                                   int64_t beginVersion,
                                                                   int64_t readVersion,
                                                                   FDBReadBlobGranuleContext granule_context) {
	RETURN_RESULT_ON_ERROR(
	    RangeResult,
	    KeyRangeRef range(KeyRef(begin_key_name, begin_key_name_length), KeyRef(end_key_name, end_key_name_length));

	    // FIXME: better way to convert?
	    ReadBlobGranuleContext context;
	    context.userContext = granule_context.userContext;
	    context.start_load_f = granule_context.start_load_f;
	    context.get_load_f = granule_context.get_load_f;
	    context.free_load_f = granule_context.free_load_f;
	    context.debugNoMaterialize = granule_context.debugNoMaterialize;
	    context.granuleParallelism = granule_context.granuleParallelism;

	    Optional<Version> rv;
	    if (readVersion != latestVersion) { rv = readVersion; }

	    return (FDBResult*)(TXN(tr)->readBlobGranules(range, beginVersion, rv, context).extractPtr()););
}

extern "C" DLLEXPORT FDBFuture* fdb_transaction_read_blob_granules_start(FDBTransaction* tr,
                                                                         uint8_t const* begin_key_name,
                                                                         int begin_key_name_length,
                                                                         uint8_t const* end_key_name,
                                                                         int end_key_name_length,
                                                                         int64_t beginVersion,
                                                                         int64_t readVersion,
                                                                         int64_t* readVersionOut) {
	Optional<Version> rv;
	if (readVersion != latestVersion) {
		rv = readVersion;
	}
	return (FDBFuture*)(TXN(tr)
	                        ->readBlobGranulesStart(KeyRangeRef(KeyRef(begin_key_name, begin_key_name_length),
	                                                            KeyRef(end_key_name, end_key_name_length)),
	                                                beginVersion,
	                                                rv,
	                                                readVersionOut)
	                        .extractPtr());
}

extern "C" DLLEXPORT FDBResult* fdb_transaction_read_blob_granules_finish(FDBTransaction* tr,
                                                                          FDBFuture* f,
                                                                          uint8_t const* begin_key_name,
                                                                          int begin_key_name_length,
                                                                          uint8_t const* end_key_name,
                                                                          int end_key_name_length,
                                                                          int64_t beginVersion,
                                                                          int64_t readVersion,
                                                                          FDBReadBlobGranuleContext* granule_context) {
	// FIXME: better way to convert?
	ReadBlobGranuleContext context;
	context.userContext = granule_context->userContext;
	context.start_load_f = granule_context->start_load_f;
	context.get_load_f = granule_context->get_load_f;
	context.free_load_f = granule_context->free_load_f;
	context.debugNoMaterialize = granule_context->debugNoMaterialize;
	context.granuleParallelism = granule_context->granuleParallelism;
	ThreadFuture<Standalone<VectorRef<BlobGranuleChunkRef>>> startFuture(
	    TSAV(Standalone<VectorRef<BlobGranuleChunkRef>>, f));

	return (FDBResult*)(TXN(tr)
	                        ->readBlobGranulesFinish(startFuture,
	                                                 KeyRangeRef(KeyRef(begin_key_name, begin_key_name_length),
	                                                             KeyRef(end_key_name, end_key_name_length)),
	                                                 beginVersion,
	                                                 readVersion,
	                                                 context)
	                        .extractPtr());
}

extern "C" DLLEXPORT FDBFuture* fdb_transaction_summarize_blob_granules(FDBTransaction* tr,
                                                                        uint8_t const* begin_key_name,
                                                                        int begin_key_name_length,
                                                                        uint8_t const* end_key_name,
                                                                        int end_key_name_length,
                                                                        int64_t summaryVersion,
                                                                        int rangeLimit) {
	RETURN_FUTURE_ON_ERROR(
	    Standalone<VectorRef<BlobGranuleSummaryRef>>,
	    KeyRangeRef range(KeyRef(begin_key_name, begin_key_name_length), KeyRef(end_key_name, end_key_name_length));

	    Optional<Version> sv;
	    if (summaryVersion != latestVersion) { sv = summaryVersion; }

	    return (FDBFuture*)(TXN(tr)->summarizeBlobGranules(range, sv, rangeLimit).extractPtr()););
}

#include "fdb_c_function_pointers.g.h"

#define FDB_API_CHANGED(func, ver)                                                                                     \
	if (header_version < ver)                                                                                          \
		fdb_api_ptr_##func = (void*)&(func##_v##ver##_PREV);                                                           \
	else if (fdb_api_ptr_##func == (void*)&fdb_api_ptr_unimpl)                                                         \
		fdb_api_ptr_##func = (void*)&(func##_impl);

#define FDB_API_REMOVED(func, ver)                                                                                     \
	if (header_version < ver)                                                                                          \
		fdb_api_ptr_##func = (void*)&(func##_v##ver##_PREV);                                                           \
	else                                                                                                               \
		fdb_api_ptr_##func = (void*)&fdb_api_ptr_removed;

extern "C" DLLEXPORT fdb_error_t fdb_select_api_version_impl(int runtime_version, int header_version) {
	/* Can only call this once */
	if (g_api_version != 0)
		return error_code_api_version_already_set;

	/* Caller screwed up, this makes no sense */
	if (runtime_version > header_version)
		return error_code_api_version_invalid;

	/* Caller requested a version we don't speak */
	if (header_version > FDB_API_VERSION)
		return error_code_api_version_not_supported;

	/* No backwards compatibility for earlier versions */
	if (runtime_version < 13)
		return error_code_api_version_not_supported;

	RETURN_ON_ERROR(API->selectApiVersion(runtime_version););

	g_api_version = runtime_version;

	platformInit();
	Error::init();

	// Versioned API changes -- descending order by version (new changes at top)
	// FDB_API_CHANGED( function, ver ) means there is a new implementation as of ver, and a function function_(ver-1)
	// is the old implementation. FDB_API_REMOVED( function, ver ) means the function was removed as of ver, and
	// function_(ver-1) is the old implementation
	//
	// WARNING: use caution when implementing removed functions by calling public API functions. This can lead to
	// undesired behavior when using the multi-version API. Instead, it is better to have both the removed and public
	// functions call an internal implementation function. See fdb_create_database_impl for an example.
	FDB_API_REMOVED(fdb_future_get_version, 620);
	FDB_API_REMOVED(fdb_create_cluster, 610);
	FDB_API_REMOVED(fdb_cluster_create_database, 610);
	FDB_API_REMOVED(fdb_cluster_set_option, 610);
	FDB_API_REMOVED(fdb_cluster_destroy, 610);
	FDB_API_REMOVED(fdb_future_get_cluster, 610);
	FDB_API_REMOVED(fdb_future_get_database, 610);
	FDB_API_CHANGED(fdb_future_get_error, 23);
	FDB_API_REMOVED(fdb_future_is_error, 23);
	FDB_API_CHANGED(fdb_future_get_keyvalue_array, 14);
	FDB_API_CHANGED(fdb_transaction_get_key, 14);
	FDB_API_CHANGED(fdb_transaction_get_range, 14);
	FDB_API_REMOVED(fdb_transaction_get_range_selector, 14);
	FDB_API_CHANGED(fdb_transaction_get, 14);
	FDB_API_CHANGED(fdb_setup_network, 14);
	FDB_API_CHANGED(fdb_transaction_set_option, 14);
	/* End versioned API changes */

	return error_code_success;
}

extern "C" DLLEXPORT int fdb_get_max_api_version() {
	return FDB_API_VERSION;
}

extern "C" DLLEXPORT const char* fdb_get_client_version() {
	return API->getClientVersion();
}

extern "C" DLLEXPORT void fdb_use_future_protocol_version() {
	API->useFutureProtocolVersion();
}

#if defined(__APPLE__)
#include <dlfcn.h>
__attribute__((constructor)) static void initialize() {
	// OS X ld doesn't support -z nodelete, so we dlopen to increment the reference count of this module
	Dl_info info;
	int ret = dladdr((void*)&fdb_select_api_version_impl, &info);
	if (!ret || !info.dli_fname)
		return; // If we get here somehow, we face the risk of seg faults if somebody unloads our library

	dlopen(info.dli_fname, RTLD_NOLOAD | RTLD_NODELETE);
}
#endif
