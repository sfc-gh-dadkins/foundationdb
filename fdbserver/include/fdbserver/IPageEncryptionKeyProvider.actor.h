/*
 * IPageEncryptionKeyProvider.actor.h
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
#if defined(NO_INTELLISENSE) && !defined(FDBSERVER_IPAGEENCRYPTIONKEYPROVIDER_ACTOR_G_H)
#define FDBSERVER_IPAGEENCRYPTIONKEYPROVIDER_ACTOR_G_H
#include "fdbserver/IPageEncryptionKeyProvider.actor.g.h"
#elif !defined(FDBSERVER_IPAGEENCRYPTIONKEYPROVIDER_ACTOR_H)
#define FDBSERVER_IPAGEENCRYPTIONKEYPROVIDER_ACTOR_H

#include "fdbclient/GetEncryptCipherKeys.actor.h"
#include "fdbclient/Tenant.h"

#include "fdbserver/EncryptionOpsUtils.h"
#include "fdbserver/IPager.h"
#include "fdbserver/ServerDBInfo.h"

#include "flow/Arena.h"
#include "flow/EncryptUtils.h"
#define XXH_INLINE_ALL
#include "flow/xxhash.h"

#include <tuple>

#include "flow/actorcompiler.h" // This must be the last #include.

// Interface used by pager to get encryption keys reading pages from disk
// and by the BTree to get encryption keys to use for new pages.
//
// Cipher key rotation:
// The key provider can rotate encryption keys, potentially per encryption domain (see below). Each of the new pages
// are encrypted using the latest encryption keys.
//
// Encryption domains:
// The key provider can specify how the page split the full key range into encryption domains by key prefixes.
// Encryption domains are expected to have their own set of encryption keys, which is managed by the key provider.
// The pager will ensure that data from different encryption domain won't fall in the same page, to make
// sure it can use one single encryption key to encrypt the whole page.
// The key provider needs to provide a default encryption domain, which is used to encrypt pages contain only
// full or partial encryption domain prefixes.
class IPageEncryptionKeyProvider : public ReferenceCounted<IPageEncryptionKeyProvider> {
public:
	using EncryptionKey = ArenaPage::EncryptionKey;

	virtual ~IPageEncryptionKeyProvider() = default;

	// Expected encoding type being used with the encryption key provider.
	virtual EncodingType expectedEncodingType() const = 0;

	// Checks whether encryption should be enabled. If not, the encryption key provider will not be used by
	// the pager, and instead the default non-encrypted encoding type (XXHash64) is used.
	virtual bool enableEncryption() const = 0;

	// Whether encryption domain is enabled.
	virtual bool enableEncryptionDomain() const { return false; }

	// Get an encryption key from given encoding header.
	virtual Future<EncryptionKey> getEncryptionKey(void* encodingHeader) { throw not_implemented(); }

	// Get latest encryption key. If encryption domain is enabled, get encryption key for the default domain.
	virtual Future<EncryptionKey> getLatestDefaultEncryptionKey() { throw not_implemented(); }

	// Get latest encryption key for data in given encryption domain.
	virtual Future<EncryptionKey> getLatestEncryptionKey(int64_t domainId) { throw not_implemented(); }

	// Return the default encryption domain.
	virtual int64_t getDefaultEncryptionDomainId() const { throw not_implemented(); }

	// Get encryption domain from a key. Return the domain id, and the size of the encryption domain prefix.
	// It is assumed that all keys with the same encryption domain prefix as the given key falls in the same encryption
	// domain. If possibleDomainId is given, it is a valid domain id previously returned by the key provider,
	// potentially for a different key. The possibleDomainId parm is used by TenantAwareEncryptionKeyProvider to speed
	// up encryption domain lookup.
	virtual std::tuple<int64_t, size_t> getEncryptionDomain(const KeyRef& key,
	                                                        Optional<int64_t> possibleDomainId = Optional<int64_t>()) {
		throw not_implemented();
	}

	// Get encryption domain of a page given encoding header.
	virtual int64_t getEncryptionDomain(void* encodingHeader) { throw not_implemented(); }

	// Setting tenant prefix to tenant name map. Used by TenantAwareEncryptionKeyProvider.
	virtual void setTenantPrefixIndex(Reference<TenantPrefixIndex> tenantPrefixIndex) {}
};

// The null key provider is useful to simplify page decoding.
// It throws an error for any key info requested.
class NullKeyProvider : public IPageEncryptionKeyProvider {
public:
	virtual ~NullKeyProvider() {}
	EncodingType expectedEncodingType() const override { return EncodingType::XXHash64; }
	bool enableEncryption() const override { return false; }
};

// Key provider for dummy XOR encryption scheme
class XOREncryptionKeyProvider_TestOnly : public IPageEncryptionKeyProvider {
public:
	using EncodingHeader = ArenaPage::XOREncryptionEncoder::Header;

	XOREncryptionKeyProvider_TestOnly(std::string filename) {
		ASSERT(g_network->isSimulated());

		// Choose a deterministic random filename (without path) byte for secret generation
		// Remove any leading directory names
		size_t lastSlash = filename.find_last_of("\\/");
		if (lastSlash != filename.npos) {
			filename.erase(0, lastSlash);
		}
		xorWith = filename.empty() ? 0x5e
		                           : (uint8_t)filename[XXH3_64bits(filename.data(), filename.size()) % filename.size()];
	}

	virtual ~XOREncryptionKeyProvider_TestOnly() {}

	EncodingType expectedEncodingType() const override { return EncodingType::XOREncryption_TestOnly; }

	bool enableEncryption() const override { return true; }

	bool enableEncryptionDomain() const override { return true; }

	Future<EncryptionKey> getEncryptionKey(void* encodingHeader) override {

		EncodingHeader* h = reinterpret_cast<EncodingHeader*>(encodingHeader);
		EncryptionKey s;
		s.xorKey = h->xorKey;
		return s;
	}

	Future<EncryptionKey> getLatestDefaultEncryptionKey() override { return getLatestEncryptionKey(0); }

	Future<EncryptionKey> getLatestEncryptionKey(int64_t domainId) override {
		EncryptionKey s;
		s.xorKey = ~(uint8_t)domainId ^ xorWith;
		return s;
	}

	int64_t getDefaultEncryptionDomainId() const override { return 0; }

	std::tuple<int64_t, size_t> getEncryptionDomain(const KeyRef& key,
	                                                Optional<int64_t> /*possibleDomainId*/) override {
		if (key.size() > 0) {
			return { *key.begin(), 1 };
		}
		return { 0, 0 };
	}

	int64_t getEncryptionDomain(void* encodingHeader) override {
		uint8_t xorKey = reinterpret_cast<EncodingHeader*>(encodingHeader)->xorKey;
		return (int64_t)(~xorKey ^ xorWith);
	}

	uint8_t xorWith;
};

