/*
 * BlobGranuleCommon.h
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

#ifndef FDBCLIENT_BLOBGRANULECOMMON_H
#define FDBCLIENT_BLOBGRANULECOMMON_H
#pragma once

#include "fdbclient/BlobCipher.h"
#include "fdbclient/CommitTransaction.h"
#include "fdbclient/FDBTypes.h"

#include "flow/EncryptUtils.h"
#include "flow/IRandom.h"
#include "flow/serialize.h"

#include <sstream>

#define BG_ENCRYPT_COMPRESS_DEBUG false

// file format of actual blob files
struct GranuleSnapshot : VectorRef<KeyValueRef> {

	constexpr static FileIdentifier file_identifier = 1300395;

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, ((VectorRef<KeyValueRef>&)*this));
	}
};

// Deltas in version order
struct GranuleDeltas : VectorRef<MutationsAndVersionRef> {
	constexpr static FileIdentifier file_identifier = 8563013;

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, ((VectorRef<MutationsAndVersionRef>&)*this));
	}
};

struct GranuleMaterializeStats {
	int64_t inputBytes;
	int64_t outputBytes;

	GranuleMaterializeStats() : inputBytes(0), outputBytes(0) {}
};

struct BlobGranuleCipherKeysMeta {
	EncryptCipherDomainId textDomainId;
	EncryptCipherBaseKeyId textBaseCipherId;
	EncryptCipherRandomSalt textSalt;
	EncryptCipherDomainId headerDomainId;
	EncryptCipherBaseKeyId headerBaseCipherId;
	EncryptCipherRandomSalt headerSalt;
	std::string ivStr;

	BlobGranuleCipherKeysMeta() {}
	BlobGranuleCipherKeysMeta(const EncryptCipherDomainId tDomainId,
	                          const EncryptCipherBaseKeyId tBaseCipherId,
	                          const EncryptCipherRandomSalt tSalt,
	                          const EncryptCipherDomainId hDomainId,
	                          const EncryptCipherBaseKeyId hBaseCipherId,
	                          const EncryptCipherRandomSalt hSalt,
	                          const std::string& iv)
	  : textDomainId(tDomainId), textBaseCipherId(tBaseCipherId), textSalt(tSalt), headerDomainId(hDomainId),
	    headerBaseCipherId(hBaseCipherId), headerSalt(hSalt), ivStr(iv) {}

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, textDomainId, textBaseCipherId, textSalt, headerDomainId, headerBaseCipherId, headerSalt, ivStr);
	}
};

struct BlobGranuleCipherKey {
	constexpr static FileIdentifier file_identifier = 7274734;
	EncryptCipherDomainId encryptDomainId;
	EncryptCipherBaseKeyId baseCipherId;
	EncryptCipherRandomSalt salt;
	StringRef baseCipher;

	static BlobGranuleCipherKey fromBlobCipherKey(Reference<BlobCipherKey> keyRef, Arena& arena) {
		BlobGranuleCipherKey cipherKey;
		cipherKey.encryptDomainId = keyRef->getDomainId();
		cipherKey.baseCipherId = keyRef->getBaseCipherId();
		cipherKey.salt = keyRef->getSalt();
		cipherKey.baseCipher = makeString(keyRef->getBaseCipherLen(), arena);
		memcpy(mutateString(cipherKey.baseCipher), keyRef->rawBaseCipher(), keyRef->getBaseCipherLen());

		return cipherKey;
	}

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, encryptDomainId, baseCipherId, salt, baseCipher);
	}
};

struct BlobGranuleCipherKeysCtx {
	constexpr static FileIdentifier file_identifier = 1278718;
	BlobGranuleCipherKey textCipherKey;
	BlobGranuleCipherKey headerCipherKey;
	StringRef ivRef;

	static BlobGranuleCipherKeysMeta toCipherKeysMeta(const BlobGranuleCipherKeysCtx& ctx) {
		return BlobGranuleCipherKeysMeta(ctx.textCipherKey.encryptDomainId,
		                                 ctx.textCipherKey.baseCipherId,
		                                 ctx.textCipherKey.salt,
		                                 ctx.headerCipherKey.encryptDomainId,
		                                 ctx.headerCipherKey.baseCipherId,
		                                 ctx.headerCipherKey.salt,
		                                 ctx.ivRef.toString());
	}

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, textCipherKey, headerCipherKey, ivRef);
	}
};

struct BlobGranuleFileEncryptionKeys {
	Reference<BlobCipherKey> textCipherKey;
	Reference<BlobCipherKey> headerCipherKey;
};

struct BlobGranuleCipherKeysMetaRef {
	EncryptCipherDomainId textDomainId;
	EncryptCipherBaseKeyId textBaseCipherId;
	EncryptCipherRandomSalt textSalt;
	EncryptCipherDomainId headerDomainId;
	EncryptCipherBaseKeyId headerBaseCipherId;
	EncryptCipherRandomSalt headerSalt;
	StringRef ivRef;

	BlobGranuleCipherKeysMetaRef() {}
	BlobGranuleCipherKeysMetaRef(Arena& to, BlobGranuleCipherKeysMeta cipherKeysMeta)
	  : textDomainId(cipherKeysMeta.textDomainId), textBaseCipherId(cipherKeysMeta.textBaseCipherId),
	    textSalt(cipherKeysMeta.textSalt), headerDomainId(cipherKeysMeta.headerDomainId),
	    headerBaseCipherId(cipherKeysMeta.headerBaseCipherId), headerSalt(cipherKeysMeta.headerSalt),
	    ivRef(StringRef(to, cipherKeysMeta.ivStr)) {}

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, textDomainId, textBaseCipherId, textSalt, headerDomainId, headerBaseCipherId, headerSalt, ivRef);
	}
};

struct BlobFilePointerRef {
	constexpr static FileIdentifier file_identifier = 5253554;
	// Serializable fields
	StringRef filename;
	int64_t offset;
	int64_t length;
	int64_t fullFileLength;
	Optional<BlobGranuleCipherKeysCtx> cipherKeysCtx;

	// Non-serializable fields
	Optional<BlobGranuleCipherKeysMetaRef>
	    cipherKeysMetaRef; // Placeholder to cache information sufficient to lookup encryption ciphers

	BlobFilePointerRef() {}

	BlobFilePointerRef(Arena& to, const std::string& filename, int64_t offset, int64_t length, int64_t fullFileLength)
	  : filename(to, filename), offset(offset), length(length), fullFileLength(fullFileLength) {}

	BlobFilePointerRef(Arena& to,
	                   const std::string& filename,
	                   int64_t offset,
	                   int64_t length,
	                   int64_t fullFileLength,
	                   Optional<BlobGranuleCipherKeysCtx> ciphKeysCtx)
	  : filename(to, filename), offset(offset), length(length), fullFileLength(fullFileLength),
	    cipherKeysCtx(ciphKeysCtx) {}

	BlobFilePointerRef(Arena& to,
	                   const std::string& filename,
	                   int64_t offset,
	                   int64_t length,
	                   int64_t fullFileLength,
	                   Optional<BlobGranuleCipherKeysMeta> ciphKeysMeta)
	  : filename(to, filename), offset(offset), length(length), fullFileLength(fullFileLength) {
		if (ciphKeysMeta.present()) {
			cipherKeysMetaRef = BlobGranuleCipherKeysMetaRef(to, ciphKeysMeta.get());
		}
	}

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, filename, offset, length, fullFileLength, cipherKeysCtx);
	}

	std::string toString() const {
		std::stringstream ss;
		ss << filename.toString() << ":" << offset << ":" << length << ":" << fullFileLength;
		if (cipherKeysCtx.present()) {
			ss << ":CipherKeysCtx:TextCipher:" << cipherKeysCtx.get().textCipherKey.encryptDomainId << ":"
			   << cipherKeysCtx.get().textCipherKey.baseCipherId << ":" << cipherKeysCtx.get().textCipherKey.salt
			   << ":HeaderCipher:" << cipherKeysCtx.get().headerCipherKey.encryptDomainId << ":"
			   << cipherKeysCtx.get().headerCipherKey.baseCipherId << ":" << cipherKeysCtx.get().headerCipherKey.salt;
		}
		return std::move(ss).str();
	}
};

// the assumption of this response is that the client will deserialize the files
// and apply the mutations themselves
// TODO could filter out delta files that don't intersect the key range being
// requested?
// TODO since client request passes version, we don't need to include the
// version of each mutation in the response if we pruned it there
struct BlobGranuleChunkRef {
	constexpr static FileIdentifier file_identifier = 865198;
	KeyRangeRef keyRange;
	Version includedVersion;
	Version snapshotVersion;
	Optional<BlobFilePointerRef> snapshotFile; // not set if it's an incremental read
	VectorRef<BlobFilePointerRef> deltaFiles;
	GranuleDeltas newDeltas;
	Optional<KeyRef> tenantPrefix;

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, keyRange, includedVersion, snapshotVersion, snapshotFile, deltaFiles, newDeltas, tenantPrefix);
	}
};

struct BlobGranuleSummaryRef {
	constexpr static FileIdentifier file_identifier = 9774587;
	KeyRangeRef keyRange;
	Version snapshotVersion;
	int64_t snapshotSize;
	Version deltaVersion;
	int64_t deltaSize;

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, keyRange, snapshotVersion, snapshotSize, deltaVersion, deltaSize);
	}
};

BlobGranuleSummaryRef summarizeGranuleChunk(Arena& ar, const BlobGranuleChunkRef& chunk);

enum BlobGranuleSplitState { Unknown = 0, Initialized = 1, Assigned = 2, Done = 3 };

// Boundary metadata for each range indexed by the beginning of the range.
struct BlobGranuleMergeBoundary {
	constexpr static FileIdentifier file_identifier = 557861;

	// Hard boundaries represent backing regions we want to keep separate.
	bool buddy;

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, buddy);
	}
};

struct BlobGranuleHistoryValue {
	constexpr static FileIdentifier file_identifier = 991434;
	UID granuleID;
	VectorRef<KeyRef> parentBoundaries;
	VectorRef<Version> parentVersions;

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, granuleID, parentBoundaries, parentVersions);
	}
};

struct GranuleHistory {
	KeyRange range;
	Version version;
	Standalone<BlobGranuleHistoryValue> value;

	GranuleHistory() {}

	GranuleHistory(KeyRange range, Version version, Standalone<BlobGranuleHistoryValue> value)
	  : range(range), version(version), value(value) {}
};

// A manifest to assist full fdb restore from blob granule files
struct BlobManifest {
	constexpr static FileIdentifier file_identifier = 298872;
	VectorRef<KeyValueRef> rows;

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, rows);
	}
};

#endif
