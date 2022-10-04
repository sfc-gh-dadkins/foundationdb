/*
 * TenantManagementWorkload.actor.cpp
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

#include <cstdint>
#include <limits>
#include "fdbclient/ClientBooleanParams.h"
#include "fdbclient/ClusterConnectionMemoryRecord.h"
#include "fdbclient/FDBOptions.g.h"
#include "fdbclient/GenericManagementAPI.actor.h"
#include "fdbclient/KeyBackedTypes.h"
#include "fdbclient/MetaclusterManagement.actor.h"
#include "fdbclient/ReadYourWrites.h"
#include "fdbclient/RunTransaction.actor.h"
#include "fdbclient/TenantManagement.actor.h"
#include "fdbclient/TenantSpecialKeys.actor.h"
#include "fdbclient/ThreadSafeTransaction.h"
#include "fdbrpc/simulator.h"
#include "fdbserver/workloads/MetaclusterConsistency.actor.h"
#include "fdbserver/workloads/TenantConsistency.actor.h"
#include "fdbserver/workloads/workloads.actor.h"
#include "fdbserver/Knobs.h"
#include "flow/ApiVersion.h"
#include "flow/Error.h"
#include "flow/IRandom.h"
#include "flow/ThreadHelper.actor.h"
#include "flow/flow.h"
#include "libb64/decode.h"
#include "flow/actorcompiler.h" // This must be the last #include.

struct TenantManagementWorkload : TestWorkload {
	struct TenantData {
		int64_t id;
		Optional<TenantGroupName> tenantGroup;
		bool empty;
		bool encrypted;

		TenantData() : id(-1), empty(true) {}
		TenantData(int64_t id, Optional<TenantGroupName> tenantGroup, bool empty, bool encrypted)
		  : id(id), tenantGroup(tenantGroup), empty(empty), encrypted(encrypted) {}
	};

	struct TenantGroupData {
		int64_t tenantCount = 0;
	};

	std::map<TenantName, TenantData> createdTenants;
	std::map<TenantGroupName, TenantGroupData> createdTenantGroups;
	int64_t maxId = -1;

	const Key keyName = "key"_sr;
	const Key testParametersKey = "test_parameters"_sr;
	const Value noTenantValue = "no_tenant"_sr;
	const TenantName tenantNamePrefix = "tenant_management_workload_"_sr;
	const ClusterName dataClusterName = "cluster1"_sr;
	TenantName localTenantNamePrefix;
	TenantName localTenantGroupNamePrefix;

	const Key specialKeysTenantMapPrefix = SpecialKeySpace::getModuleRange(SpecialKeySpace::MODULE::MANAGEMENT)
	                                           .begin.withSuffix(TenantRangeImpl::submoduleRange.begin)
	                                           .withSuffix(TenantRangeImpl::mapSubRange.begin);
	const Key specialKeysTenantConfigPrefix = SpecialKeySpace::getModuleRange(SpecialKeySpace::MODULE::MANAGEMENT)
	                                              .begin.withSuffix(TenantRangeImpl::submoduleRange.begin)
	                                              .withSuffix(TenantRangeImpl::configureSubRange.begin);
	const Key specialKeysTenantRenamePrefix = SpecialKeySpace::getModuleRange(SpecialKeySpace::MODULE::MANAGEMENT)
	                                              .begin.withSuffix(TenantRangeImpl::submoduleRange.begin)
	                                              .withSuffix(TenantRangeImpl::renameSubRange.begin);

	int maxTenants;
	int maxTenantGroups;
	double testDuration;
	bool useMetacluster;
	bool singleClient;

	Version oldestDeletionVersion = 0;
	Version newestDeletionVersion = 0;

	Reference<IDatabase> mvDb;
	Database dataDb;

	// This test exercises multiple different ways to work with tenants
	enum class OperationType {
		// Use the special key-space APIs
		SPECIAL_KEYS,
		// Use the ManagementAPI functions that take a Database object and implement a retry loop
		MANAGEMENT_DATABASE,
		// Use the ManagementAPI functions that take a Transaction object
		MANAGEMENT_TRANSACTION,
		// Use the Metacluster API, if applicable. Note: not all APIs have a metacluster variant,
		// and if there isn't one this will choose one of the other options.
		METACLUSTER
	};

	OperationType randomOperationType() {
		double metaclusterProb = useMetacluster ? 0.9 : 0.1;

		if (deterministicRandom()->random01() < metaclusterProb) {
			return OperationType::METACLUSTER;
		} else {
			return (OperationType)deterministicRandom()->randomInt(0, 3);
		}
	}

	TenantManagementWorkload(WorkloadContext const& wcx) : TestWorkload(wcx) {
		maxTenants = std::min<int>(1e8 - 1, getOption(options, "maxTenants"_sr, 1000));
		maxTenantGroups = std::min<int>(2 * maxTenants, getOption(options, "maxTenantGroups"_sr, 20));
		testDuration = getOption(options, "testDuration"_sr, 120.0);
		singleClient = getOption(options, "singleClient"_sr, false);

		localTenantNamePrefix = format("%stenant_%d_", tenantNamePrefix.toString().c_str(), clientId);
		localTenantGroupNamePrefix = format("%stenantgroup_%d_", tenantNamePrefix.toString().c_str(), clientId);

		bool defaultUseMetacluster = false;
		if (clientId == 0 && g_network->isSimulated() && !g_simulator->extraDatabases.empty()) {
			defaultUseMetacluster = deterministicRandom()->coinflip();
		}

		useMetacluster = getOption(options, "useMetacluster"_sr, defaultUseMetacluster);
	}

	std::string description() const override { return "TenantManagement"; }

	struct TestParameters {
		constexpr static FileIdentifier file_identifier = 1527576;

		bool useMetacluster = false;

		TestParameters() {}
		TestParameters(bool useMetacluster) : useMetacluster(useMetacluster) {}

		template <class Ar>
		void serialize(Ar& ar) {
			serializer(ar, useMetacluster);
		}

		Value encode() const { return ObjectWriter::toValue(*this, Unversioned()); }
		static TestParameters decode(ValueRef const& value) {
			return ObjectReader::fromStringRef<TestParameters>(value, Unversioned());
		}
	};

	Future<Void> setup(Database const& cx) override {
		if (clientId == 0 && g_network->isSimulated() && BUGGIFY) {
			IKnobCollection::getMutableGlobalKnobCollection().setKnob(
			    "max_tenants_per_cluster", KnobValueRef::create(int{ deterministicRandom()->randomInt(20, 100) }));
		}

		if (clientId == 0 || !singleClient) {
			return _setup(cx, this);
		} else {
			return Void();
		}
	}

	ACTOR Future<Void> _setup(Database cx, TenantManagementWorkload* self) {
		Reference<IDatabase> threadSafeHandle =
		    wait(unsafeThreadFutureToFuture(ThreadSafeDatabase::createFromExistingDatabase(cx)));

		MultiVersionApi::api->selectApiVersion(cx->apiVersion.version());
		self->mvDb = MultiVersionDatabase::debugCreateFromExistingDatabase(threadSafeHandle);

		if (self->useMetacluster && self->clientId == 0) {
			wait(success(MetaclusterAPI::createMetacluster(cx.getReference(), "management_cluster"_sr)));

			DataClusterEntry entry;
			entry.capacity.numTenantGroups = 1e9;
			wait(MetaclusterAPI::registerCluster(
			    self->mvDb, self->dataClusterName, g_simulator->extraDatabases[0], entry));
		}

		state Transaction tr(cx);
		if (self->clientId == 0) {
			// Communicates test parameters to all other clients by storing it in a key
			loop {
				try {
					tr.setOption(FDBTransactionOptions::RAW_ACCESS);
					tr.set(self->testParametersKey, TestParameters(self->useMetacluster).encode());
					wait(tr.commit());
					break;
				} catch (Error& e) {
					wait(tr.onError(e));
				}
			}
		} else {
			// Read the parameters chosen and saved by client 0
			loop {
				try {
					tr.setOption(FDBTransactionOptions::RAW_ACCESS);
					Optional<Value> val = wait(tr.get(self->testParametersKey));
					if (val.present()) {
						TestParameters params = TestParameters::decode(val.get());
						self->useMetacluster = params.useMetacluster;
						break;
					}

					wait(delay(1.0));
					tr.reset();
				} catch (Error& e) {
					wait(tr.onError(e));
				}
			}
		}

		if (self->useMetacluster) {
			ASSERT(g_simulator->extraDatabases.size() == 1);
			self->dataDb = Database::createSimulatedExtraDatabase(g_simulator->extraDatabases[0], cx->defaultTenant);
		} else {
			self->dataDb = cx;
		}

		if (self->clientId == 0) {
			// Set a key outside of all tenants to make sure that our tenants aren't writing to the regular key-space
			state Transaction dataTr(self->dataDb);
			loop {
				try {
					dataTr.setOption(FDBTransactionOptions::RAW_ACCESS);
					dataTr.set(self->keyName, self->noTenantValue);
					wait(dataTr.commit());
					break;
				} catch (Error& e) {
					wait(dataTr.onError(e));
				}
			}
		}

		return Void();
	}

	TenantName chooseTenantName(bool allowSystemTenant) {
		TenantName tenant(format(
		    "%s%08d", localTenantNamePrefix.toString().c_str(), deterministicRandom()->randomInt(0, maxTenants)));
		if (allowSystemTenant && deterministicRandom()->random01() < 0.02) {
			tenant = tenant.withPrefix("\xff"_sr);
		}

		return tenant;
	}

	Optional<TenantGroupName> chooseTenantGroup(bool allowSystemTenantGroup, bool allowEmptyGroup = true) {
		Optional<TenantGroupName> tenantGroup;
		if (!allowEmptyGroup || deterministicRandom()->coinflip()) {
			tenantGroup = TenantGroupNameRef(format("%s%08d",
			                                        localTenantGroupNamePrefix.toString().c_str(),
			                                        deterministicRandom()->randomInt(0, maxTenantGroups)));
			if (allowSystemTenantGroup && deterministicRandom()->random01() < 0.02) {
				tenantGroup = tenantGroup.get().withPrefix("\xff"_sr);
			}
		}

		return tenantGroup;
	}

	Future<Optional<TenantMapEntry>> tryGetTenant(TenantName tenantName, OperationType operationType) {
		if (operationType == OperationType::METACLUSTER) {
			return MetaclusterAPI::tryGetTenant(mvDb, tenantName);
		} else {
			return TenantAPI::tryGetTenant(dataDb.getReference(), tenantName);
		}
	}

	// Creates tenant(s) using the specified operation type
	ACTOR static Future<Void> createTenantImpl(Reference<ReadYourWritesTransaction> tr,
	                                           std::map<TenantName, TenantMapEntry> tenantsToCreate,
	                                           OperationType operationType,
	                                           TenantManagementWorkload* self) {
		if (operationType == OperationType::SPECIAL_KEYS) {
			tr->setOption(FDBTransactionOptions::SPECIAL_KEY_SPACE_ENABLE_WRITES);
			for (auto [tenant, entry] : tenantsToCreate) {
				tr->set(self->specialKeysTenantMapPrefix.withSuffix(tenant), ""_sr);
				if (entry.tenantGroup.present()) {
					tr->set(self->specialKeysTenantConfigPrefix.withSuffix(
					            Tuple::makeTuple(tenant, "tenant_group"_sr).pack()),
					        entry.tenantGroup.get());
				}
			}
			wait(tr->commit());
		} else if (operationType == OperationType::MANAGEMENT_DATABASE) {
			ASSERT(tenantsToCreate.size() == 1);
			wait(success(TenantAPI::createTenant(
			    self->dataDb.getReference(), tenantsToCreate.begin()->first, tenantsToCreate.begin()->second)));
		} else if (operationType == OperationType::MANAGEMENT_TRANSACTION) {
			tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
			int64_t _nextId = wait(TenantAPI::getNextTenantId(tr));
			int64_t nextId = _nextId;

			std::vector<Future<Void>> createFutures;
			for (auto [tenant, entry] : tenantsToCreate) {
				entry.setId(nextId++);
				createFutures.push_back(success(TenantAPI::createTenantTransaction(tr, tenant, entry)));
			}
			TenantMetadata::lastTenantId().set(tr, nextId - 1);
			wait(waitForAll(createFutures));
			wait(tr->commit());
		} else {
			ASSERT(tenantsToCreate.size() == 1);
			if (deterministicRandom()->coinflip()) {
				tenantsToCreate.begin()->second.assignedCluster = self->dataClusterName;
			}
			wait(MetaclusterAPI::createTenant(
			    self->mvDb, tenantsToCreate.begin()->first, tenantsToCreate.begin()->second));
		}

		return Void();
	}

	ACTOR static Future<Void> createTenant(TenantManagementWorkload* self) {
		state OperationType operationType = self->randomOperationType();
		int numTenants = 1;

		// For transaction-based operations, test creating multiple tenants in the same transaction
		if (operationType == OperationType::SPECIAL_KEYS || operationType == OperationType::MANAGEMENT_TRANSACTION) {
			numTenants = deterministicRandom()->randomInt(1, 5);
		}

		// Tracks whether any tenant exists in the database or not. This variable is updated if we have to retry
		// the creation.
		state bool alreadyExists = false;

		// True if any tenant name starts with \xff
		state bool hasSystemTenant = false;

		// True if any tenant group name starts with \xff
		state bool hasSystemTenantGroup = false;

		state int newTenants = 0;
		state std::map<TenantName, TenantMapEntry> tenantsToCreate;
		for (int i = 0; i < numTenants; ++i) {
			TenantName tenant = self->chooseTenantName(true);
			while (tenantsToCreate.count(tenant)) {
				tenant = self->chooseTenantName(true);
			}

			TenantMapEntry entry;
			entry.tenantGroup = self->chooseTenantGroup(true);
			if (operationType == OperationType::SPECIAL_KEYS) {
				entry.encrypted = SERVER_KNOBS->ENABLE_ENCRYPTION;
			} else {
				entry.encrypted = deterministicRandom()->coinflip();
			}

			if (self->createdTenants.count(tenant)) {
				alreadyExists = true;
			} else if (!tenantsToCreate.count(tenant)) {
				++newTenants;
			}

			tenantsToCreate[tenant] = entry;
			hasSystemTenant = hasSystemTenant || tenant.startsWith("\xff"_sr);
			hasSystemTenantGroup = hasSystemTenantGroup || entry.tenantGroup.orDefault(""_sr).startsWith("\xff"_sr);
		}

		// If any tenant existed at the start of this function, then we expect the creation to fail or be a no-op,
		// depending on the type of create operation being executed
		state bool existedAtStart = alreadyExists;

		state Reference<ReadYourWritesTransaction> tr = makeReference<ReadYourWritesTransaction>(self->dataDb);
		state int64_t minTenantCount = std::numeric_limits<int64_t>::max();
		state int64_t finalTenantCount = 0;

		loop {
			try {
				// First, attempt to create the tenants
				state bool retried = false;
				loop {
					if (operationType == OperationType::MANAGEMENT_TRANSACTION ||
					    operationType == OperationType::SPECIAL_KEYS) {
						tr->setOption(FDBTransactionOptions::READ_SYSTEM_KEYS);
						wait(store(finalTenantCount, TenantMetadata::tenantCount().getD(tr, Snapshot::False, 0)));
						minTenantCount = std::min(finalTenantCount, minTenantCount);
					}

					try {
						Optional<Void> result = wait(timeout(createTenantImpl(tr, tenantsToCreate, operationType, self),
						                                     deterministicRandom()->randomInt(1, 30)));

						if (result.present()) {
							// Make sure that we had capacity to create the tenants. This cannot be validated for
							// database operations because we cannot determine the tenant count in the same transaction
							// that the tenant is created
							if (operationType == OperationType::SPECIAL_KEYS ||
							    operationType == OperationType::MANAGEMENT_TRANSACTION) {
								ASSERT(minTenantCount + newTenants <= CLIENT_KNOBS->MAX_TENANTS_PER_CLUSTER);
							} else {
								// Database operations shouldn't get here if the tenant already exists
								ASSERT(!alreadyExists);
							}

							break;
						}

						retried = true;
						tr->reset();
					} catch (Error& e) {
						// If we retried the creation after our initial attempt succeeded, then we proceed with the rest
						// of the creation steps normally. Otherwise, the creation happened elsewhere and we failed
						// here, so we can rethrow the error.
						if (e.code() == error_code_tenant_already_exists && !existedAtStart) {
							ASSERT(operationType == OperationType::METACLUSTER ||
							       operationType == OperationType::MANAGEMENT_DATABASE);
							ASSERT(retried);
							break;
						} else {
							throw;
						}
					}

					// Check the state of the first created tenant
					Optional<TenantMapEntry> resultEntry =
					    wait(self->tryGetTenant(tenantsToCreate.begin()->first, operationType));

					if (resultEntry.present()) {
						if (resultEntry.get().tenantState == TenantState::READY) {
							// The tenant now exists, so we will retry and expect the creation to react accordingly
							alreadyExists = true;
						} else {
							// Only a metacluster tenant creation can end up in a partially created state
							// We should be able to retry and pick up where we left off
							ASSERT(operationType == OperationType::METACLUSTER);
							ASSERT(resultEntry.get().tenantState == TenantState::REGISTERING);
						}
					}
				}

				// Check that using the wrong creation type fails depending on whether we are using a metacluster
				ASSERT(self->useMetacluster == (operationType == OperationType::METACLUSTER));

				// Database-based creation modes will fail if the tenant already existed
				if (operationType == OperationType::MANAGEMENT_DATABASE ||
				    operationType == OperationType::METACLUSTER) {
					ASSERT(!existedAtStart);
				}

				// It is not legal to create a tenant or tenant group starting with \xff
				ASSERT(!hasSystemTenant);
				ASSERT(!hasSystemTenantGroup);

				state std::map<TenantName, TenantMapEntry>::iterator tenantItr;
				for (tenantItr = tenantsToCreate.begin(); tenantItr != tenantsToCreate.end(); ++tenantItr) {
					// Ignore any tenants that already existed
					if (self->createdTenants.count(tenantItr->first)) {
						continue;
					}

					// Read the created tenant object and verify that its state is correct
					state Optional<TenantMapEntry> entry = wait(self->tryGetTenant(tenantItr->first, operationType));

					ASSERT(entry.present());
					ASSERT(entry.get().id > self->maxId);
					ASSERT(entry.get().tenantGroup == tenantItr->second.tenantGroup);
					ASSERT(entry.get().tenantState == TenantState::READY);

					if (self->useMetacluster) {
						// In a metacluster, we should also see that the tenant was created on the data cluster
						Optional<TenantMapEntry> dataEntry =
						    wait(TenantAPI::tryGetTenant(self->dataDb.getReference(), tenantItr->first));
						ASSERT(dataEntry.present());
						ASSERT(dataEntry.get().id == entry.get().id);
						ASSERT(dataEntry.get().tenantGroup == entry.get().tenantGroup);
						ASSERT(dataEntry.get().tenantState == TenantState::READY);
					}

					// Update our local tenant state to include the newly created one
					self->maxId = entry.get().id;
					self->createdTenants[tenantItr->first] =
					    TenantData(entry.get().id, tenantItr->second.tenantGroup, true, tenantItr->second.encrypted);

					// If this tenant has a tenant group, create or update the entry for it
					if (tenantItr->second.tenantGroup.present()) {
						self->createdTenantGroups[tenantItr->second.tenantGroup.get()].tenantCount++;
					}

					// Randomly decide to insert a key into the tenant
					state bool insertData = deterministicRandom()->random01() < 0.5;
					if (insertData) {
						state Transaction insertTr(self->dataDb, tenantItr->first);
						loop {
							try {
								// The value stored in the key will be the name of the tenant
								insertTr.set(self->keyName, tenantItr->first);
								wait(insertTr.commit());
								break;
							} catch (Error& e) {
								wait(insertTr.onError(e));
							}
						}

						self->createdTenants[tenantItr->first].empty = false;

						// Make sure that the key inserted correctly concatenates the tenant prefix with the
						// relative key
						state Transaction checkTr(self->dataDb);
						loop {
							try {
								checkTr.setOption(FDBTransactionOptions::RAW_ACCESS);
								Optional<Value> val = wait(checkTr.get(self->keyName.withPrefix(entry.get().prefix)));
								ASSERT(val.present());
								ASSERT(val.get() == tenantItr->first);
								break;
							} catch (Error& e) {
								wait(checkTr.onError(e));
							}
						}
					}

					// Perform some final tenant validation
					wait(checkTenantContents(self, tenantItr->first, self->createdTenants[tenantItr->first]));
				}

				return Void();
			} catch (Error& e) {
				if (e.code() == error_code_invalid_tenant_name) {
					ASSERT(hasSystemTenant);
					return Void();
				} else if (e.code() == error_code_invalid_tenant_group_name) {
					ASSERT(hasSystemTenantGroup);
					return Void();
				} else if (e.code() == error_code_invalid_metacluster_operation) {
					ASSERT(operationType == OperationType::METACLUSTER != self->useMetacluster);
					return Void();
				} else if (e.code() == error_code_cluster_no_capacity) {
					// Confirm that we overshot our capacity. This check cannot be done for database operations
					// because we cannot transactionally get the tenant count with the creation.
					if (operationType == OperationType::MANAGEMENT_TRANSACTION ||
					    operationType == OperationType::SPECIAL_KEYS) {
						ASSERT(finalTenantCount + newTenants > CLIENT_KNOBS->MAX_TENANTS_PER_CLUSTER);
					}
					return Void();
				}

				// Database-based operations should not need to be retried
				else if (operationType == OperationType::MANAGEMENT_DATABASE ||
				         operationType == OperationType::METACLUSTER) {
					if (e.code() == error_code_tenant_already_exists) {
						ASSERT(existedAtStart);
					} else {
						ASSERT(tenantsToCreate.size() == 1);
						TraceEvent(SevError, "CreateTenantFailure")
						    .error(e)
						    .detail("TenantName", tenantsToCreate.begin()->first);
					}
					return Void();
				}

				// Transaction-based operations should be retried
				else {
					try {
						wait(tr->onError(e));
					} catch (Error& e) {
						for (auto [tenant, _] : tenantsToCreate) {
							TraceEvent(SevError, "CreateTenantFailure").error(e).detail("TenantName", tenant);
						}
						return Void();
					}
				}
			}
		}
	}

	// Deletes the tenant or tenant range using the specified operation type
	ACTOR static Future<Void> deleteTenantImpl(Reference<ReadYourWritesTransaction> tr,
	                                           TenantName beginTenant,
	                                           Optional<TenantName> endTenant,
	                                           std::vector<TenantName> tenants,
	                                           OperationType operationType,
	                                           TenantManagementWorkload* self) {
		state int tenantIndex;
		if (operationType == OperationType::SPECIAL_KEYS) {
			tr->setOption(FDBTransactionOptions::SPECIAL_KEY_SPACE_ENABLE_WRITES);
			Key key = self->specialKeysTenantMapPrefix.withSuffix(beginTenant);
			if (endTenant.present()) {
				tr->clear(KeyRangeRef(key, self->specialKeysTenantMapPrefix.withSuffix(endTenant.get())));
			} else {
				tr->clear(key);
			}
			wait(tr->commit());
		} else if (operationType == OperationType::MANAGEMENT_DATABASE) {
			ASSERT(!endTenant.present() && tenants.size() == 1);
			wait(TenantAPI::deleteTenant(self->dataDb.getReference(), beginTenant));
		} else if (operationType == OperationType::MANAGEMENT_TRANSACTION) {
			tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
			std::vector<Future<Void>> deleteFutures;
			for (tenantIndex = 0; tenantIndex != tenants.size(); ++tenantIndex) {
				deleteFutures.push_back(TenantAPI::deleteTenantTransaction(tr, tenants[tenantIndex]));
			}

			wait(waitForAll(deleteFutures));
			wait(tr->commit());
		} else {
			ASSERT(!endTenant.present() && tenants.size() == 1);
			wait(MetaclusterAPI::deleteTenant(self->mvDb, beginTenant));
		}

		return Void();
	}

	// Returns GRV and eats GRV errors
	ACTOR static Future<Version> getReadVersion(Reference<ReadYourWritesTransaction> tr) {
		loop {
			try {
				Version version = wait(tr->getReadVersion());
				return version;
			} catch (Error& e) {
				if (e.code() == error_code_grv_proxy_memory_limit_exceeded ||
				    e.code() == error_code_batch_transaction_throttled) {
					wait(tr->onError(e));
				} else {
					throw;
				}
			}
		}
	}

	ACTOR static Future<Void> deleteTenant(TenantManagementWorkload* self) {
		state TenantName beginTenant = self->chooseTenantName(true);
		state OperationType operationType = self->randomOperationType();
		state Reference<ReadYourWritesTransaction> tr = makeReference<ReadYourWritesTransaction>(self->dataDb);

		// For transaction-based deletion, we randomly allow the deletion of a range of tenants
		state Optional<TenantName> endTenant =
		    operationType != OperationType::MANAGEMENT_DATABASE && operationType != OperationType::METACLUSTER &&
		            !beginTenant.startsWith("\xff"_sr) && deterministicRandom()->random01() < 0.2
		        ? Optional<TenantName>(self->chooseTenantName(false))
		        : Optional<TenantName>();

		if (endTenant.present() && endTenant < beginTenant) {
			TenantName temp = beginTenant;
			beginTenant = endTenant.get();
			endTenant = temp;
		}

		auto itr = self->createdTenants.find(beginTenant);

		// True if the beginTenant should exist and be deletable. This is updated if a deletion fails and gets
		// retried.
		state bool alreadyExists = itr != self->createdTenants.end();

		// True if the beginTenant existed at the start of this function
		state bool existedAtStart = alreadyExists;

		// True if all of the tenants in the range are empty and can be deleted
		state bool isEmpty = true;

		// True if we expect that some tenant will be deleted
		state bool anyExists = alreadyExists;

		// Collect a list of all tenants that we expect should be deleted by this operation
		state std::vector<TenantName> tenants;
		if (!endTenant.present()) {
			tenants.push_back(beginTenant);
		} else if (endTenant.present()) {
			for (auto itr = self->createdTenants.lower_bound(beginTenant);
			     itr != self->createdTenants.end() && itr->first < endTenant.get();
			     ++itr) {
				tenants.push_back(itr->first);
				anyExists = true;
			}
		}

		// Check whether each tenant is empty.
		state int tenantIndex;
		try {
			if (alreadyExists || endTenant.present()) {
				for (tenantIndex = 0; tenantIndex < tenants.size(); ++tenantIndex) {
					// For most tenants, we will delete the contents and make them empty
					if (deterministicRandom()->random01() < 0.9) {
						state Transaction clearTr(self->dataDb, tenants[tenantIndex]);
						loop {
							try {
								clearTr.clear(self->keyName);
								wait(clearTr.commit());
								auto itr = self->createdTenants.find(tenants[tenantIndex]);
								ASSERT(itr != self->createdTenants.end());
								itr->second.empty = true;
								break;
							} catch (Error& e) {
								wait(clearTr.onError(e));
							}
						}
					}
					// Otherwise, we will just report the current emptiness of the tenant
					else {
						auto itr = self->createdTenants.find(tenants[tenantIndex]);
						ASSERT(itr != self->createdTenants.end());
						isEmpty = isEmpty && itr->second.empty;
					}
				}
			}
		} catch (Error& e) {
			TraceEvent(SevError, "DeleteTenantFailure")
			    .error(e)
			    .detail("TenantName", beginTenant)
			    .detail("EndTenant", endTenant);
			return Void();
		}

		loop {
			try {
				// Attempt to delete the tenant(s)
				state bool retried = false;
				loop {
					try {
						state Version beforeVersion = wait(self->getReadVersion(tr));
						Optional<Void> result =
						    wait(timeout(deleteTenantImpl(tr, beginTenant, endTenant, tenants, operationType, self),
						                 deterministicRandom()->randomInt(1, 30)));

						if (result.present()) {
							if (anyExists) {
								if (self->oldestDeletionVersion == 0 && !tenants.empty()) {
									tr->reset();
									Version afterVersion = wait(self->getReadVersion(tr));
									self->oldestDeletionVersion = afterVersion;
								}
								self->newestDeletionVersion = beforeVersion;
							}

							// Database operations shouldn't get here if the tenant didn't exist
							ASSERT(operationType == OperationType::SPECIAL_KEYS ||
							       operationType == OperationType::MANAGEMENT_TRANSACTION || alreadyExists);
							break;
						}

						retried = true;
						tr->reset();
					} catch (Error& e) {
						// If we retried the deletion after our initial attempt succeeded, then we proceed with the
						// rest of the deletion steps normally. Otherwise, the deletion happened elsewhere and we
						// failed here, so we can rethrow the error.
						if (e.code() == error_code_tenant_not_found && existedAtStart) {
							ASSERT(operationType == OperationType::METACLUSTER ||
							       operationType == OperationType::MANAGEMENT_DATABASE);
							ASSERT(retried);
							break;
						} else if (e.code() == error_code_grv_proxy_memory_limit_exceeded ||
						           e.code() == error_code_batch_transaction_throttled) {
							// GRV proxy returns an error
							wait(tr->onError(e));
							continue;
						} else {
							throw;
						}
					}

					if (!tenants.empty()) {
						// Check the state of the first deleted tenant
						Optional<TenantMapEntry> resultEntry =
						    wait(self->tryGetTenant(*tenants.begin(), operationType));

						if (!resultEntry.present()) {
							alreadyExists = false;
						} else if (resultEntry.get().tenantState == TenantState::REMOVING) {
							ASSERT(operationType == OperationType::METACLUSTER);
						} else {
							ASSERT(resultEntry.get().tenantState == TenantState::READY);
						}
					}
				}

				// The management transaction operation is a no-op if there are no tenants to delete in a range
				// delete
				if (tenants.size() == 0 && operationType == OperationType::MANAGEMENT_TRANSACTION) {
					return Void();
				}

				// The special keys operation is a no-op if the begin and end tenant are equal (i.e. the range is
				// empty)
				if (endTenant.present() && beginTenant == endTenant.get() &&
				    operationType == OperationType::SPECIAL_KEYS) {
					return Void();
				}

				// Check that using the wrong deletion type fails depending on whether we are using a metacluster
				ASSERT(self->useMetacluster == (operationType == OperationType::METACLUSTER));

				// Transaction-based operations do not fail if the tenant isn't present. If we attempted to delete a
				// single tenant that didn't exist, we can just return.
				if (!existedAtStart && !endTenant.present() &&
				    (operationType == OperationType::MANAGEMENT_TRANSACTION ||
				     operationType == OperationType::SPECIAL_KEYS)) {
					return Void();
				}

				ASSERT(existedAtStart || endTenant.present());

				// Deletion should not succeed if any tenant in the range wasn't empty
				ASSERT(isEmpty);

				// Update our local state to remove the deleted tenants
				for (auto tenant : tenants) {
					auto itr = self->createdTenants.find(tenant);
					ASSERT(itr != self->createdTenants.end());

					// If the tenant group has no tenants remaining, stop tracking it
					if (itr->second.tenantGroup.present()) {
						auto tenantGroupItr = self->createdTenantGroups.find(itr->second.tenantGroup.get());
						ASSERT(tenantGroupItr != self->createdTenantGroups.end());
						if (--tenantGroupItr->second.tenantCount == 0) {
							self->createdTenantGroups.erase(tenantGroupItr);
						}
					}

					self->createdTenants.erase(tenant);
				}
				return Void();
			} catch (Error& e) {
				if (e.code() == error_code_tenant_not_empty) {
					ASSERT(!isEmpty);
					return Void();
				} else if (e.code() == error_code_invalid_metacluster_operation) {
					ASSERT(operationType == OperationType::METACLUSTER != self->useMetacluster);
					return Void();
				}

				// Database-based operations do not need to be retried
				else if (operationType == OperationType::MANAGEMENT_DATABASE ||
				         operationType == OperationType::METACLUSTER) {
					if (e.code() == error_code_tenant_not_found) {
						ASSERT(!existedAtStart && !endTenant.present());
					} else {
						TraceEvent(SevError, "DeleteTenantFailure")
						    .error(e)
						    .detail("TenantName", beginTenant)
						    .detail("EndTenant", endTenant);
					}
					return Void();
				}

				// Transaction-based operations should be retried
				else {
					try {
						wait(tr->onError(e));
					} catch (Error& e) {
						TraceEvent(SevError, "DeleteTenantFailure")
						    .error(e)
						    .detail("TenantName", beginTenant)
						    .detail("EndTenant", endTenant);
						return Void();
					}
				}
			}
		}
	}

	// Performs some validation on a tenant's contents
	ACTOR static Future<Void> checkTenantContents(TenantManagementWorkload* self,
	                                              TenantName tenant,
	                                              TenantData tenantData) {
		state Transaction tr(self->dataDb, tenant);
		loop {
			try {
				// We only every store a single key in each tenant. Therefore we expect a range read of the entire
				// tenant to return either 0 or 1 keys, depending on whether that key has been set.
				state RangeResult result = wait(tr.getRange(KeyRangeRef(""_sr, "\xff"_sr), 2));

				// An empty tenant should have no data
				if (tenantData.empty) {
					ASSERT(result.size() == 0);
				}
				// A non-empty tenant should have our single key. The value of that key should be the name of the
				// tenant.
				else {
					ASSERT(result.size() == 1);
					ASSERT(result[0].key == self->keyName);
					ASSERT(result[0].value == tenant);
				}
				break;
			} catch (Error& e) {
				wait(tr.onError(e));
			}
		}

		return Void();
	}

	// Convert the JSON document returned by the special-key space when reading tenant metadata
	// into a TenantMapEntry
	static TenantMapEntry jsonToTenantMapEntry(ValueRef tenantJson) {
		json_spirit::mValue jsonObject;
		json_spirit::read_string(tenantJson.toString(), jsonObject);
		JSONDoc jsonDoc(jsonObject);

		int64_t id;

		std::string prefix;
		std::string base64Prefix;
		std::string printablePrefix;
		std::string tenantStateStr;
		std::string base64TenantGroup;
		std::string printableTenantGroup;
		bool encrypted;
		std::string assignedClusterStr;

		jsonDoc.get("id", id);
		jsonDoc.get("prefix.base64", base64Prefix);
		jsonDoc.get("prefix.printable", printablePrefix);
		jsonDoc.get("prefix.encrypted", encrypted);

		prefix = base64::decoder::from_string(base64Prefix);
		ASSERT(prefix == unprintable(printablePrefix));

		jsonDoc.get("tenant_state", tenantStateStr);

		Optional<TenantGroupName> tenantGroup;
		if (jsonDoc.tryGet("tenant_group.base64", base64TenantGroup)) {
			jsonDoc.get("tenant_group.printable", printableTenantGroup);
			std::string tenantGroupStr = base64::decoder::from_string(base64TenantGroup);
			ASSERT(tenantGroupStr == unprintable(printableTenantGroup));
			tenantGroup = TenantGroupNameRef(tenantGroupStr);
		}

		Optional<ClusterName> assignedCluster;
		if (jsonDoc.tryGet("assigned_cluster", assignedClusterStr)) {
			assignedCluster = ClusterNameRef(assignedClusterStr);
		}

		TenantMapEntry entry(id, TenantMapEntry::stringToTenantState(tenantStateStr), tenantGroup, encrypted);
		ASSERT(entry.prefix == prefix);
		return entry;
	}

	// Gets the metadata for a tenant using the specified operation type
	ACTOR static Future<TenantMapEntry> getTenantImpl(Reference<ReadYourWritesTransaction> tr,
	                                                  TenantName tenant,
	                                                  OperationType operationType,
	                                                  TenantManagementWorkload* self) {
		state TenantMapEntry entry;
		if (operationType == OperationType::SPECIAL_KEYS) {
			Key key = self->specialKeysTenantMapPrefix.withSuffix(tenant);
			Optional<Value> value = wait(tr->get(key));
			if (!value.present()) {
				throw tenant_not_found();
			}
			entry = TenantManagementWorkload::jsonToTenantMapEntry(value.get());
		} else if (operationType == OperationType::MANAGEMENT_DATABASE) {
			wait(store(entry, TenantAPI::getTenant(self->dataDb.getReference(), tenant)));
		} else if (operationType == OperationType::MANAGEMENT_TRANSACTION) {
			tr->setOption(FDBTransactionOptions::READ_SYSTEM_KEYS);
			wait(store(entry, TenantAPI::getTenantTransaction(tr, tenant)));
		} else {
			wait(store(entry, MetaclusterAPI::getTenant(self->mvDb, tenant)));
		}

		return entry;
	}

	ACTOR static Future<Void> getTenant(TenantManagementWorkload* self) {
		state TenantName tenant = self->chooseTenantName(true);
		state OperationType operationType = self->randomOperationType();
		state Reference<ReadYourWritesTransaction> tr = makeReference<ReadYourWritesTransaction>(self->dataDb);

		// True if the tenant should should exist and return a result
		auto itr = self->createdTenants.find(tenant);
		state bool alreadyExists = itr != self->createdTenants.end() &&
		                           !(operationType == OperationType::METACLUSTER && !self->useMetacluster);
		state TenantData tenantData = alreadyExists ? itr->second : TenantData();

		loop {
			try {
				// Get the tenant metadata and check that it matches our local state
				state TenantMapEntry entry = wait(getTenantImpl(tr, tenant, operationType, self));
				ASSERT(alreadyExists);
				ASSERT(entry.id == tenantData.id);
				ASSERT(entry.tenantGroup == tenantData.tenantGroup);
				wait(checkTenantContents(self, tenant, tenantData));
				return Void();
			} catch (Error& e) {
				state bool retry = false;
				state Error error = e;

				if (e.code() == error_code_tenant_not_found) {
					ASSERT(!alreadyExists);
					return Void();
				}

				// Transaction-based operations should retry
				else if (operationType == OperationType::MANAGEMENT_TRANSACTION ||
				         operationType == OperationType::SPECIAL_KEYS) {
					try {
						retry = true;
						wait(tr->onError(e));
						retry = true;
					} catch (Error& e) {
						error = e;
						retry = false;
					}
				}

				if (!retry) {
					TraceEvent(SevError, "GetTenantFailure").error(error).detail("TenantName", tenant);
					return Void();
				}
			}
		}
	}

	// Gets a list of tenants using the specified operation type
	ACTOR static Future<std::vector<std::pair<TenantName, TenantMapEntry>>> listTenantsImpl(
	    Reference<ReadYourWritesTransaction> tr,
	    TenantName beginTenant,
	    TenantName endTenant,
	    int limit,
	    OperationType operationType,
	    TenantManagementWorkload* self) {
		state std::vector<std::pair<TenantName, TenantMapEntry>> tenants;

		if (operationType == OperationType::SPECIAL_KEYS) {
			KeyRange range = KeyRangeRef(beginTenant, endTenant).withPrefix(self->specialKeysTenantMapPrefix);
			RangeResult results = wait(tr->getRange(range, limit));
			for (auto result : results) {
				tenants.push_back(std::make_pair(result.key.removePrefix(self->specialKeysTenantMapPrefix),
				                                 TenantManagementWorkload::jsonToTenantMapEntry(result.value)));
			}
		} else if (operationType == OperationType::MANAGEMENT_DATABASE) {
			wait(store(tenants, TenantAPI::listTenants(self->dataDb.getReference(), beginTenant, endTenant, limit)));
		} else if (operationType == OperationType::MANAGEMENT_TRANSACTION) {
			tr->setOption(FDBTransactionOptions::READ_SYSTEM_KEYS);
			wait(store(tenants, TenantAPI::listTenantsTransaction(tr, beginTenant, endTenant, limit)));
		} else {
			wait(store(tenants, MetaclusterAPI::listTenants(self->mvDb, beginTenant, endTenant, limit)));
		}

		return tenants;
	}

	ACTOR static Future<Void> listTenants(TenantManagementWorkload* self) {
		state TenantName beginTenant = self->chooseTenantName(false);
		state TenantName endTenant = self->chooseTenantName(false);
		state int limit = std::min(CLIENT_KNOBS->MAX_TENANTS_PER_CLUSTER + 1,
		                           deterministicRandom()->randomInt(1, self->maxTenants * 2));
		state OperationType operationType = self->randomOperationType();
		state Reference<ReadYourWritesTransaction> tr = makeReference<ReadYourWritesTransaction>(self->dataDb);

		if (beginTenant > endTenant) {
			std::swap(beginTenant, endTenant);
		}

		loop {
			try {
				// Attempt to read the chosen list of tenants
				state std::vector<std::pair<TenantName, TenantMapEntry>> tenants =
				    wait(listTenantsImpl(tr, beginTenant, endTenant, limit, operationType, self));

				// Attempting to read the list of tenants using the metacluster API in a non-metacluster should
				// return nothing in this test
				if (operationType == OperationType::METACLUSTER && !self->useMetacluster) {
					ASSERT(tenants.size() == 0);
					return Void();
				}

				ASSERT(tenants.size() <= limit);

				// Compare the resulting tenant list to the list we expected to get
				auto localItr = self->createdTenants.lower_bound(beginTenant);
				auto tenantMapItr = tenants.begin();
				for (; tenantMapItr != tenants.end(); ++tenantMapItr, ++localItr) {
					ASSERT(localItr != self->createdTenants.end());
					ASSERT(localItr->first == tenantMapItr->first);
				}

				// Make sure the list terminated at the right spot
				ASSERT(tenants.size() == limit || localItr == self->createdTenants.end() ||
				       localItr->first >= endTenant);
				return Void();
			} catch (Error& e) {
				state bool retry = false;
				state Error error = e;

				// Transaction-based operations need to be retried
				if (operationType == OperationType::MANAGEMENT_TRANSACTION ||
				    operationType == OperationType::SPECIAL_KEYS) {
					try {
						retry = true;
						wait(tr->onError(e));
					} catch (Error& e) {
						error = e;
						retry = false;
					}
				}

				if (!retry) {
					TraceEvent(SevError, "ListTenantFailure")
					    .error(error)
					    .detail("BeginTenant", beginTenant)
					    .detail("EndTenant", endTenant);

					return Void();
				}
			}
		}
	}

	// Helper function that checks tenant keyspace and updates internal Tenant Map after a rename operation
	ACTOR static Future<Void> verifyTenantRename(TenantManagementWorkload* self,
	                                             TenantName oldTenantName,
	                                             TenantName newTenantName) {
		state Optional<TenantMapEntry> oldTenantEntry =
		    wait(TenantAPI::tryGetTenant(self->dataDb.getReference(), oldTenantName));
		state Optional<TenantMapEntry> newTenantEntry =
		    wait(TenantAPI::tryGetTenant(self->dataDb.getReference(), newTenantName));
		ASSERT(!oldTenantEntry.present());
		ASSERT(newTenantEntry.present());
		TenantData tData = self->createdTenants[oldTenantName];
		self->createdTenants[newTenantName] = tData;
		self->createdTenants.erase(oldTenantName);
		state Transaction insertTr(self->dataDb, newTenantName);
		if (!tData.empty) {
			loop {
				try {
					insertTr.set(self->keyName, newTenantName);
					wait(insertTr.commit());
					break;
				} catch (Error& e) {
					wait(insertTr.onError(e));
				}
			}
		}
		return Void();
	}

	ACTOR static Future<Void> verifyTenantRenames(TenantManagementWorkload* self,
	                                              std::map<TenantName, TenantName> tenantRenames) {
		state std::map<TenantName, TenantName> tenantRenamesCopy = tenantRenames;
		state std::map<TenantName, TenantName>::iterator iter = tenantRenamesCopy.begin();
		for (; iter != tenantRenamesCopy.end(); ++iter) {
			wait(verifyTenantRename(self, iter->first, iter->second));
			wait(checkTenantContents(self, iter->second, self->createdTenants[iter->second]));
		}
		return Void();
	}

	ACTOR static Future<Void> renameTenantImpl(Reference<ReadYourWritesTransaction> tr,
	                                           OperationType operationType,
	                                           std::map<TenantName, TenantName> tenantRenames,
	                                           bool tenantNotFound,
	                                           bool tenantExists,
	                                           bool tenantOverlap,
	                                           TenantManagementWorkload* self) {
		if (operationType == OperationType::SPECIAL_KEYS) {
			tr->setOption(FDBTransactionOptions::SPECIAL_KEY_SPACE_ENABLE_WRITES);
			for (auto& iter : tenantRenames) {
				tr->set(self->specialKeysTenantRenamePrefix.withSuffix(iter.first), iter.second);
			}
			wait(tr->commit());
		} else if (operationType == OperationType::MANAGEMENT_DATABASE) {
			ASSERT(tenantRenames.size() == 1);
			auto iter = tenantRenames.begin();
			wait(TenantAPI::renameTenant(self->dataDb.getReference(), iter->first, iter->second));
			ASSERT(!tenantNotFound && !tenantExists);
		} else if (operationType == OperationType::MANAGEMENT_TRANSACTION) {
			tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
			std::vector<Future<Void>> renameFutures;
			for (auto& iter : tenantRenames) {
				renameFutures.push_back(success(TenantAPI::renameTenantTransaction(tr, iter.first, iter.second)));
			}
			wait(waitForAll(renameFutures));
			wait(tr->commit());
			ASSERT(!tenantNotFound && !tenantExists);
		} else { // operationType == OperationType::METACLUSTER
			ASSERT(tenantRenames.size() == 1);
			auto iter = tenantRenames.begin();
			wait(MetaclusterAPI::renameTenant(self->mvDb, iter->first, iter->second));
		}
		return Void();
	}

	ACTOR static Future<Void> renameTenant(TenantManagementWorkload* self) {
		state OperationType operationType = self->randomOperationType();
		state int numTenants = 1;
		state Reference<ReadYourWritesTransaction> tr = self->dataDb->createTransaction();

		if (operationType == OperationType::SPECIAL_KEYS || operationType == OperationType::MANAGEMENT_TRANSACTION) {
			numTenants = deterministicRandom()->randomInt(1, 5);
		}

		state std::map<TenantName, TenantName> tenantRenames;
		state std::set<TenantName> allTenantNames;

		// Tenant Error flags
		state bool tenantExists = false;
		state bool tenantNotFound = false;
		state bool tenantOverlap = false;
		state bool unknownResult = false;

		for (int i = 0; i < numTenants; ++i) {
			TenantName oldTenant = self->chooseTenantName(false);
			TenantName newTenant = self->chooseTenantName(false);
			bool checkOverlap =
			    oldTenant == newTenant || allTenantNames.count(oldTenant) || allTenantNames.count(newTenant);
			// renameTenantTransaction does not handle rename collisions:
			// reject the rename here if it has overlap and we are doing a transaction operation
			// and then pick another combination
			if (checkOverlap && operationType == OperationType::MANAGEMENT_TRANSACTION) {
				--i;
				continue;
			}
			tenantOverlap = tenantOverlap || checkOverlap;
			tenantRenames[oldTenant] = newTenant;
			allTenantNames.insert(oldTenant);
			allTenantNames.insert(newTenant);
			if (!self->createdTenants.count(oldTenant)) {
				tenantNotFound = true;
			}
			if (self->createdTenants.count(newTenant)) {
				tenantExists = true;
			}
		}

		loop {
			try {
				wait(renameTenantImpl(
				    tr, operationType, tenantRenames, tenantNotFound, tenantExists, tenantOverlap, self));
				wait(verifyTenantRenames(self, tenantRenames));
				// Check that using the wrong rename API fails depending on whether we are using a metacluster
				ASSERT(self->useMetacluster == (operationType == OperationType::METACLUSTER));
				return Void();
			} catch (Error& e) {
				if (e.code() == error_code_tenant_not_found) {
					if (unknownResult) {
						wait(verifyTenantRenames(self, tenantRenames));
					} else {
						ASSERT(tenantNotFound);
					}
					return Void();
				} else if (e.code() == error_code_tenant_already_exists) {
					if (unknownResult) {
						wait(verifyTenantRenames(self, tenantRenames));
					} else {
						ASSERT(tenantExists);
					}
					return Void();
				} else if (e.code() == error_code_special_keys_api_failure) {
					ASSERT(tenantOverlap);
					return Void();
				} else if (e.code() == error_code_invalid_metacluster_operation) {
					ASSERT(operationType == OperationType::METACLUSTER != self->useMetacluster);
					return Void();
				} else if (e.code() == error_code_cluster_no_capacity) {
					// This error should only occur on metacluster due to the multi-stage process.
					// Having temporary tenants may exceed capacity, so we disallow the rename.
					ASSERT(operationType == OperationType::METACLUSTER);
					return Void();
				} else {
					try {
						// In the case of commit_unknown_result, assume we continue retrying
						// until it's successful. Next loop around may throw error because it's
						// already been moved, so account for that and update internal map as needed.
						if (e.code() == error_code_commit_unknown_result) {
							ASSERT(operationType != OperationType::MANAGEMENT_DATABASE &&
							       operationType != OperationType::METACLUSTER);
							unknownResult = true;
						}
						wait(tr->onError(e));
					} catch (Error& e) {
						TraceEvent(SevError, "RenameTenantFailure")
						    .error(e)
						    .detail("TenantRenames", describe(tenantRenames));
						return Void();
					}
				}
			}
		}
	}

	// Changes the configuration of a tenant
	ACTOR static Future<Void> configureTenantImpl(Reference<ReadYourWritesTransaction> tr,
	                                              TenantName tenant,
	                                              std::map<Standalone<StringRef>, Optional<Value>> configParameters,
	                                              OperationType operationType,
	                                              bool specialKeysUseInvalidTuple,
	                                              TenantManagementWorkload* self) {
		if (operationType == OperationType::SPECIAL_KEYS) {
			tr->setOption(FDBTransactionOptions::SPECIAL_KEY_SPACE_ENABLE_WRITES);
			for (auto const& [config, value] : configParameters) {
				Tuple t;
				if (specialKeysUseInvalidTuple) {
					// Wrong number of items
					if (deterministicRandom()->coinflip()) {
						int numItems = deterministicRandom()->randomInt(0, 3);
						if (numItems > 0) {
							t.append(tenant);
						}
						if (numItems > 1) {
							t.append(config).append(""_sr);
						}
					}
					// Wrong data types
					else {
						if (deterministicRandom()->coinflip()) {
							t.append(0).append(config);
						} else {
							t.append(tenant).append(0);
						}
					}
				} else {
					t.append(tenant).append(config);
				}
				if (value.present()) {
					tr->set(self->specialKeysTenantConfigPrefix.withSuffix(t.pack()), value.get());
				} else {
					tr->clear(self->specialKeysTenantConfigPrefix.withSuffix(t.pack()));
				}
			}

			wait(tr->commit());
			ASSERT(!specialKeysUseInvalidTuple);
		} else if (operationType == OperationType::METACLUSTER) {
			wait(MetaclusterAPI::configureTenant(self->mvDb, tenant, configParameters));
		} else {
			// We don't have a transaction or database variant of this function
			ASSERT(false);
		}

		return Void();
	}

	ACTOR static Future<Void> configureTenant(TenantManagementWorkload* self) {
		state OperationType operationType =
		    deterministicRandom()->coinflip() ? OperationType::SPECIAL_KEYS : OperationType::METACLUSTER;

		state TenantName tenant = self->chooseTenantName(true);
		auto itr = self->createdTenants.find(tenant);
		state bool exists = itr != self->createdTenants.end();
		state Reference<ReadYourWritesTransaction> tr = makeReference<ReadYourWritesTransaction>(self->dataDb);

		state std::map<Standalone<StringRef>, Optional<Value>> configuration;
		state Optional<TenantGroupName> newTenantGroup;

		// If true, the options generated may include an unknown option
		state bool hasInvalidOption = deterministicRandom()->random01() < 0.1;

		// True if any tenant group name starts with \xff
		state bool hasSystemTenantGroup = false;

		state bool specialKeysUseInvalidTuple =
		    operationType == OperationType::SPECIAL_KEYS && deterministicRandom()->random01() < 0.1;

		// Generate a tenant group. Sometimes do this at the same time that we include an invalid option to ensure
		// that the configure function still fails
		if (!hasInvalidOption || deterministicRandom()->coinflip()) {
			newTenantGroup = self->chooseTenantGroup(true);
			hasSystemTenantGroup = hasSystemTenantGroup || newTenantGroup.orDefault(""_sr).startsWith("\xff"_sr);
			configuration["tenant_group"_sr] = newTenantGroup;
		}
		if (hasInvalidOption) {
			configuration["invalid_option"_sr] = ""_sr;
		}

		loop {
			try {
				wait(configureTenantImpl(tr, tenant, configuration, operationType, specialKeysUseInvalidTuple, self));

				ASSERT(exists);
				ASSERT(!hasInvalidOption);
				ASSERT(!hasSystemTenantGroup);
				ASSERT(!specialKeysUseInvalidTuple);

				auto itr = self->createdTenants.find(tenant);
				if (itr->second.tenantGroup.present()) {
					auto tenantGroupItr = self->createdTenantGroups.find(itr->second.tenantGroup.get());
					ASSERT(tenantGroupItr != self->createdTenantGroups.end());
					if (--tenantGroupItr->second.tenantCount == 0) {
						self->createdTenantGroups.erase(tenantGroupItr);
					}
				}
				if (newTenantGroup.present()) {
					self->createdTenantGroups[newTenantGroup.get()].tenantCount++;
				}
				itr->second.tenantGroup = newTenantGroup;
				return Void();
			} catch (Error& e) {
				state Error error = e;
				if (e.code() == error_code_tenant_not_found) {
					ASSERT(!exists);
					return Void();
				} else if (e.code() == error_code_special_keys_api_failure) {
					ASSERT(hasInvalidOption || specialKeysUseInvalidTuple);
					return Void();
				} else if (e.code() == error_code_invalid_tenant_configuration) {
					ASSERT(hasInvalidOption);
					return Void();
				} else if (e.code() == error_code_invalid_metacluster_operation) {
					ASSERT(operationType == OperationType::METACLUSTER != self->useMetacluster);
					return Void();
				} else if (e.code() == error_code_invalid_tenant_group_name) {
					ASSERT(hasSystemTenantGroup);
					return Void();
				}

				try {
					wait(tr->onError(e));
				} catch (Error&) {
					TraceEvent(SevError, "ConfigureTenantFailure").error(error).detail("TenantName", tenant);
					return Void();
				}
			}
		}
	}

	// Gets the metadata for a tenant group using the specified operation type
	ACTOR static Future<Optional<TenantGroupEntry>> getTenantGroupImpl(Reference<ReadYourWritesTransaction> tr,
	                                                                   TenantGroupName tenant,
	                                                                   OperationType operationType,
	                                                                   TenantManagementWorkload* self) {
		state Optional<TenantGroupEntry> entry;
		if (operationType == OperationType::MANAGEMENT_DATABASE) {
			wait(store(entry, TenantAPI::tryGetTenantGroup(self->dataDb.getReference(), tenant)));
		} else if (operationType == OperationType::MANAGEMENT_TRANSACTION ||
		           operationType == OperationType::SPECIAL_KEYS) {
			// There is no special-keys interface for reading tenant groups currently, so read them
			// using the TenantAPI.
			tr->setOption(FDBTransactionOptions::READ_SYSTEM_KEYS);
			wait(store(entry, TenantAPI::tryGetTenantGroupTransaction(tr, tenant)));
		} else {
			wait(store(entry, MetaclusterAPI::tryGetTenantGroup(self->mvDb, tenant)));
		}

		return entry;
	}

	ACTOR static Future<Void> getTenantGroup(TenantManagementWorkload* self) {
		state TenantGroupName tenantGroup = self->chooseTenantGroup(true, false).get();
		state OperationType operationType = self->randomOperationType();
		state Reference<ReadYourWritesTransaction> tr = makeReference<ReadYourWritesTransaction>(self->dataDb);

		// True if the tenant group should should exist and return a result
		auto itr = self->createdTenantGroups.find(tenantGroup);
		state bool alreadyExists = itr != self->createdTenantGroups.end() &&
		                           !(operationType == OperationType::METACLUSTER && !self->useMetacluster);

		loop {
			try {
				// Get the tenant group metadata and check that it matches our local state
				state Optional<TenantGroupEntry> entry = wait(getTenantGroupImpl(tr, tenantGroup, operationType, self));
				ASSERT(alreadyExists == entry.present());
				if (entry.present()) {
					ASSERT(entry.get().assignedCluster.present() == (operationType == OperationType::METACLUSTER));
				}
				return Void();
			} catch (Error& e) {
				state bool retry = false;
				state Error error = e;

				// Transaction-based operations should retry
				if (operationType == OperationType::MANAGEMENT_TRANSACTION ||
				    operationType == OperationType::SPECIAL_KEYS) {
					try {
						wait(tr->onError(e));
						retry = true;
					} catch (Error& e) {
						error = e;
						retry = false;
					}
				}

				if (!retry) {
					TraceEvent(SevError, "GetTenantGroupFailure").error(error).detail("TenantGroupName", tenantGroup);
					return Void();
				}
			}
		}
	}

	// Gets a list of tenant groups using the specified operation type
	ACTOR static Future<std::vector<std::pair<TenantGroupName, TenantGroupEntry>>> listTenantGroupsImpl(
	    Reference<ReadYourWritesTransaction> tr,
	    TenantGroupName beginTenantGroup,
	    TenantGroupName endTenantGroup,
	    int limit,
	    OperationType operationType,
	    TenantManagementWorkload* self) {
		state std::vector<std::pair<TenantGroupName, TenantGroupEntry>> tenantGroups;

		if (operationType == OperationType::MANAGEMENT_DATABASE) {
			wait(store(
			    tenantGroups,
			    TenantAPI::listTenantGroups(self->dataDb.getReference(), beginTenantGroup, endTenantGroup, limit)));
		} else if (operationType == OperationType::MANAGEMENT_TRANSACTION ||
		           operationType == OperationType::SPECIAL_KEYS) {
			tr->setOption(FDBTransactionOptions::READ_SYSTEM_KEYS);
			wait(store(tenantGroups,
			           TenantAPI::listTenantGroupsTransaction(tr, beginTenantGroup, endTenantGroup, limit)));
		} else {
			wait(store(tenantGroups,
			           MetaclusterAPI::listTenantGroups(self->mvDb, beginTenantGroup, endTenantGroup, limit)));
		}

		return tenantGroups;
	}

	ACTOR static Future<Void> listTenantGroups(TenantManagementWorkload* self) {
		state TenantGroupName beginTenantGroup = self->chooseTenantGroup(false, false).get();
		state TenantGroupName endTenantGroup = self->chooseTenantGroup(false, false).get();
		state int limit = std::min(CLIENT_KNOBS->MAX_TENANTS_PER_CLUSTER + 1,
		                           deterministicRandom()->randomInt(1, self->maxTenants * 2));
		state OperationType operationType = self->randomOperationType();
		state Reference<ReadYourWritesTransaction> tr = makeReference<ReadYourWritesTransaction>(self->dataDb);

		if (beginTenantGroup > endTenantGroup) {
			std::swap(beginTenantGroup, endTenantGroup);
		}

		loop {
			try {
				// Attempt to read the chosen list of tenant groups
				state std::vector<std::pair<TenantGroupName, TenantGroupEntry>> tenantGroups =
				    wait(listTenantGroupsImpl(tr, beginTenantGroup, endTenantGroup, limit, operationType, self));

				// Attempting to read the list of tenant groups using the metacluster API in a non-metacluster should
				// return nothing in this test
				if (operationType == OperationType::METACLUSTER && !self->useMetacluster) {
					ASSERT(tenantGroups.size() == 0);
					return Void();
				}

				ASSERT(tenantGroups.size() <= limit);

				// Compare the resulting tenant list to the list we expected to get
				auto localItr = self->createdTenantGroups.lower_bound(beginTenantGroup);
				auto tenantMapItr = tenantGroups.begin();
				for (; tenantMapItr != tenantGroups.end(); ++tenantMapItr, ++localItr) {
					ASSERT(localItr != self->createdTenantGroups.end());
					ASSERT(localItr->first == tenantMapItr->first);
				}

				// Make sure the list terminated at the right spot
				ASSERT(tenantGroups.size() == limit || localItr == self->createdTenantGroups.end() ||
				       localItr->first >= endTenantGroup);
				return Void();
			} catch (Error& e) {
				state bool retry = false;
				state Error error = e;

				// Transaction-based operations need to be retried
				if (operationType == OperationType::MANAGEMENT_TRANSACTION ||
				    operationType == OperationType::SPECIAL_KEYS) {
					try {
						retry = true;
						wait(tr->onError(e));
					} catch (Error& e) {
						error = e;
						retry = false;
					}
				}

				if (!retry) {
					TraceEvent(SevError, "ListTenantGroupFailure")
					    .error(error)
					    .detail("BeginTenant", beginTenantGroup)
					    .detail("EndTenant", endTenantGroup);

					return Void();
				}
			}
		}
	}

	Future<Void> start(Database const& cx) override {
		if (clientId == 0 || !singleClient) {
			return _start(cx, this);
		} else {
			return Void();
		}
	}

	ACTOR Future<Void> _start(Database cx, TenantManagementWorkload* self) {
		state double start = now();

		// Run a random sequence of tenant management operations for the duration of the test
		while (now() < start + self->testDuration) {
			state int operation = deterministicRandom()->randomInt(0, 8);
			if (operation == 0) {
				wait(createTenant(self));
			} else if (operation == 1) {
				wait(deleteTenant(self));
			} else if (operation == 2) {
				wait(getTenant(self));
			} else if (operation == 3) {
				wait(listTenants(self));
			} else if (operation == 4) {
				wait(renameTenant(self));
			} else if (operation == 5) {
				wait(configureTenant(self));
			} else if (operation == 6) {
				wait(getTenantGroup(self));
			} else if (operation == 7) {
				wait(listTenantGroups(self));
			}
		}

		return Void();
	}

	// Verify that the set of tenants in the database matches our local state
	ACTOR static Future<Void> compareTenants(TenantManagementWorkload* self) {
		state std::map<TenantName, TenantData>::iterator localItr = self->createdTenants.begin();
		state std::vector<Future<Void>> checkTenants;
		state TenantName beginTenant = ""_sr.withPrefix(self->localTenantNamePrefix);
		state TenantName endTenant = "\xff\xff"_sr.withPrefix(self->localTenantNamePrefix);

		loop {
			// Read the tenant list from the data cluster.
			state std::vector<std::pair<TenantName, TenantMapEntry>> dataClusterTenants =
			    wait(TenantAPI::listTenants(self->dataDb.getReference(), beginTenant, endTenant, 1000));

			auto dataItr = dataClusterTenants.begin();

			TenantNameRef lastTenant;
			while (dataItr != dataClusterTenants.end()) {
				ASSERT(localItr != self->createdTenants.end());
				ASSERT(dataItr->first == localItr->first);
				ASSERT(dataItr->second.tenantGroup == localItr->second.tenantGroup);
				ASSERT(dataItr->second.encrypted == localItr->second.encrypted);

				checkTenants.push_back(checkTenantContents(self, dataItr->first, localItr->second));
				lastTenant = dataItr->first;

				++localItr;
				++dataItr;
			}

			if (dataClusterTenants.size() < 1000) {
				break;
			} else {
				beginTenant = keyAfter(lastTenant);
			}
		}

		ASSERT(localItr == self->createdTenants.end());
		wait(waitForAll(checkTenants));
		return Void();
	}

	// Check that the given tenant group has the expected number of tenants
	ACTOR template <class DB>
	static Future<Void> checkTenantGroupTenantCount(Reference<DB> db, TenantGroupName tenantGroup, int expectedCount) {
		TenantGroupName const& tenantGroupRef = tenantGroup;
		int const& expectedCountRef = expectedCount;

		KeyBackedSet<Tuple>::RangeResultType tenants =
		    wait(runTransaction(db, [tenantGroupRef, expectedCountRef](Reference<typename DB::TransactionT> tr) {
			    tr->setOption(FDBTransactionOptions::READ_SYSTEM_KEYS);
			    return TenantMetadata::tenantGroupTenantIndex().getRange(tr,
			                                                             Tuple::makeTuple(tenantGroupRef),
			                                                             Tuple::makeTuple(keyAfter(tenantGroupRef)),
			                                                             expectedCountRef + 1);
		    }));

		ASSERT(tenants.results.size() == expectedCount && !tenants.more);
		return Void();
	}

	// Verify that the set of tenants in the database matches our local state
	ACTOR static Future<Void> compareTenantGroups(TenantManagementWorkload* self) {
		state std::map<TenantName, TenantGroupData>::iterator localItr = self->createdTenantGroups.begin();
		state TenantName beginTenantGroup = ""_sr.withPrefix(self->localTenantGroupNamePrefix);
		state TenantName endTenantGroup = "\xff\xff"_sr.withPrefix(self->localTenantGroupNamePrefix);
		state std::vector<Future<Void>> checkTenantGroups;

		loop {
			// Read the tenant group list from the data cluster.
			state KeyBackedRangeResult<std::pair<TenantGroupName, TenantGroupEntry>> dataClusterTenantGroups;
			TenantName const& beginTenantGroupRef = beginTenantGroup;
			TenantName const& endTenantGroupRef = endTenantGroup;
			wait(
			    store(dataClusterTenantGroups,
			          runTransaction(self->dataDb.getReference(),
			                         [beginTenantGroupRef, endTenantGroupRef](Reference<ReadYourWritesTransaction> tr) {
				                         tr->setOption(FDBTransactionOptions::READ_SYSTEM_KEYS);
				                         return TenantMetadata::tenantGroupMap().getRange(
				                             tr, beginTenantGroupRef, endTenantGroupRef, 1000);
			                         })));

			auto dataItr = dataClusterTenantGroups.results.begin();

			TenantGroupNameRef lastTenantGroup;
			while (dataItr != dataClusterTenantGroups.results.end()) {
				ASSERT(localItr != self->createdTenantGroups.end());
				ASSERT(dataItr->first == localItr->first);
				ASSERT(!dataItr->second.assignedCluster.present());
				lastTenantGroup = dataItr->first;

				checkTenantGroups.push_back(checkTenantGroupTenantCount(
				    self->dataDb.getReference(), dataItr->first, localItr->second.tenantCount));

				++localItr;
				++dataItr;
			}

			if (!dataClusterTenantGroups.more) {
				break;
			} else {
				beginTenantGroup = keyAfter(lastTenantGroup);
			}
		}

		ASSERT(localItr == self->createdTenantGroups.end());
		return Void();
	}

	// Check that the tenant tombstones are properly cleaned up
	ACTOR static Future<Void> checkTombstoneCleanup(TenantManagementWorkload* self) {
		state Reference<ReadYourWritesTransaction> tr = self->dataDb->createTransaction();
		loop {
			try {
				tr->setOption(FDBTransactionOptions::READ_SYSTEM_KEYS);

				Optional<TenantTombstoneCleanupData> tombstoneCleanupData =
				    wait(TenantMetadata::tombstoneCleanupData().get(tr));

				if (self->oldestDeletionVersion != 0) {
					ASSERT(tombstoneCleanupData.present());
					if (self->newestDeletionVersion - self->oldestDeletionVersion >
					    CLIENT_KNOBS->TENANT_TOMBSTONE_CLEANUP_INTERVAL * CLIENT_KNOBS->VERSIONS_PER_SECOND) {
						ASSERT(tombstoneCleanupData.get().tombstonesErasedThrough >= 0);
					}
				}
				break;
			} catch (Error& e) {
				wait(tr->onError(e));
			}
		}

		return Void();
	}

	Future<bool> check(Database const& cx) override {
		if (clientId == 0 || !singleClient) {
			return _check(cx, this);
		} else {
			return true;
		}
	}

	ACTOR static Future<bool> _check(Database cx, TenantManagementWorkload* self) {
		state Transaction tr(self->dataDb);

		// Check that the key we set outside of the tenant is present and has the correct value
		// This is the same key we set inside some of our tenants, so this checks that no tenant
		// writes accidentally happened in the raw key-space
		loop {
			try {
				tr.setOption(FDBTransactionOptions::RAW_ACCESS);
				Optional<Value> val = wait(tr.get(self->keyName));
				ASSERT(val.present() && val.get() == self->noTenantValue);
				break;
			} catch (Error& e) {
				wait(tr.onError(e));
			}
		}

		wait(compareTenants(self) && compareTenantGroups(self));

		if (self->useMetacluster) {
			// The metacluster consistency check runs the tenant consistency check for each cluster
			state MetaclusterConsistencyCheck<IDatabase> metaclusterConsistencyCheck(
			    self->mvDb, AllowPartialMetaclusterOperations::False);
			wait(metaclusterConsistencyCheck.run());
			wait(checkTombstoneCleanup(self));
		} else {
			state TenantConsistencyCheck<DatabaseContext> tenantConsistencyCheck(self->dataDb.getReference());
			wait(tenantConsistencyCheck.run());
		}

		return true;
	}

	void getMetrics(std::vector<PerfMetric>& m) override {}
};

WorkloadFactory<TenantManagementWorkload> TenantManagementWorkload("TenantManagement");