// Key provider to provider cipher keys randomly from a pre-generated pool. It does not maintain encryption domains.
// Use for testing.
class RandomEncryptionKeyProvider : public IPageEncryptionKeyProvider {
public:
	RandomEncryptionKeyProvider() {
		for (unsigned i = 0; i < NUM_CIPHER; i++) {
			BlobCipherDetails cipherDetails;
			cipherDetails.encryptDomainId = i;
			cipherDetails.baseCipherId = deterministicRandom()->randomUInt64();
			cipherDetails.salt = deterministicRandom()->randomUInt64();
			cipherKeys[i] = generateCipherKey(cipherDetails);
		}
	}
	virtual ~RandomEncryptionKeyProvider() = default;

	EncodingType expectedEncodingType() const override { return EncodingType::AESEncryptionV1; }

	bool enableEncryption() const override { return true; }

	Future<EncryptionKey> getEncryptionKey(void* encodingHeader) override {
		using Header = ArenaPage::AESEncryptionV1Encoder::Header;
		Header* h = reinterpret_cast<Header*>(encodingHeader);
		EncryptionKey s;
		s.aesKey.cipherTextKey = cipherKeys[h->cipherTextDetails.encryptDomainId];
		s.aesKey.cipherHeaderKey = cipherKeys[h->cipherHeaderDetails.encryptDomainId];
		return s;
	}

