/*
 * BlobGranuleServerCommon.h
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2018 Apple Inc. and the FoundationDB project authors
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

#if defined(NO_INTELLISENSE) && !defined(FDBSERVER_BLOBGRANULESERVERCOMMON_ACTOR_G_H)
#define FDBSERVER_BLOBGRANULESERVERCOMMON_ACTOR_G_H
#include "fdbserver/BlobGranuleServerCommon.actor.g.h"
#elif !defined(FDBSERVER_BLOBGRANULESERVERCOMMON_ACTOR_H)
#define FDBSERVER_BLOBGRANULESERVERCOMMON_ACTOR_H

#pragma once

#include "fdbclient/BlobConnectionProvider.h"
#include "fdbclient/BlobGranuleCommon.h"
#include "fdbclient/CommitTransaction.h"
#include "fdbclient/FDBTypes.h"
#include "fdbclient/Tenant.h"

#include "fdbserver/ServerDBInfo.h"
#include "fdbserver/Knobs.h"
#include "flow/flow.h"

#include "flow/actorcompiler.h" // has to be last include

// Stores info about a file in blob storage
struct BlobFileIndex {
	Version version;
	std::string filename;
	int64_t offset;
	int64_t length;
	int64_t fullFileLength;
	Optional<BlobGranuleCipherKeysMeta> cipherKeysMeta;

	BlobFileIndex() {}

	BlobFileIndex(Version version, std::string filename, int64_t offset, int64_t length, int64_t fullFileLength)
	  : version(version), filename(filename), offset(offset), length(length), fullFileLength(fullFileLength) {}

	BlobFileIndex(Version version,
	              std::string filename,
	              int64_t offset,
	              int64_t length,
	              int64_t fullFileLength,
	              Optional<BlobGranuleCipherKeysMeta> ciphKeysMeta)
	  : version(version), filename(filename), offset(offset), length(length), fullFileLength(fullFileLength),
	    cipherKeysMeta(ciphKeysMeta) {}

	// compare on version
	bool operator<(const BlobFileIndex& r) const { return version < r.version; }
};

// FIXME: initialize these to smaller default sizes to save a bit of memory,
// particularly snapshotFiles Stores the files that comprise a blob granule
struct GranuleFiles {
	std::vector<BlobFileIndex> snapshotFiles;
	std::vector<BlobFileIndex> deltaFiles;

	void getFiles(Version beginVersion,
	              Version readVersion,
	              bool canCollapse,
	              BlobGranuleChunkRef& chunk,
	              Arena& replyArena,
	              int64_t& deltaBytesCounter,
	              bool summarize) const;
};

// serialize change feed key as UID bytes, to use 16 bytes on disk
Key granuleIDToCFKey(UID granuleID);

// parse change feed key back to UID, to be human-readable
UID cfKeyToGranuleID(Key cfKey);

class Transaction;
ACTOR Future<Optional<GranuleHistory>> getLatestGranuleHistory(Transaction* tr, KeyRange range);
ACTOR Future<Void> readGranuleFiles(Transaction* tr, Key* startKey, Key endKey, GranuleFiles* files, UID granuleID);

ACTOR Future<GranuleFiles> loadHistoryFiles(Database cx, UID granuleID);

enum ForcedPurgeState { NonePurged, SomePurged, AllPurged };
ACTOR Future<ForcedPurgeState> getForcePurgedState(Transaction* tr, KeyRange keyRange);

// TODO: versioned like SS has?
struct GranuleTenantData : NonCopyable, ReferenceCounted<GranuleTenantData> {
	TenantName name;
	TenantMapEntry entry;
	Reference<BlobConnectionProvider> bstore;
	Promise<Void> bstoreLoaded;

	GranuleTenantData() {}
	GranuleTenantData(TenantName name, TenantMapEntry entry) : name(name), entry(entry) {}

	void setBStore(Reference<BlobConnectionProvider> bs) {
		ASSERT(bstoreLoaded.canBeSet());
		bstore = bs;
		bstoreLoaded.send(Void());
	}
};

// TODO: add refreshing
struct BGTenantMap {
public:
	void addTenants(std::vector<std::pair<TenantName, TenantMapEntry>>);
	void removeTenants(std::vector<int64_t> tenantIds);

	Optional<TenantMapEntry> getTenantById(int64_t id);
	Reference<GranuleTenantData> getDataForGranule(const KeyRangeRef& keyRange);

	KeyRangeMap<Reference<GranuleTenantData>> tenantData;
	std::unordered_map<int64_t, TenantMapEntry> tenantInfoById;
	Reference<AsyncVar<ServerDBInfo> const> dbInfo;
	PromiseStream<Future<Void>> addActor;

	BGTenantMap() {}
	explicit BGTenantMap(const Reference<AsyncVar<ServerDBInfo> const> dbInfo) : dbInfo(dbInfo) {
		collection = actorCollection(addActor.getFuture());
	}

private:
	Future<Void> collection;
};

ACTOR Future<Void> dumpManifest(Database db, Reference<BlobConnectionProvider> blobConn);
ACTOR Future<Void> loadManifest(Database db, Reference<BlobConnectionProvider> blobConn);
ACTOR Future<Void> printRestoreSummary(Database db, Reference<BlobConnectionProvider> blobConn);
inline bool isFullRestoreMode() {
	return SERVER_KNOBS->BLOB_FULL_RESTORE_MODE;
};

#include "flow/unactorcompiler.h"

#endif
