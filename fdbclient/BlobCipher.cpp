/*
 * BlobCipher.cpp
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

#include "fdbclient/BlobCipher.h"

#include "fdbclient/Knobs.h"
#include "flow/Arena.h"
#include "flow/EncryptUtils.h"
#include "flow/Knobs.h"
#include "flow/Error.h"
#include "flow/FastRef.h"
#include "flow/IRandom.h"
#include "flow/ITrace.h"
#include "flow/Platform.h"
#include "flow/flow.h"
#include "flow/network.h"
#include "flow/Trace.h"
#include "flow/UnitTest.h"

#include <chrono>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#ifndef _WIN32
#include <unistd.h>
#else
#include <io.h>
#endif

#define BLOB_CIPHER_DEBUG false

namespace {
bool isEncryptHeaderAuthTokenModeValid(const EncryptAuthTokenMode mode) {
	return mode >= ENCRYPT_HEADER_AUTH_TOKEN_MODE_NONE && mode < ENCRYPT_HEADER_AUTH_TOKEN_LAST;
}
} // namespace

// BlobCipherMetrics methods

BlobCipherMetrics::CounterSet::CounterSet(CounterCollection& cc, std::string name)
  : encryptCPUTimeNS(name + "EncryptCPUTimeNS", cc), decryptCPUTimeNS(name + "DecryptCPUTimeNS", cc),
    getCipherKeysLatency(name + "GetCipherKeysLatency",
                         UID(),
                         FLOW_KNOBS->ENCRYPT_KEY_CACHE_LOGGING_INTERVAL,
                         FLOW_KNOBS->ENCRYPT_KEY_CACHE_LOGGING_SAMPLE_SIZE),
    getLatestCipherKeysLatency(name + "GetLatestCipherKeysLatency",
                               UID(),
                               FLOW_KNOBS->ENCRYPT_KEY_CACHE_LOGGING_INTERVAL,
                               FLOW_KNOBS->ENCRYPT_KEY_CACHE_LOGGING_SAMPLE_SIZE) {}

BlobCipherMetrics::BlobCipherMetrics()
  : cc("BlobCipher"), cipherKeyCacheHit("CipherKeyCacheHit", cc), cipherKeyCacheMiss("CipherKeyCacheMiss", cc),
    cipherKeyCacheExpired("CipherKeyCacheExpired", cc), latestCipherKeyCacheHit("LatestCipherKeyCacheHit", cc),
    latestCipherKeyCacheMiss("LatestCipherKeyCacheMiss", cc),
    latestCipherKeyCacheNeedsRefresh("LatestCipherKeyCacheNeedsRefresh", cc),
    getCipherKeysLatency("GetCipherKeysLatency",
                         UID(),
                         FLOW_KNOBS->ENCRYPT_KEY_CACHE_LOGGING_INTERVAL,
                         FLOW_KNOBS->ENCRYPT_KEY_CACHE_LOGGING_SAMPLE_SIZE),
    getLatestCipherKeysLatency("GetLatestCipherKeysLatency",
                               UID(),
                               FLOW_KNOBS->ENCRYPT_KEY_CACHE_LOGGING_INTERVAL,
                               FLOW_KNOBS->ENCRYPT_KEY_CACHE_LOGGING_SAMPLE_SIZE),
    counterSets({ CounterSet(cc, "TLog"),
                  CounterSet(cc, "KVMemory"),
                  CounterSet(cc, "KVRedwood"),
                  CounterSet(cc, "BlobGranule"),
                  CounterSet(cc, "Backup"),
                  CounterSet(cc, "Test") }) {
	specialCounter(cc, "CacheSize", []() { return BlobCipherKeyCache::getInstance()->getSize(); });
	traceFuture = traceCounters("BlobCipherMetrics", UID(), FLOW_KNOBS->ENCRYPT_KEY_CACHE_LOGGING_INTERVAL, &cc);
}

// BlobCipherKey class methods

BlobCipherKey::BlobCipherKey(const EncryptCipherDomainId& domainId,
                             const EncryptCipherBaseKeyId& baseCiphId,
                             const uint8_t* baseCiph,
                             int baseCiphLen,
                             const int64_t refreshAt,
                             const int64_t expireAt) {
	// Salt generated is used while applying HMAC Key derivation, hence, not using crypto-secure hash algorithm is ok.
	// Further, 'deterministic' salt generation is used to preserve simulation determinism properties.
	EncryptCipherRandomSalt salt;
	if (g_network->isSimulated()) {
		salt = deterministicRandom()->randomUInt64();
	} else {
		salt = nondeterministicRandom()->randomUInt64();
	}

	// Support two type of CipherKeys: 'revocable' & 'non-revocable' ciphers.
	// In all cases, either cipherKey never expires i.e. refreshAt == infinite, or, refreshAt needs <= expireAt
	// timestamp.
	ASSERT(refreshAt == std::numeric_limits<int64_t>::max() || (refreshAt <= expireAt));

	initKey(domainId, baseCiph, baseCiphLen, baseCiphId, salt, refreshAt, expireAt);
}

BlobCipherKey::BlobCipherKey(const EncryptCipherDomainId& domainId,
                             const EncryptCipherBaseKeyId& baseCiphId,
                             const uint8_t* baseCiph,
                             int baseCiphLen,
                             const EncryptCipherRandomSalt& salt,
                             const int64_t refreshAt,
                             const int64_t expireAt) {
	initKey(domainId, baseCiph, baseCiphLen, baseCiphId, salt, refreshAt, expireAt);
}

void BlobCipherKey::initKey(const EncryptCipherDomainId& domainId,
                            const uint8_t* baseCiph,
                            int baseCiphLen,
                            const EncryptCipherBaseKeyId& baseCiphId,
                            const EncryptCipherRandomSalt& salt,
                            const int64_t refreshAt,
                            const int64_t expireAt) {
	// Set the base encryption key properties
	baseCipher = std::make_unique<uint8_t[]>(AES_256_KEY_LENGTH);
	memset(baseCipher.get(), 0, AES_256_KEY_LENGTH);
	memcpy(baseCipher.get(), baseCiph, std::min<int>(baseCiphLen, AES_256_KEY_LENGTH));
	baseCipherLen = baseCiphLen;
	baseCipherId = baseCiphId;
	// Set the encryption domain for the base encryption key
	encryptDomainId = domainId;
	randomSalt = salt;
	// derive the encryption key
	cipher = std::make_unique<uint8_t[]>(AES_256_KEY_LENGTH);
	memset(cipher.get(), 0, AES_256_KEY_LENGTH);
	applyHmacSha256Derivation();
	// update cipher 'refresh' and 'expire' TS
	refreshAtTS = refreshAt;
	expireAtTS = expireAt;

#if BLOB_CIPHER_DEBUG
	TraceEvent(SevDebug, "BlobCipherKeyInit")
	    .detail("DomainId", domainId)
	    .detail("BaseCipherId", baseCipherId)
	    .detail("BaseCipherLen", baseCipherLen)
	    .detail("RandomSalt", randomSalt)
	    .detail("RefreshAt", refreshAtTS)
	    .detail("ExpireAtTS", expireAtTS);
#endif
}

void BlobCipherKey::applyHmacSha256Derivation() {
	Arena arena;
	uint8_t buf[baseCipherLen + sizeof(EncryptCipherRandomSalt)];
	memcpy(&buf[0], baseCipher.get(), baseCipherLen);
	memcpy(&buf[0] + baseCipherLen, &randomSalt, sizeof(EncryptCipherRandomSalt));
	HmacSha256DigestGen hmacGen(baseCipher.get(), baseCipherLen);
	unsigned int digestLen =
	    hmacGen.digest(&buf[0], baseCipherLen + sizeof(EncryptCipherRandomSalt), cipher.get(), AUTH_TOKEN_SIZE);
	if (digestLen < AES_256_KEY_LENGTH) {
		memcpy(cipher.get() + digestLen, buf, AES_256_KEY_LENGTH - digestLen);
	}
}

void BlobCipherKey::reset() {
	memset(baseCipher.get(), 0, baseCipherLen);
	memset(cipher.get(), 0, AES_256_KEY_LENGTH);
}

// BlobKeyIdCache class methods

BlobCipherKeyIdCache::BlobCipherKeyIdCache(EncryptCipherDomainId dId, size_t* sizeStat)
  : domainId(dId), latestBaseCipherKeyId(), latestRandomSalt(), sizeStat(sizeStat) {
	ASSERT(sizeStat != nullptr);
	TraceEvent(SevInfo, "BlobCipherKeyIdCacheInit").detail("DomainId", domainId);
}

BlobCipherKeyIdCacheKey BlobCipherKeyIdCache::getCacheKey(const EncryptCipherBaseKeyId& baseCipherKeyId,
                                                          const EncryptCipherRandomSalt& salt) {
	if (baseCipherKeyId == INVALID_ENCRYPT_CIPHER_KEY_ID || salt == INVALID_ENCRYPT_RANDOM_SALT) {
		throw encrypt_invalid_id();
	}
	return std::make_pair(baseCipherKeyId, salt);
}

Reference<BlobCipherKey> BlobCipherKeyIdCache::getLatestCipherKey() {
	if (!latestBaseCipherKeyId.present()) {
		return Reference<BlobCipherKey>();
	}
	ASSERT_NE(latestBaseCipherKeyId.get(), INVALID_ENCRYPT_CIPHER_KEY_ID);
	ASSERT(latestRandomSalt.present());
	ASSERT_NE(latestRandomSalt.get(), INVALID_ENCRYPT_RANDOM_SALT);

	return getCipherByBaseCipherId(latestBaseCipherKeyId.get(), latestRandomSalt.get());
}

Reference<BlobCipherKey> BlobCipherKeyIdCache::getCipherByBaseCipherId(const EncryptCipherBaseKeyId& baseCipherKeyId,
                                                                       const EncryptCipherRandomSalt& salt) {
	BlobCipherKeyIdCacheMapCItr itr = keyIdCache.find(getCacheKey(baseCipherKeyId, salt));
	if (itr == keyIdCache.end()) {
		return Reference<BlobCipherKey>();
	}
	return itr->second;
}

Reference<BlobCipherKey> BlobCipherKeyIdCache::insertBaseCipherKey(const EncryptCipherBaseKeyId& baseCipherId,
                                                                   const uint8_t* baseCipher,
                                                                   int baseCipherLen,
                                                                   const int64_t refreshAt,
                                                                   const int64_t expireAt) {
	ASSERT_GT(baseCipherId, INVALID_ENCRYPT_CIPHER_KEY_ID);

	// BaseCipherKeys are immutable, given the routine invocation updates 'latestCipher',
	// ensure no key-tampering is done
	Reference<BlobCipherKey> latestCipherKey = getLatestCipherKey();
	if (latestCipherKey.isValid() && latestCipherKey->getBaseCipherId() == baseCipherId) {
		if (memcmp(latestCipherKey->rawBaseCipher(), baseCipher, baseCipherLen) == 0) {
#if BLOB_CIPHER_DEBUG
			TraceEvent(SevDebug, "InsertBaseCipherKeyAlreadyPresent")
			    .detail("BaseCipherKeyId", baseCipherId)
			    .detail("DomainId", domainId);
#endif

			// Key is already present; nothing more to do.
			return latestCipherKey;
		} else {
			TraceEvent(SevInfo, "BlobCipherUpdatetBaseCipherKey")
			    .detail("BaseCipherKeyId", baseCipherId)
			    .detail("DomainId", domainId);
			throw encrypt_update_cipher();
		}
	}

	TraceEvent(SevInfo, "BlobCipherKeyInsertBaseCipherKeyLatest")
	    .detail("DomainId", domainId)
	    .detail("BaseCipherId", baseCipherId)
	    .detail("RefreshAt", refreshAt)
	    .detail("ExpireAt", expireAt);

	Reference<BlobCipherKey> cipherKey =
	    makeReference<BlobCipherKey>(domainId, baseCipherId, baseCipher, baseCipherLen, refreshAt, expireAt);
	BlobCipherKeyIdCacheKey cacheKey = getCacheKey(cipherKey->getBaseCipherId(), cipherKey->getSalt());
	keyIdCache.emplace(cacheKey, cipherKey);

	// Update the latest BaseCipherKeyId for the given encryption domain
	latestBaseCipherKeyId = baseCipherId;
	latestRandomSalt = cipherKey->getSalt();

	(*sizeStat)++;
	return cipherKey;
}

Reference<BlobCipherKey> BlobCipherKeyIdCache::insertBaseCipherKey(const EncryptCipherBaseKeyId& baseCipherId,
                                                                   const uint8_t* baseCipher,
                                                                   int baseCipherLen,
                                                                   const EncryptCipherRandomSalt& salt,
                                                                   const int64_t refreshAt,
                                                                   const int64_t expireAt) {
	ASSERT_NE(baseCipherId, INVALID_ENCRYPT_CIPHER_KEY_ID);
	ASSERT_NE(salt, INVALID_ENCRYPT_RANDOM_SALT);

	BlobCipherKeyIdCacheKey cacheKey = getCacheKey(baseCipherId, salt);

	// BaseCipherKeys are immutable, ensure that cached value doesn't get updated.
	BlobCipherKeyIdCacheMapCItr itr = keyIdCache.find(cacheKey);
	if (itr != keyIdCache.end()) {
		if (memcmp(itr->second->rawBaseCipher(), baseCipher, baseCipherLen) == 0) {
#if BLOB_CIPHER_DEBUG
			TraceEvent(SevDebug, "InsertBaseCipherKeyAlreadyPresent")
			    .detail("BaseCipherKeyId", baseCipherId)
			    .detail("DomainId", domainId);
#endif

			// Key is already present; nothing more to do.
			return itr->second;
		} else {
			TraceEvent(SevInfo, "BlobCipherUpdateBaseCipherKey")
			    .detail("BaseCipherKeyId", baseCipherId)
			    .detail("DomainId", domainId);
			throw encrypt_update_cipher();
		}
	}

	TraceEvent(SevInfo, "BlobCipherKeyInsertBaseCipherKey")
	    .detail("DomainId", domainId)
	    .detail("BaseCipherId", baseCipherId)
	    .detail("Salt", salt)
	    .detail("RefreshAt", refreshAt)
	    .detail("ExpireAt", expireAt);

	Reference<BlobCipherKey> cipherKey =
	    makeReference<BlobCipherKey>(domainId, baseCipherId, baseCipher, baseCipherLen, salt, refreshAt, expireAt);
	keyIdCache.emplace(cacheKey, cipherKey);
	(*sizeStat)++;
	return cipherKey;
}

void BlobCipherKeyIdCache::cleanup() {
	for (auto& keyItr : keyIdCache) {
		keyItr.second->reset();
	}

	keyIdCache.clear();
}

std::vector<Reference<BlobCipherKey>> BlobCipherKeyIdCache::getAllCipherKeys() {
	std::vector<Reference<BlobCipherKey>> cipherKeys;
	for (auto& keyItr : keyIdCache) {
		cipherKeys.push_back(keyItr.second);
	}
	return cipherKeys;
}

// BlobCipherKeyCache class methods

Reference<BlobCipherKey> BlobCipherKeyCache::insertCipherKey(const EncryptCipherDomainId& domainId,
                                                             const EncryptCipherBaseKeyId& baseCipherId,
                                                             const uint8_t* baseCipher,
                                                             int baseCipherLen,
                                                             const int64_t refreshAt,
                                                             const int64_t expireAt) {
	if (domainId == INVALID_ENCRYPT_DOMAIN_ID || baseCipherId == INVALID_ENCRYPT_CIPHER_KEY_ID) {
		throw encrypt_invalid_id();
	}

	Reference<BlobCipherKey> cipherKey;

	try {
		auto domainItr = domainCacheMap.find(domainId);
		if (domainItr == domainCacheMap.end()) {
			// Add mapping to track new encryption domain
			Reference<BlobCipherKeyIdCache> keyIdCache = makeReference<BlobCipherKeyIdCache>(domainId, &size);
			cipherKey = keyIdCache->insertBaseCipherKey(baseCipherId, baseCipher, baseCipherLen, refreshAt, expireAt);
			domainCacheMap.emplace(domainId, keyIdCache);
		} else {
			// Track new baseCipher keys
			Reference<BlobCipherKeyIdCache> keyIdCache = domainItr->second;
			cipherKey = keyIdCache->insertBaseCipherKey(baseCipherId, baseCipher, baseCipherLen, refreshAt, expireAt);
		}
	} catch (Error& e) {
		TraceEvent(SevWarn, "BlobCipherInsertCipherKeyFailed")
		    .detail("BaseCipherKeyId", baseCipherId)
		    .detail("DomainId", domainId);
		throw;
	}
	return cipherKey;
}

Reference<BlobCipherKey> BlobCipherKeyCache::insertCipherKey(const EncryptCipherDomainId& domainId,
                                                             const EncryptCipherBaseKeyId& baseCipherId,
                                                             const uint8_t* baseCipher,
                                                             int baseCipherLen,
                                                             const EncryptCipherRandomSalt& salt,
                                                             const int64_t refreshAt,
                                                             const int64_t expireAt) {
	if (domainId == INVALID_ENCRYPT_DOMAIN_ID || baseCipherId == INVALID_ENCRYPT_CIPHER_KEY_ID ||
	    salt == INVALID_ENCRYPT_RANDOM_SALT) {
		throw encrypt_invalid_id();
	}

	Reference<BlobCipherKey> cipherKey;
	try {
		auto domainItr = domainCacheMap.find(domainId);
		if (domainItr == domainCacheMap.end()) {
			// Add mapping to track new encryption domain
			Reference<BlobCipherKeyIdCache> keyIdCache = makeReference<BlobCipherKeyIdCache>(domainId, &size);
			cipherKey =
			    keyIdCache->insertBaseCipherKey(baseCipherId, baseCipher, baseCipherLen, salt, refreshAt, expireAt);
			domainCacheMap.emplace(domainId, keyIdCache);
		} else {
			// Track new baseCipher keys
			Reference<BlobCipherKeyIdCache> keyIdCache = domainItr->second;
			cipherKey =
			    keyIdCache->insertBaseCipherKey(baseCipherId, baseCipher, baseCipherLen, salt, refreshAt, expireAt);
		}
	} catch (Error& e) {
		TraceEvent(SevWarn, "BlobCipherInsertCipherKey_Failed")
		    .detail("BaseCipherKeyId", baseCipherId)
		    .detail("DomainId", domainId)
		    .detail("Salt", salt);
		throw;
	}
	return cipherKey;
}

Reference<BlobCipherKey> BlobCipherKeyCache::getLatestCipherKey(const EncryptCipherDomainId& domainId) {
	if (domainId == INVALID_ENCRYPT_DOMAIN_ID) {
		TraceEvent(SevWarn, "BlobCipherGetLatestCipherKeyInvalidID").detail("DomainId", domainId);
		throw encrypt_invalid_id();
	}
	auto domainItr = domainCacheMap.find(domainId);
	if (domainItr == domainCacheMap.end()) {
		TraceEvent(SevInfo, "BlobCipherGetLatestCipherKeyDomainNotFound").detail("DomainId", domainId);
		return Reference<BlobCipherKey>();
	}

	Reference<BlobCipherKeyIdCache> keyIdCache = domainItr->second;
	Reference<BlobCipherKey> cipherKey = keyIdCache->getLatestCipherKey();

	// Ensure 'freshness' guarantees for the latestCipher
	if (cipherKey.isValid()) {
		if (cipherKey->needsRefresh()) {
#if BLOB_CIPHER_DEBUG
			TraceEvent("SevDebug, BlobCipherGetLatestNeedsRefresh")
			    .detail("DomainId", domainId)
			    .detail("Now", now())
			    .detail("RefreshAt", cipherKey->getRefreshAtTS());
#endif
			++BlobCipherMetrics::getInstance()->latestCipherKeyCacheNeedsRefresh;
			return Reference<BlobCipherKey>();
		}
		++BlobCipherMetrics::getInstance()->latestCipherKeyCacheHit;
	} else {
		++BlobCipherMetrics::getInstance()->latestCipherKeyCacheMiss;
	}

	return cipherKey;
}

Reference<BlobCipherKey> BlobCipherKeyCache::getCipherKey(const EncryptCipherDomainId& domainId,
                                                          const EncryptCipherBaseKeyId& baseCipherId,
                                                          const EncryptCipherRandomSalt& salt) {
	auto domainItr = domainCacheMap.find(domainId);
	if (domainItr == domainCacheMap.end()) {
		return Reference<BlobCipherKey>();
	}

	Reference<BlobCipherKeyIdCache> keyIdCache = domainItr->second;
	Reference<BlobCipherKey> cipherKey = keyIdCache->getCipherByBaseCipherId(baseCipherId, salt);

	// Ensure 'liveness' guarantees for the cipher
	if (cipherKey.isValid()) {
		if (cipherKey->isExpired()) {
#if BLOB_CIPHER_DEBUG
			TraceEvent(SevDebug, "BlobCipherGetCipherExpired")
			    .detail("DomainId", domainId)
			    .detail("BaseCipherId", baseCipherId)
			    .detail("Now", now())
			    .detail("ExpireAt", cipherKey->getExpireAtTS());
#endif
			++BlobCipherMetrics::getInstance()->cipherKeyCacheExpired;
			return Reference<BlobCipherKey>();
		}
		++BlobCipherMetrics::getInstance()->cipherKeyCacheHit;
	} else {
		++BlobCipherMetrics::getInstance()->cipherKeyCacheMiss;
	}

	return cipherKey;
}

void BlobCipherKeyCache::resetEncryptDomainId(const EncryptCipherDomainId domainId) {
	auto domainItr = domainCacheMap.find(domainId);
	if (domainItr == domainCacheMap.end()) {
		return;
	}

	Reference<BlobCipherKeyIdCache> keyIdCache = domainItr->second;
	ASSERT(keyIdCache->getSize() <= size);
	size -= keyIdCache->getSize();
	keyIdCache->cleanup();
	TraceEvent(SevInfo, "BlobCipherResetEncryptDomainId").detail("DomainId", domainId);
}

void BlobCipherKeyCache::cleanup() noexcept {
	Reference<BlobCipherKeyCache> instance = BlobCipherKeyCache::getInstance();

	TraceEvent(SevInfo, "BlobCipherKeyCacheCleanup").log();

	for (auto& domainItr : instance->domainCacheMap) {
		Reference<BlobCipherKeyIdCache> keyIdCache = domainItr.second;
		keyIdCache->cleanup();
		TraceEvent(SevInfo, "BlobCipherKeyCacheCleanup").detail("DomainId", domainItr.first);
	}

	instance->domainCacheMap.clear();
	instance->size = 0;
}

std::vector<Reference<BlobCipherKey>> BlobCipherKeyCache::getAllCiphers(const EncryptCipherDomainId& domainId) {
	auto domainItr = domainCacheMap.find(domainId);
	if (domainItr == domainCacheMap.end()) {
		return {};
	}

	Reference<BlobCipherKeyIdCache> keyIdCache = domainItr->second;
	return keyIdCache->getAllCipherKeys();
}

// EncryptBlobCipherAes265Ctr class methods

EncryptBlobCipherAes265Ctr::EncryptBlobCipherAes265Ctr(Reference<BlobCipherKey> tCipherKey,
                                                       Reference<BlobCipherKey> hCipherKey,
                                                       const uint8_t* cipherIV,
                                                       const int ivLen,
                                                       const EncryptAuthTokenMode mode,
                                                       BlobCipherMetrics::UsageType usageType)
  : ctx(EVP_CIPHER_CTX_new()), textCipherKey(tCipherKey), headerCipherKey(hCipherKey), authTokenMode(mode),
    usageType(usageType) {
	ASSERT(isEncryptHeaderAuthTokenModeValid(mode));
	ASSERT_EQ(ivLen, AES_256_IV_LENGTH);
	memcpy(&iv[0], cipherIV, ivLen);
	init();
}

EncryptBlobCipherAes265Ctr::EncryptBlobCipherAes265Ctr(Reference<BlobCipherKey> tCipherKey,
                                                       Reference<BlobCipherKey> hCipherKey,
                                                       const EncryptAuthTokenMode mode,
                                                       BlobCipherMetrics::UsageType usageType)
  : ctx(EVP_CIPHER_CTX_new()), textCipherKey(tCipherKey), headerCipherKey(hCipherKey), authTokenMode(mode),
    usageType(usageType) {
	ASSERT(isEncryptHeaderAuthTokenModeValid(mode));
	deterministicRandom()->randomBytes(iv, AES_256_IV_LENGTH);
	init();
}

void EncryptBlobCipherAes265Ctr::init() {
	if (ctx == nullptr) {
		throw encrypt_ops_error();
	}
	if (EVP_EncryptInit_ex(ctx, EVP_aes_256_ctr(), nullptr, nullptr, nullptr) != 1) {
		throw encrypt_ops_error();
	}
	if (EVP_EncryptInit_ex(ctx, nullptr, nullptr, textCipherKey.getPtr()->data(), iv) != 1) {
		throw encrypt_ops_error();
	}
}

Reference<EncryptBuf> EncryptBlobCipherAes265Ctr::encrypt(const uint8_t* plaintext,
                                                          const int plaintextLen,
                                                          BlobCipherEncryptHeader* header,
                                                          Arena& arena) {
	double startTime = 0.0;
	if (CLIENT_KNOBS->ENABLE_ENCRYPTION_CPU_TIME_LOGGING) {
		startTime = timer_monotonic();
	}

	memset(reinterpret_cast<uint8_t*>(header), 0, sizeof(BlobCipherEncryptHeader));

	// Alloc buffer computation accounts for 'header authentication' generation scheme. If single-auth-token needs
	// to be generated, allocate buffer sufficient to append header to the cipherText optimizing memcpy cost.

	const int allocSize = authTokenMode == ENCRYPT_HEADER_AUTH_TOKEN_MODE_SINGLE
	                          ? plaintextLen + AES_BLOCK_SIZE + sizeof(BlobCipherEncryptHeader)
	                          : plaintextLen + AES_BLOCK_SIZE;
	Reference<EncryptBuf> encryptBuf = makeReference<EncryptBuf>(allocSize, arena);
	uint8_t* ciphertext = encryptBuf->begin();
	int bytes{ 0 };
	if (EVP_EncryptUpdate(ctx, ciphertext, &bytes, plaintext, plaintextLen) != 1) {
		TraceEvent(SevWarn, "BlobCipherEncryptUpdateFailed")
		    .detail("BaseCipherId", textCipherKey->getBaseCipherId())
		    .detail("EncryptDomainId", textCipherKey->getDomainId());
		throw encrypt_ops_error();
	}

	int finalBytes{ 0 };
	if (EVP_EncryptFinal_ex(ctx, ciphertext + bytes, &finalBytes) != 1) {
		TraceEvent(SevWarn, "BlobCipherEncryptFinalFailed")
		    .detail("BaseCipherId", textCipherKey->getBaseCipherId())
		    .detail("EncryptDomainId", textCipherKey->getDomainId());
		throw encrypt_ops_error();
	}

	if ((bytes + finalBytes) != plaintextLen) {
		TraceEvent(SevWarn, "BlobCipherEncryptUnexpectedCipherLen")
		    .detail("PlaintextLen", plaintextLen)
		    .detail("EncryptedBufLen", bytes + finalBytes);
		throw encrypt_ops_error();
	}

	// Populate encryption header flags details
	header->flags.size = sizeof(BlobCipherEncryptHeader);
	header->flags.headerVersion = EncryptBlobCipherAes265Ctr::ENCRYPT_HEADER_VERSION;
	header->flags.encryptMode = ENCRYPT_CIPHER_MODE_AES_256_CTR;
	header->flags.authTokenMode = authTokenMode;

	// Populate cipherText encryption-key details
	header->cipherTextDetails.baseCipherId = textCipherKey->getBaseCipherId();
	header->cipherTextDetails.encryptDomainId = textCipherKey->getDomainId();
	header->cipherTextDetails.salt = textCipherKey->getSalt();
	memcpy(&header->iv[0], &iv[0], AES_256_IV_LENGTH);

	if (authTokenMode == ENCRYPT_HEADER_AUTH_TOKEN_MODE_NONE) {
		// No header 'authToken' generation needed.
	} else {
		// Populate header encryption-key details
		header->cipherHeaderDetails.encryptDomainId = headerCipherKey->getDomainId();
		header->cipherHeaderDetails.baseCipherId = headerCipherKey->getBaseCipherId();
		header->cipherHeaderDetails.salt = headerCipherKey->getSalt();

		// Populate header authToken details
		if (header->flags.authTokenMode == ENCRYPT_HEADER_AUTH_TOKEN_MODE_SINGLE) {
			ASSERT_GE(allocSize, (bytes + finalBytes + sizeof(BlobCipherEncryptHeader)));
			ASSERT_GE(encryptBuf->getLogicalSize(), (bytes + finalBytes + sizeof(BlobCipherEncryptHeader)));

			memcpy(&ciphertext[bytes + finalBytes],
			       reinterpret_cast<const uint8_t*>(header),
			       sizeof(BlobCipherEncryptHeader));
			computeAuthToken(ciphertext,
			                 bytes + finalBytes + sizeof(BlobCipherEncryptHeader),
			                 headerCipherKey->rawCipher(),
			                 AES_256_KEY_LENGTH,
			                 &header->singleAuthToken.authToken[0],
			                 AUTH_TOKEN_SIZE);
		} else {
			ASSERT_EQ(header->flags.authTokenMode, ENCRYPT_HEADER_AUTH_TOKEN_MODE_MULTI);

			computeAuthToken(ciphertext,
			                 bytes + finalBytes,
			                 reinterpret_cast<const uint8_t*>(&header->cipherTextDetails.salt),
			                 sizeof(EncryptCipherRandomSalt),
			                 &header->multiAuthTokens.cipherTextAuthToken[0],
			                 AUTH_TOKEN_SIZE);
			computeAuthToken(reinterpret_cast<const uint8_t*>(header),
			                 sizeof(BlobCipherEncryptHeader),
			                 headerCipherKey->rawCipher(),
			                 AES_256_KEY_LENGTH,
			                 &header->multiAuthTokens.headerAuthToken[0],
			                 AUTH_TOKEN_SIZE);
		}
	}

	encryptBuf->setLogicalSize(plaintextLen);

	if (CLIENT_KNOBS->ENABLE_ENCRYPTION_CPU_TIME_LOGGING) {
		BlobCipherMetrics::counters(usageType).encryptCPUTimeNS += int64_t((timer_monotonic() - startTime) * 1e9);
	}

	CODE_PROBE(true, "Encrypting data with BlobCipher");

	return encryptBuf;
}

EncryptBlobCipherAes265Ctr::~EncryptBlobCipherAes265Ctr() {
	if (ctx != nullptr) {
		EVP_CIPHER_CTX_free(ctx);
	}
}

// DecryptBlobCipherAes256Ctr class methods

DecryptBlobCipherAes256Ctr::DecryptBlobCipherAes256Ctr(Reference<BlobCipherKey> tCipherKey,
                                                       Reference<BlobCipherKey> hCipherKey,
                                                       const uint8_t* iv,
                                                       BlobCipherMetrics::UsageType usageType)
  : ctx(EVP_CIPHER_CTX_new()), textCipherKey(tCipherKey), headerCipherKey(hCipherKey),
    headerAuthTokenValidationDone(false), authTokensValidationDone(false), usageType(usageType) {
	if (ctx == nullptr) {
		throw encrypt_ops_error();
	}
	if (!EVP_DecryptInit_ex(ctx, EVP_aes_256_ctr(), nullptr, nullptr, nullptr)) {
		throw encrypt_ops_error();
	}
	if (!EVP_DecryptInit_ex(ctx, nullptr, nullptr, tCipherKey.getPtr()->data(), iv)) {
		throw encrypt_ops_error();
	}
}

void DecryptBlobCipherAes256Ctr::verifyHeaderAuthToken(const BlobCipherEncryptHeader& header, Arena& arena) {
	if (header.flags.authTokenMode != ENCRYPT_HEADER_AUTH_TOKEN_MODE_MULTI) {
		// NoneAuthToken mode; no authToken is generated; nothing to do
		// SingleAuthToken mode; verification will happen as part of decryption.
		return;
	}

	ASSERT_EQ(header.flags.authTokenMode, ENCRYPT_HEADER_AUTH_TOKEN_MODE_MULTI);

	BlobCipherEncryptHeader headerCopy;
	memcpy(reinterpret_cast<uint8_t*>(&headerCopy),
	       reinterpret_cast<const uint8_t*>(&header),
	       sizeof(BlobCipherEncryptHeader));
	memset(reinterpret_cast<uint8_t*>(&headerCopy.multiAuthTokens.headerAuthToken), 0, AUTH_TOKEN_SIZE);
	uint8_t computedHeaderAuthToken[AUTH_TOKEN_SIZE];
	computeAuthToken(reinterpret_cast<const uint8_t*>(&headerCopy),
	                 sizeof(BlobCipherEncryptHeader),
	                 headerCipherKey->rawCipher(),
	                 AES_256_KEY_LENGTH,
	                 &computedHeaderAuthToken[0],
	                 AUTH_TOKEN_SIZE);
	if (memcmp(&header.multiAuthTokens.headerAuthToken[0], &computedHeaderAuthToken[0], AUTH_TOKEN_SIZE) != 0) {
		TraceEvent(SevWarn, "BlobCipherVerifyEncryptBlobHeaderAuthTokenMismatch")
		    .detail("HeaderVersion", header.flags.headerVersion)
		    .detail("HeaderMode", header.flags.encryptMode)
		    .detail("MultiAuthHeaderAuthToken",
		            StringRef(arena, &header.multiAuthTokens.headerAuthToken[0], AUTH_TOKEN_SIZE).toString())
		    .detail("ComputedHeaderAuthToken", StringRef(computedHeaderAuthToken, AUTH_TOKEN_SIZE));
		throw encrypt_header_authtoken_mismatch();
	}

	headerAuthTokenValidationDone = true;
}

void DecryptBlobCipherAes256Ctr::verifyHeaderSingleAuthToken(const uint8_t* ciphertext,
                                                             const int ciphertextLen,
                                                             const BlobCipherEncryptHeader& header,
                                                             uint8_t* buff,
                                                             Arena& arena) {
	// Header authToken not set for single auth-token mode.
	ASSERT(!headerAuthTokenValidationDone);

	// prepare the payload {cipherText + encryptionHeader}
	memcpy(&buff[0], ciphertext, ciphertextLen);
	memcpy(&buff[ciphertextLen], reinterpret_cast<const uint8_t*>(&header), sizeof(BlobCipherEncryptHeader));
	// ensure the 'authToken' is reset before computing the 'authentication token'
	BlobCipherEncryptHeader* eHeader = (BlobCipherEncryptHeader*)(&buff[ciphertextLen]);
	memset(reinterpret_cast<uint8_t*>(&eHeader->singleAuthToken), 0, 2 * AUTH_TOKEN_SIZE);

	uint8_t computed[AUTH_TOKEN_SIZE];
	computeAuthToken(buff,
	                 ciphertextLen + sizeof(BlobCipherEncryptHeader),
	                 headerCipherKey->rawCipher(),
	                 AES_256_KEY_LENGTH,
	                 &computed[0],
	                 AUTH_TOKEN_SIZE);
	if (memcmp(&header.singleAuthToken.authToken[0], &computed[0], AUTH_TOKEN_SIZE) != 0) {
		TraceEvent(SevWarn, "BlobCipherVerifyEncryptBlobHeaderAuthTokenMismatch")
		    .detail("HeaderVersion", header.flags.headerVersion)
		    .detail("HeaderMode", header.flags.encryptMode)
		    .detail("SingleAuthToken",
		            StringRef(arena, &header.singleAuthToken.authToken[0], AUTH_TOKEN_SIZE).toString())
		    .detail("ComputedSingleAuthToken", StringRef(computed, AUTH_TOKEN_SIZE));
		throw encrypt_header_authtoken_mismatch();
	}
}

void DecryptBlobCipherAes256Ctr::verifyHeaderMultiAuthToken(const uint8_t* ciphertext,
                                                            const int ciphertextLen,
                                                            const BlobCipherEncryptHeader& header,
                                                            uint8_t* buff,
                                                            Arena& arena) {
	if (!headerAuthTokenValidationDone) {
		verifyHeaderAuthToken(header, arena);
	}
	uint8_t computedCipherTextAuthToken[AUTH_TOKEN_SIZE];
	computeAuthToken(ciphertext,
	                 ciphertextLen,
	                 reinterpret_cast<const uint8_t*>(&header.cipherTextDetails.salt),
	                 sizeof(EncryptCipherRandomSalt),
	                 &computedCipherTextAuthToken[0],
	                 AUTH_TOKEN_SIZE);
	if (memcmp(&header.multiAuthTokens.cipherTextAuthToken[0], &computedCipherTextAuthToken[0], AUTH_TOKEN_SIZE) != 0) {
		TraceEvent(SevWarn, "BlobCipherVerifyEncryptBlobHeaderAuthTokenMismatch")
		    .detail("HeaderVersion", header.flags.headerVersion)
		    .detail("HeaderMode", header.flags.encryptMode)
		    .detail("MultiAuthCipherTextAuthToken",
		            StringRef(arena, &header.multiAuthTokens.cipherTextAuthToken[0], AUTH_TOKEN_SIZE).toString())
		    .detail("ComputedCipherTextAuthToken", StringRef(computedCipherTextAuthToken, AUTH_TOKEN_SIZE));
		throw encrypt_header_authtoken_mismatch();
	}
}

void DecryptBlobCipherAes256Ctr::verifyAuthTokens(const uint8_t* ciphertext,
                                                  const int ciphertextLen,
                                                  const BlobCipherEncryptHeader& header,
                                                  uint8_t* buff,
                                                  Arena& arena) {
	if (header.flags.authTokenMode == ENCRYPT_HEADER_AUTH_TOKEN_MODE_SINGLE) {
		verifyHeaderSingleAuthToken(ciphertext, ciphertextLen, header, buff, arena);
	} else {
		ASSERT_EQ(header.flags.authTokenMode, ENCRYPT_HEADER_AUTH_TOKEN_MODE_MULTI);
		verifyHeaderMultiAuthToken(ciphertext, ciphertextLen, header, buff, arena);
	}

	authTokensValidationDone = true;
}

void DecryptBlobCipherAes256Ctr::verifyEncryptHeaderMetadata(const BlobCipherEncryptHeader& header) {
	// validate header flag sanity
	if (header.flags.headerVersion != EncryptBlobCipherAes265Ctr::ENCRYPT_HEADER_VERSION ||
	    header.flags.encryptMode != ENCRYPT_CIPHER_MODE_AES_256_CTR ||
	    !isEncryptHeaderAuthTokenModeValid((EncryptAuthTokenMode)header.flags.authTokenMode)) {
		TraceEvent(SevWarn, "BlobCipherVerifyEncryptBlobHeader")
		    .detail("HeaderVersion", header.flags.headerVersion)
		    .detail("ExpectedVersion", EncryptBlobCipherAes265Ctr::ENCRYPT_HEADER_VERSION)
		    .detail("EncryptCipherMode", header.flags.encryptMode)
		    .detail("ExpectedCipherMode", ENCRYPT_CIPHER_MODE_AES_256_CTR)
		    .detail("EncryptHeaderAuthTokenMode", header.flags.authTokenMode);
		throw encrypt_header_metadata_mismatch();
	}
}

Reference<EncryptBuf> DecryptBlobCipherAes256Ctr::decrypt(const uint8_t* ciphertext,
                                                          const int ciphertextLen,
                                                          const BlobCipherEncryptHeader& header,
                                                          Arena& arena) {
	double startTime = 0.0;
	if (CLIENT_KNOBS->ENABLE_ENCRYPTION_CPU_TIME_LOGGING) {
		startTime = timer_monotonic();
	}

	verifyEncryptHeaderMetadata(header);

	if (header.flags.authTokenMode != ENCRYPT_HEADER_AUTH_TOKEN_MODE_NONE && !headerCipherKey.isValid()) {
		TraceEvent(SevWarn, "BlobCipherDecryptInvalidHeaderCipherKey")
		    .detail("AuthTokenMode", header.flags.authTokenMode);
		throw encrypt_ops_error();
	}

	const int allocSize = header.flags.authTokenMode == ENCRYPT_HEADER_AUTH_TOKEN_MODE_SINGLE
	                          ? ciphertextLen + AES_BLOCK_SIZE + sizeof(BlobCipherEncryptHeader)
	                          : ciphertextLen + AES_BLOCK_SIZE;
	Reference<EncryptBuf> decrypted = makeReference<EncryptBuf>(allocSize, arena);

	if (header.flags.authTokenMode != ENCRYPT_HEADER_AUTH_TOKEN_MODE_NONE) {
		verifyAuthTokens(ciphertext, ciphertextLen, header, decrypted->begin(), arena);
		ASSERT(authTokensValidationDone);
	}

	uint8_t* plaintext = decrypted->begin();
	int bytesDecrypted{ 0 };
	if (!EVP_DecryptUpdate(ctx, plaintext, &bytesDecrypted, ciphertext, ciphertextLen)) {
		TraceEvent(SevWarn, "BlobCipherDecryptUpdateFailed")
		    .detail("BaseCipherId", header.cipherTextDetails.baseCipherId)
		    .detail("EncryptDomainId", header.cipherTextDetails.encryptDomainId);
		throw encrypt_ops_error();
	}

	int finalBlobBytes{ 0 };
	if (EVP_DecryptFinal_ex(ctx, plaintext + bytesDecrypted, &finalBlobBytes) <= 0) {
		TraceEvent(SevWarn, "BlobCipherDecryptFinalFailed")
		    .detail("BaseCipherId", header.cipherTextDetails.baseCipherId)
		    .detail("EncryptDomainId", header.cipherTextDetails.encryptDomainId);
		throw encrypt_ops_error();
	}

	if ((bytesDecrypted + finalBlobBytes) != ciphertextLen) {
		TraceEvent(SevWarn, "BlobCipherEncryptUnexpectedPlaintextLen")
		    .detail("CiphertextLen", ciphertextLen)
		    .detail("DecryptedBufLen", bytesDecrypted + finalBlobBytes);
		throw encrypt_ops_error();
	}

	decrypted->setLogicalSize(ciphertextLen);

	if (CLIENT_KNOBS->ENABLE_ENCRYPTION_CPU_TIME_LOGGING) {
		BlobCipherMetrics::counters(usageType).decryptCPUTimeNS += int64_t((timer_monotonic() - startTime) * 1e9);
	}

	CODE_PROBE(true, "Decrypting data with BlobCipher");

	return decrypted;
}

DecryptBlobCipherAes256Ctr::~DecryptBlobCipherAes256Ctr() {
	if (ctx != nullptr) {
		EVP_CIPHER_CTX_free(ctx);
	}
}

// HmacSha256DigestGen class methods

HmacSha256DigestGen::HmacSha256DigestGen(const unsigned char* key, size_t len) : ctx(HMAC_CTX_new()) {
	if (!HMAC_Init_ex(ctx, key, len, EVP_sha256(), nullptr)) {
		throw encrypt_ops_error();
	}
}

HmacSha256DigestGen::~HmacSha256DigestGen() {
	if (ctx != nullptr) {
		HMAC_CTX_free(ctx);
	}
}

unsigned int HmacSha256DigestGen::digest(const unsigned char* data,
                                         size_t len,
                                         unsigned char* buf,
                                         unsigned int bufLen) {
	ASSERT_EQ(bufLen, HMAC_size(ctx));

	if (HMAC_Update(ctx, data, len) != 1) {
		throw encrypt_ops_error();
	}

	unsigned int digestLen = 0;
	if (HMAC_Final(ctx, buf, &digestLen) != 1) {
		throw encrypt_ops_error();
	}

	CODE_PROBE(true, "Digest generation");

	return digestLen;
}

void computeAuthToken(const uint8_t* payload,
                      const int payloadLen,
                      const uint8_t* key,
                      const int keyLen,
                      unsigned char* digestBuf,
                      unsigned int digestBufSz) {
	HmacSha256DigestGen hmacGenerator(key, keyLen);
	unsigned int digestLen = hmacGenerator.digest(payload, payloadLen, digestBuf, digestBufSz);

	ASSERT_EQ(digestLen, digestBufSz);

	CODE_PROBE(true, "Auth token generation");
}

// Only used to link unit tests
void forceLinkBlobCipherTests() {}

// Tests cases includes:
// 1. Populate cache by inserting 'baseCipher' details for new encryptionDomainIds
// 2. Random lookup for cipherKeys and content validation
// 3. Inserting of 'identical' cipherKey (already cached) more than once works as desired.
// 4. Inserting of 'non-identical' cipherKey (already cached) more than once works as desired.
// 5. Validation encryption ops (correctness):
//  5.1. Encrypt a buffer followed by decryption of the buffer, validate the contents.
//  5.2. Simulate anomalies such as: EncryptionHeader corruption, authToken mismatch / encryptionMode mismatch etc.
// 6. Cache cleanup
//  6.1  cleanup cipherKeys by given encryptDomainId
//  6.2. Cleanup all cached cipherKeys
TEST_CASE("flow/BlobCipher") {
	TraceEvent("BlobCipherTestStart").log();

	// Construct a dummy External Key Manager representation and populate with some keys
	class BaseCipher : public ReferenceCounted<BaseCipher>, NonCopyable {
	public:
		EncryptCipherDomainId domainId;
		int len;
		EncryptCipherBaseKeyId keyId;
		std::unique_ptr<uint8_t[]> key;
		int64_t refreshAt;
		int64_t expireAt;
		EncryptCipherRandomSalt generatedSalt;

		BaseCipher(const EncryptCipherDomainId& dId,
		           const EncryptCipherBaseKeyId& kId,
		           const int64_t rAt,
		           const int64_t eAt)
		  : domainId(dId), len(deterministicRandom()->randomInt(AES_256_KEY_LENGTH / 2, AES_256_KEY_LENGTH + 1)),
		    keyId(kId), key(std::make_unique<uint8_t[]>(len)), refreshAt(rAt), expireAt(eAt) {
			deterministicRandom()->randomBytes(key.get(), len);
		}
	};

	using BaseKeyMap = std::unordered_map<EncryptCipherBaseKeyId, Reference<BaseCipher>>;
	using DomainKeyMap = std::unordered_map<EncryptCipherDomainId, BaseKeyMap>;
	DomainKeyMap domainKeyMap;
	const EncryptCipherDomainId minDomainId = 1;
	const EncryptCipherDomainId maxDomainId = deterministicRandom()->randomInt(minDomainId, minDomainId + 10) + 5;
	const EncryptCipherBaseKeyId minBaseCipherKeyId = 100;
	const EncryptCipherBaseKeyId maxBaseCipherKeyId =
	    deterministicRandom()->randomInt(minBaseCipherKeyId, minBaseCipherKeyId + 50) + 15;
	for (int dId = minDomainId; dId <= maxDomainId; dId++) {
		for (int kId = minBaseCipherKeyId; kId <= maxBaseCipherKeyId; kId++) {
			domainKeyMap[dId].emplace(
			    kId,
			    makeReference<BaseCipher>(
			        dId, kId, std::numeric_limits<int64_t>::max(), std::numeric_limits<int64_t>::max()));
		}
	}
	ASSERT_EQ(domainKeyMap.size(), maxDomainId);

	Reference<BlobCipherKeyCache> cipherKeyCache = BlobCipherKeyCache::getInstance();

	// validate getLatestCipherKey return empty when there's no cipher key
	TraceEvent("BlobCipherTestLatestKeyNotExists").log();
	Reference<BlobCipherKey> latestKeyNonexists =
	    cipherKeyCache->getLatestCipherKey(deterministicRandom()->randomInt(minDomainId, maxDomainId));
	ASSERT(!latestKeyNonexists.isValid());
	try {
		cipherKeyCache->getLatestCipherKey(INVALID_ENCRYPT_DOMAIN_ID);
		ASSERT(false); // shouldn't get here
	} catch (Error& e) {
		ASSERT_EQ(e.code(), error_code_encrypt_invalid_id);
	}

	// insert BlobCipher keys into BlobCipherKeyCache map and validate
	TraceEvent("BlobCipherTestInsertKeys").log();
	for (auto& domainItr : domainKeyMap) {
		for (auto& baseKeyItr : domainItr.second) {
			Reference<BaseCipher> baseCipher = baseKeyItr.second;

			cipherKeyCache->insertCipherKey(baseCipher->domainId,
			                                baseCipher->keyId,
			                                baseCipher->key.get(),
			                                baseCipher->len,
			                                baseCipher->refreshAt,
			                                baseCipher->expireAt);
			Reference<BlobCipherKey> fetchedKey = cipherKeyCache->getLatestCipherKey(baseCipher->domainId);
			baseCipher->generatedSalt = fetchedKey->getSalt();
		}
	}
	// insert EncryptHeader BlobCipher key
	Reference<BaseCipher> headerBaseCipher = makeReference<BaseCipher>(
	    ENCRYPT_HEADER_DOMAIN_ID, 1, std::numeric_limits<int64_t>::max(), std::numeric_limits<int64_t>::max());
	cipherKeyCache->insertCipherKey(headerBaseCipher->domainId,
	                                headerBaseCipher->keyId,
	                                headerBaseCipher->key.get(),
	                                headerBaseCipher->len,
	                                headerBaseCipher->refreshAt,
	                                headerBaseCipher->expireAt);

	TraceEvent("BlobCipherTestInsertKeysDone").log();

	// validate the cipherKey lookups work as desired
	for (auto& domainItr : domainKeyMap) {
		for (auto& baseKeyItr : domainItr.second) {
			Reference<BaseCipher> baseCipher = baseKeyItr.second;
			Reference<BlobCipherKey> cipherKey =
			    cipherKeyCache->getCipherKey(baseCipher->domainId, baseCipher->keyId, baseCipher->generatedSalt);
			ASSERT(cipherKey.isValid());
			// validate common cipher properties - domainId, baseCipherId, baseCipherLen, rawBaseCipher
			ASSERT_EQ(cipherKey->getBaseCipherId(), baseCipher->keyId);
			ASSERT_EQ(cipherKey->getDomainId(), baseCipher->domainId);
			ASSERT_EQ(cipherKey->getBaseCipherLen(), baseCipher->len);
			// ensure that baseCipher matches with the cached information
			ASSERT_EQ(std::memcmp(cipherKey->rawBaseCipher(), baseCipher->key.get(), cipherKey->getBaseCipherLen()), 0);
			// validate the encryption derivation
			ASSERT_NE(std::memcmp(cipherKey->rawCipher(), baseCipher->key.get(), cipherKey->getBaseCipherLen()), 0);
		}
	}
	TraceEvent("BlobCipherTestLooksupDone").log();

	// Ensure attemtping to insert existing cipherKey (identical) more than once is treated as a NOP
	try {
		Reference<BaseCipher> baseCipher = domainKeyMap[minDomainId][minBaseCipherKeyId];
		cipherKeyCache->insertCipherKey(baseCipher->domainId,
		                                baseCipher->keyId,
		                                baseCipher->key.get(),
		                                baseCipher->len,
		                                std::numeric_limits<int64_t>::max(),
		                                std::numeric_limits<int64_t>::max());
	} catch (Error& e) {
		throw;
	}
	TraceEvent("BlobCipherTestReinsertIdempotentKeyDone").log();

	// Ensure attemtping to insert an existing cipherKey (modified) fails with appropriate error
	try {
		Reference<BaseCipher> baseCipher = domainKeyMap[minDomainId][minBaseCipherKeyId];
		uint8_t rawCipher[baseCipher->len];
		memcpy(rawCipher, baseCipher->key.get(), baseCipher->len);
		// modify few bytes in the cipherKey
		for (int i = 2; i < 5; i++) {
			rawCipher[i]++;
		}
		cipherKeyCache->insertCipherKey(baseCipher->domainId,
		                                baseCipher->keyId,
		                                &rawCipher[0],
		                                baseCipher->len,
		                                std::numeric_limits<int64_t>::max(),
		                                std::numeric_limits<int64_t>::max());
	} catch (Error& e) {
		if (e.code() != error_code_encrypt_update_cipher) {
			throw;
		}
	}
	TraceEvent("BlobCipherTestReinsertNonIdempotentKeyDone").log();

	// Validate Encryption ops
	Reference<BlobCipherKey> cipherKey = cipherKeyCache->getLatestCipherKey(minDomainId);
	Reference<BlobCipherKey> headerCipherKey = cipherKeyCache->getLatestCipherKey(ENCRYPT_HEADER_DOMAIN_ID);
	const int bufLen = deterministicRandom()->randomInt(786, 2127) + 512;
	uint8_t orgData[bufLen];
	deterministicRandom()->randomBytes(&orgData[0], bufLen);

	Arena arena;
	uint8_t iv[AES_256_IV_LENGTH];
	deterministicRandom()->randomBytes(&iv[0], AES_256_IV_LENGTH);

	BlobCipherEncryptHeader headerCopy;
	// validate basic encrypt followed by decrypt operation for AUTH_MODE_NONE
	{
		TraceEvent("NoneAuthModeStart").log();

		EncryptBlobCipherAes265Ctr encryptor(cipherKey,
		                                     Reference<BlobCipherKey>(),
		                                     iv,
		                                     AES_256_IV_LENGTH,
		                                     ENCRYPT_HEADER_AUTH_TOKEN_MODE_NONE,
		                                     BlobCipherMetrics::TEST);
		BlobCipherEncryptHeader header;
		Reference<EncryptBuf> encrypted = encryptor.encrypt(&orgData[0], bufLen, &header, arena);

		ASSERT_EQ(encrypted->getLogicalSize(), bufLen);
		ASSERT_NE(memcmp(&orgData[0], encrypted->begin(), bufLen), 0);
		ASSERT_EQ(header.flags.headerVersion, EncryptBlobCipherAes265Ctr::ENCRYPT_HEADER_VERSION);
		ASSERT_EQ(header.flags.encryptMode, ENCRYPT_CIPHER_MODE_AES_256_CTR);
		ASSERT_EQ(header.flags.authTokenMode, ENCRYPT_HEADER_AUTH_TOKEN_MODE_NONE);

		TraceEvent("BlobCipherTestEncryptDone")
		    .detail("HeaderVersion", header.flags.headerVersion)
		    .detail("HeaderEncryptMode", header.flags.encryptMode)
		    .detail("DomainId", header.cipherTextDetails.encryptDomainId)
		    .detail("BaseCipherId", header.cipherTextDetails.baseCipherId);

		Reference<BlobCipherKey> tCipherKeyKey = cipherKeyCache->getCipherKey(header.cipherTextDetails.encryptDomainId,
		                                                                      header.cipherTextDetails.baseCipherId,
		                                                                      header.cipherTextDetails.salt);
		ASSERT(tCipherKeyKey->isEqual(cipherKey));
		DecryptBlobCipherAes256Ctr decryptor(
		    tCipherKeyKey, Reference<BlobCipherKey>(), &header.iv[0], BlobCipherMetrics::TEST);
		Reference<EncryptBuf> decrypted = decryptor.decrypt(encrypted->begin(), bufLen, header, arena);

		ASSERT_EQ(decrypted->getLogicalSize(), bufLen);
		ASSERT_EQ(memcmp(decrypted->begin(), &orgData[0], bufLen), 0);

		TraceEvent("BlobCipherTestDecryptDone").log();

		// induce encryption header corruption - headerVersion corrupted
		memcpy(reinterpret_cast<uint8_t*>(&headerCopy),
		       reinterpret_cast<const uint8_t*>(&header),
		       sizeof(BlobCipherEncryptHeader));
		headerCopy.flags.headerVersion += 1;
		try {
			encrypted = encryptor.encrypt(&orgData[0], bufLen, &header, arena);
			DecryptBlobCipherAes256Ctr decryptor(
			    tCipherKeyKey, Reference<BlobCipherKey>(), header.iv, BlobCipherMetrics::TEST);
			decrypted = decryptor.decrypt(encrypted->begin(), bufLen, headerCopy, arena);
			ASSERT(false); // error expected
		} catch (Error& e) {
			if (e.code() != error_code_encrypt_header_metadata_mismatch) {
				throw;
			}
		}

		// induce encryption header corruption - encryptionMode corrupted
		memcpy(reinterpret_cast<uint8_t*>(&headerCopy),
		       reinterpret_cast<const uint8_t*>(&header),
		       sizeof(BlobCipherEncryptHeader));
		headerCopy.flags.encryptMode += 1;
		try {
			encrypted = encryptor.encrypt(&orgData[0], bufLen, &header, arena);
			DecryptBlobCipherAes256Ctr decryptor(
			    tCipherKeyKey, Reference<BlobCipherKey>(), header.iv, BlobCipherMetrics::TEST);
			decrypted = decryptor.decrypt(encrypted->begin(), bufLen, headerCopy, arena);
			ASSERT(false); // error expected
		} catch (Error& e) {
			if (e.code() != error_code_encrypt_header_metadata_mismatch) {
				throw;
			}
		}

		// induce encrypted buffer payload corruption
		try {
			encrypted = encryptor.encrypt(&orgData[0], bufLen, &header, arena);
			uint8_t temp[bufLen];
			memcpy(encrypted->begin(), &temp[0], bufLen);
			int tIdx = deterministicRandom()->randomInt(0, bufLen - 1);
			temp[tIdx] += 1;
			DecryptBlobCipherAes256Ctr decryptor(
			    tCipherKeyKey, Reference<BlobCipherKey>(), header.iv, BlobCipherMetrics::TEST);
			decrypted = decryptor.decrypt(&temp[0], bufLen, header, arena);
		} catch (Error& e) {
			// No authToken, hence, no corruption detection supported
			ASSERT(false);
		}

		TraceEvent("NoneAuthMode.Done").log();
	}

	// validate basic encrypt followed by decrypt operation for AUTH_TOKEN_MODE_SINGLE
	{
		TraceEvent("SingleAuthMode.Start").log();

		EncryptBlobCipherAes265Ctr encryptor(cipherKey,
		                                     headerCipherKey,
		                                     iv,
		                                     AES_256_IV_LENGTH,
		                                     ENCRYPT_HEADER_AUTH_TOKEN_MODE_SINGLE,
		                                     BlobCipherMetrics::TEST);
		BlobCipherEncryptHeader header;
		Reference<EncryptBuf> encrypted = encryptor.encrypt(&orgData[0], bufLen, &header, arena);

		ASSERT_EQ(encrypted->getLogicalSize(), bufLen);
		ASSERT_NE(memcmp(&orgData[0], encrypted->begin(), bufLen), 0);
		ASSERT_EQ(header.flags.headerVersion, EncryptBlobCipherAes265Ctr::ENCRYPT_HEADER_VERSION);
		ASSERT_EQ(header.flags.encryptMode, ENCRYPT_CIPHER_MODE_AES_256_CTR);
		ASSERT_EQ(header.flags.authTokenMode, ENCRYPT_HEADER_AUTH_TOKEN_MODE_SINGLE);

		TraceEvent("BlobCipherTestEncryptDone")
		    .detail("HeaderVersion", header.flags.headerVersion)
		    .detail("HeaderEncryptMode", header.flags.encryptMode)
		    .detail("DomainId", header.cipherTextDetails.encryptDomainId)
		    .detail("BaseCipherId", header.cipherTextDetails.baseCipherId)
		    .detail("HeaderAuthToken",
		            StringRef(arena, &header.singleAuthToken.authToken[0], AUTH_TOKEN_SIZE).toString());

		Reference<BlobCipherKey> tCipherKeyKey = cipherKeyCache->getCipherKey(header.cipherTextDetails.encryptDomainId,
		                                                                      header.cipherTextDetails.baseCipherId,
		                                                                      header.cipherTextDetails.salt);
		Reference<BlobCipherKey> hCipherKey = cipherKeyCache->getCipherKey(header.cipherHeaderDetails.encryptDomainId,
		                                                                   header.cipherHeaderDetails.baseCipherId,
		                                                                   header.cipherHeaderDetails.salt);
		ASSERT(tCipherKeyKey->isEqual(cipherKey));
		DecryptBlobCipherAes256Ctr decryptor(tCipherKeyKey, hCipherKey, header.iv, BlobCipherMetrics::TEST);
		Reference<EncryptBuf> decrypted = decryptor.decrypt(encrypted->begin(), bufLen, header, arena);

		ASSERT_EQ(decrypted->getLogicalSize(), bufLen);
		ASSERT_EQ(memcmp(decrypted->begin(), &orgData[0], bufLen), 0);

		TraceEvent("BlobCipherTestDecryptDone").log();

		// induce encryption header corruption - headerVersion corrupted
		encrypted = encryptor.encrypt(&orgData[0], bufLen, &header, arena);
		memcpy(reinterpret_cast<uint8_t*>(&headerCopy),
		       reinterpret_cast<const uint8_t*>(&header),
		       sizeof(BlobCipherEncryptHeader));
		headerCopy.flags.headerVersion += 1;
		try {
			DecryptBlobCipherAes256Ctr decryptor(tCipherKeyKey, hCipherKey, header.iv, BlobCipherMetrics::TEST);
			decrypted = decryptor.decrypt(encrypted->begin(), bufLen, headerCopy, arena);
			ASSERT(false); // error expected
		} catch (Error& e) {
			if (e.code() != error_code_encrypt_header_metadata_mismatch) {
				throw;
			}
		}

		// induce encryption header corruption - encryptionMode corrupted
		encrypted = encryptor.encrypt(&orgData[0], bufLen, &header, arena);
		memcpy(reinterpret_cast<uint8_t*>(&headerCopy),
		       reinterpret_cast<const uint8_t*>(&header),
		       sizeof(BlobCipherEncryptHeader));
		headerCopy.flags.encryptMode += 1;
		try {
			DecryptBlobCipherAes256Ctr decryptor(tCipherKeyKey, hCipherKey, header.iv, BlobCipherMetrics::TEST);
			decrypted = decryptor.decrypt(encrypted->begin(), bufLen, headerCopy, arena);
			ASSERT(false); // error expected
		} catch (Error& e) {
			if (e.code() != error_code_encrypt_header_metadata_mismatch) {
				throw;
			}
		}

		// induce encryption header corruption - authToken mismatch
		encrypted = encryptor.encrypt(&orgData[0], bufLen, &header, arena);
		memcpy(reinterpret_cast<uint8_t*>(&headerCopy),
		       reinterpret_cast<const uint8_t*>(&header),
		       sizeof(BlobCipherEncryptHeader));
		int hIdx = deterministicRandom()->randomInt(0, AUTH_TOKEN_SIZE - 1);
		headerCopy.singleAuthToken.authToken[hIdx] += 1;
		try {
			DecryptBlobCipherAes256Ctr decryptor(tCipherKeyKey, hCipherKey, header.iv, BlobCipherMetrics::TEST);
			decrypted = decryptor.decrypt(encrypted->begin(), bufLen, headerCopy, arena);
			ASSERT(false); // error expected
		} catch (Error& e) {
			if (e.code() != error_code_encrypt_header_authtoken_mismatch) {
				throw;
			}
		}

		// induce encrypted buffer payload corruption
		try {
			encrypted = encryptor.encrypt(&orgData[0], bufLen, &header, arena);
			uint8_t temp[bufLen];
			memcpy(encrypted->begin(), &temp[0], bufLen);
			int tIdx = deterministicRandom()->randomInt(0, bufLen - 1);
			temp[tIdx] += 1;
			DecryptBlobCipherAes256Ctr decryptor(tCipherKeyKey, hCipherKey, header.iv, BlobCipherMetrics::TEST);
			decrypted = decryptor.decrypt(&temp[0], bufLen, header, arena);
		} catch (Error& e) {
			if (e.code() != error_code_encrypt_header_authtoken_mismatch) {
				throw;
			}
		}

		TraceEvent("SingleAuthModeDone").log();
	}

	// validate basic encrypt followed by decrypt operation for AUTH_TOKEN_MODE_MULTI
	{
		TraceEvent("MultiAuthModeStart").log();

		EncryptBlobCipherAes265Ctr encryptor(cipherKey,
		                                     headerCipherKey,
		                                     iv,
		                                     AES_256_IV_LENGTH,
		                                     ENCRYPT_HEADER_AUTH_TOKEN_MODE_MULTI,
		                                     BlobCipherMetrics::TEST);
		BlobCipherEncryptHeader header;
		Reference<EncryptBuf> encrypted = encryptor.encrypt(&orgData[0], bufLen, &header, arena);

		ASSERT_EQ(encrypted->getLogicalSize(), bufLen);
		ASSERT_NE(memcmp(&orgData[0], encrypted->begin(), bufLen), 0);
		ASSERT_EQ(header.flags.headerVersion, EncryptBlobCipherAes265Ctr::ENCRYPT_HEADER_VERSION);
		ASSERT_EQ(header.flags.encryptMode, ENCRYPT_CIPHER_MODE_AES_256_CTR);
		ASSERT_EQ(header.flags.authTokenMode, ENCRYPT_HEADER_AUTH_TOKEN_MODE_MULTI);

		TraceEvent("BlobCipherTestEncryptDone")
		    .detail("HeaderVersion", header.flags.headerVersion)
		    .detail("HeaderEncryptMode", header.flags.encryptMode)
		    .detail("DomainId", header.cipherTextDetails.encryptDomainId)
		    .detail("BaseCipherId", header.cipherTextDetails.baseCipherId)
		    .detail("HeaderAuthToken",
		            StringRef(arena, &header.singleAuthToken.authToken[0], AUTH_TOKEN_SIZE).toString());

		Reference<BlobCipherKey> tCipherKey = cipherKeyCache->getCipherKey(header.cipherTextDetails.encryptDomainId,
		                                                                   header.cipherTextDetails.baseCipherId,
		                                                                   header.cipherTextDetails.salt);
		Reference<BlobCipherKey> hCipherKey = cipherKeyCache->getCipherKey(header.cipherHeaderDetails.encryptDomainId,
		                                                                   header.cipherHeaderDetails.baseCipherId,
		                                                                   header.cipherHeaderDetails.salt);

		ASSERT(tCipherKey->isEqual(cipherKey));
		DecryptBlobCipherAes256Ctr decryptor(tCipherKey, hCipherKey, header.iv, BlobCipherMetrics::TEST);
		Reference<EncryptBuf> decrypted = decryptor.decrypt(encrypted->begin(), bufLen, header, arena);

		ASSERT_EQ(decrypted->getLogicalSize(), bufLen);
		ASSERT_EQ(memcmp(decrypted->begin(), &orgData[0], bufLen), 0);

		TraceEvent("BlobCipherTestDecryptDone").log();

		// induce encryption header corruption - headerVersion corrupted
		encrypted = encryptor.encrypt(&orgData[0], bufLen, &header, arena);
		memcpy(reinterpret_cast<uint8_t*>(&headerCopy),
		       reinterpret_cast<const uint8_t*>(&header),
		       sizeof(BlobCipherEncryptHeader));
		headerCopy.flags.headerVersion += 1;
		try {
			DecryptBlobCipherAes256Ctr decryptor(tCipherKey, hCipherKey, header.iv, BlobCipherMetrics::TEST);
			decrypted = decryptor.decrypt(encrypted->begin(), bufLen, headerCopy, arena);
			ASSERT(false); // error expected
		} catch (Error& e) {
			if (e.code() != error_code_encrypt_header_metadata_mismatch) {
				throw;
			}
		}

		// induce encryption header corruption - encryptionMode corrupted
		encrypted = encryptor.encrypt(&orgData[0], bufLen, &header, arena);
		memcpy(reinterpret_cast<uint8_t*>(&headerCopy),
		       reinterpret_cast<const uint8_t*>(&header),
		       sizeof(BlobCipherEncryptHeader));
		headerCopy.flags.encryptMode += 1;
		try {
			DecryptBlobCipherAes256Ctr decryptor(tCipherKey, hCipherKey, header.iv, BlobCipherMetrics::TEST);
			decrypted = decryptor.decrypt(encrypted->begin(), bufLen, headerCopy, arena);
			ASSERT(false); // error expected
		} catch (Error& e) {
			if (e.code() != error_code_encrypt_header_metadata_mismatch) {
				throw;
			}
		}

		// induce encryption header corruption - cipherText authToken mismatch
		encrypted = encryptor.encrypt(&orgData[0], bufLen, &header, arena);
		memcpy(reinterpret_cast<uint8_t*>(&headerCopy),
		       reinterpret_cast<const uint8_t*>(&header),
		       sizeof(BlobCipherEncryptHeader));
		int hIdx = deterministicRandom()->randomInt(0, AUTH_TOKEN_SIZE - 1);
		headerCopy.multiAuthTokens.cipherTextAuthToken[hIdx] += 1;
		try {
			DecryptBlobCipherAes256Ctr decryptor(tCipherKey, hCipherKey, header.iv, BlobCipherMetrics::TEST);
			decrypted = decryptor.decrypt(encrypted->begin(), bufLen, headerCopy, arena);
			ASSERT(false); // error expected
		} catch (Error& e) {
			if (e.code() != error_code_encrypt_header_authtoken_mismatch) {
				throw;
			}
		}

		// induce encryption header corruption - header authToken mismatch
		encrypted = encryptor.encrypt(&orgData[0], bufLen, &header, arena);
		memcpy(reinterpret_cast<uint8_t*>(&headerCopy),
		       reinterpret_cast<const uint8_t*>(&header),
		       sizeof(BlobCipherEncryptHeader));
		hIdx = deterministicRandom()->randomInt(0, AUTH_TOKEN_SIZE - 1);
		headerCopy.multiAuthTokens.headerAuthToken[hIdx] += 1;
		try {
			DecryptBlobCipherAes256Ctr decryptor(tCipherKey, hCipherKey, header.iv, BlobCipherMetrics::TEST);
			decrypted = decryptor.decrypt(encrypted->begin(), bufLen, headerCopy, arena);
			ASSERT(false); // error expected
		} catch (Error& e) {
			if (e.code() != error_code_encrypt_header_authtoken_mismatch) {
				throw;
			}
		}

		try {
			encrypted = encryptor.encrypt(&orgData[0], bufLen, &header, arena);
			uint8_t temp[bufLen];
			memcpy(encrypted->begin(), &temp[0], bufLen);
			int tIdx = deterministicRandom()->randomInt(0, bufLen - 1);
			temp[tIdx] += 1;
			DecryptBlobCipherAes256Ctr decryptor(tCipherKey, hCipherKey, header.iv, BlobCipherMetrics::TEST);
			decrypted = decryptor.decrypt(&temp[0], bufLen, header, arena);
		} catch (Error& e) {
			if (e.code() != error_code_encrypt_header_authtoken_mismatch) {
				throw;
			}
		}

		TraceEvent("MultiAuthModeDone").log();
	}

	// Validate dropping encryptDomainId cached keys
	const EncryptCipherDomainId candidate = deterministicRandom()->randomInt(minDomainId, maxDomainId);
	cipherKeyCache->resetEncryptDomainId(candidate);
	std::vector<Reference<BlobCipherKey>> cachedKeys = cipherKeyCache->getAllCiphers(candidate);
	ASSERT(cachedKeys.empty());

	// Validate dropping all cached cipherKeys
	cipherKeyCache->cleanup();
	for (int dId = minDomainId; dId < maxDomainId; dId++) {
		std::vector<Reference<BlobCipherKey>> cachedKeys = cipherKeyCache->getAllCiphers(dId);
		ASSERT(cachedKeys.empty());
	}

	TraceEvent("BlobCipherTestDone").log();
	return Void();
}