	Future<EncryptionKey> getLatestDefaultEncryptionKey() override {
		EncryptionKey s;
		s.aesKey.cipherTextKey = cipherKeys[deterministicRandom()->randomInt(0, NUM_CIPHER)];
		s.aesKey.cipherHeaderKey = cipherKeys[deterministicRandom()->randomInt(0, NUM_CIPHER)];
		return s;
	}

private:
	Reference<BlobCipherKey> generateCipherKey(const BlobCipherDetails& cipherDetails) {
		static unsigned char SHA_KEY[] = "3ab9570b44b8315fdb261da6b1b6c13b";
		Arena arena;
		uint8_t digest[AUTH_TOKEN_SIZE];
		computeAuthToken(reinterpret_cast<const unsigned char*>(&cipherDetails.baseCipherId),
		                 sizeof(EncryptCipherBaseKeyId),
		                 SHA_KEY,
		                 AES_256_KEY_LENGTH,
		                 &digest[0],
		                 AUTH_TOKEN_SIZE);
		ASSERT_EQ(AUTH_TOKEN_SIZE, AES_256_KEY_LENGTH);
		return makeReference<BlobCipherKey>(cipherDetails.encryptDomainId,
		                                    cipherDetails.baseCipherId,
		                                    &digest[0],
		                                    AES_256_KEY_LENGTH,
		                                    cipherDetails.salt,
		                                    std::numeric_limits<int64_t>::max() /* refreshAt */,
		                                    std::numeric_limits<int64_t>::max() /* expireAt */);
	}

	static constexpr int NUM_CIPHER = 1000;
	Reference<BlobCipherKey> cipherKeys[NUM_CIPHER];
};

// Key provider which extract tenant id from range key prefixes, and fetch tenant specific encryption keys from
// EncryptKeyProxy.
class TenantAwareEncryptionKeyProvider : public IPageEncryptionKeyProvider {
public:
	using EncodingHeader = ArenaPage::AESEncryptionV1Encoder::Header;

	TenantAwareEncryptionKeyProvider(Reference<AsyncVar<ServerDBInfo> const> db) : db(db) {}

	virtual ~TenantAwareEncryptionKeyProvider() = default;

	EncodingType expectedEncodingType() const override { return EncodingType::AESEncryptionV1; }

	bool enableEncryption() const override {
		return isEncryptionOpSupported(EncryptOperationType::STORAGE_SERVER_ENCRYPTION, db->get().client);
	}

	bool enableEncryptionDomain() const override { return true; }

	ACTOR static Future<EncryptionKey> getEncryptionKey(TenantAwareEncryptionKeyProvider* self, void* encodingHeader) {
		BlobCipherEncryptHeader* header = reinterpret_cast<EncodingHeader*>(encodingHeader);
		TextAndHeaderCipherKeys cipherKeys =
		    wait(getEncryptCipherKeys(self->db, *header, BlobCipherMetrics::KV_REDWOOD));
		EncryptionKey encryptionKey;
		encryptionKey.aesKey = cipherKeys;
		return encryptionKey;
	}

	Future<EncryptionKey> getEncryptionKey(void* encodingHeader) override {
		return getEncryptionKey(this, encodingHeader);
	}

	Future<EncryptionKey> getLatestDefaultEncryptionKey() override {
		return getLatestEncryptionKey(getDefaultEncryptionDomainId());
	}

	ACTOR static Future<EncryptionKey> getLatestEncryptionKey(TenantAwareEncryptionKeyProvider* self,
	                                                          int64_t domainId) {

		EncryptCipherDomainNameRef domainName = self->getDomainName(domainId);
		TextAndHeaderCipherKeys cipherKeys =
		    wait(getLatestEncryptCipherKeysForDomain(self->db, domainId, domainName, BlobCipherMetrics::KV_REDWOOD));
		EncryptionKey encryptionKey;
		encryptionKey.aesKey = cipherKeys;
		return encryptionKey;
	}

	Future<EncryptionKey> getLatestEncryptionKey(int64_t domainId) override {
		return getLatestEncryptionKey(this, domainId);
	}

	int64_t getDefaultEncryptionDomainId() const override { return FDB_DEFAULT_ENCRYPT_DOMAIN_ID; }

	std::tuple<int64_t, size_t> getEncryptionDomain(const KeyRef& key, Optional<int64_t> possibleDomainId) override {
		// System key.
		if (key.startsWith("\xff\xff"_sr)) {
			return { SYSTEM_KEYSPACE_ENCRYPT_DOMAIN_ID, 2 };
		}
		// Key smaller than tenant prefix in size belongs to the default domain.
		if (key.size() < TENANT_PREFIX_SIZE) {
			return { FDB_DEFAULT_ENCRYPT_DOMAIN_ID, 0 };
		}
		StringRef prefix = key.substr(0, TENANT_PREFIX_SIZE);
		int64_t tenantId = TenantMapEntry::prefixToId(prefix);
		// Tenant id must be non-negative.
		if (tenantId < 0) {
			return { FDB_DEFAULT_ENCRYPT_DOMAIN_ID, 0 };
		}
		// Optimization: Caller guarantee possibleDomainId is a valid domain id that we previously returned.
		// We can return immediately without checking with tenant map.
		if (possibleDomainId.present() && possibleDomainId.get() == tenantId) {
			return { tenantId, TENANT_PREFIX_SIZE };
		}
		if (tenantPrefixIndex.isValid()) {
			auto view = tenantPrefixIndex->atLatest();
			auto itr = view.find(prefix);
			if (itr != view.end()) {
				// Tenant not found. Tenant must be disabled, or in optional mode.
				return { tenantId, TENANT_PREFIX_SIZE };
			}
		}
		// The prefix does not belong to any tenant. The key belongs to the default domain.
		return { FDB_DEFAULT_ENCRYPT_DOMAIN_ID, 0 };
	}

	int64_t getEncryptionDomain(void* encodingHeader) override {
		BlobCipherEncryptHeader* header = reinterpret_cast<EncodingHeader*>(encodingHeader);
		return header->cipherTextDetails.encryptDomainId;
	}

	void setTenantPrefixIndex(Reference<TenantPrefixIndex> tenantPrefixIndex) override {
		this->tenantPrefixIndex = tenantPrefixIndex;
	}

private:
	EncryptCipherDomainNameRef getDomainName(int64_t domainId) {
		if (domainId == SYSTEM_KEYSPACE_ENCRYPT_DOMAIN_ID) {
			return FDB_SYSTEM_KEYSPACE_ENCRYPT_DOMAIN_NAME;
		}
		if (domainId == FDB_DEFAULT_ENCRYPT_DOMAIN_ID) {
			return FDB_DEFAULT_ENCRYPT_DOMAIN_NAME;
		}
		if (tenantPrefixIndex.isValid()) {
			StringRef prefix = TenantMapEntry::idToPrefix(domainId);
			auto view = tenantPrefixIndex->atLatest();
			auto itr = view.find(prefix);
			if (itr != view.end()) {
				return *itr;
			}
		}
		TraceEvent(SevWarn, "TenantAwareEncryptionKeyProvider_TenantNotFoundForDomain").detail("DomainId", domainId);
		throw tenant_not_found();
	}

	Reference<AsyncVar<ServerDBInfo> const> db;
	Reference<TenantPrefixIndex> tenantPrefixIndex;
};

#include "flow/unactorcompiler.h"
#endif