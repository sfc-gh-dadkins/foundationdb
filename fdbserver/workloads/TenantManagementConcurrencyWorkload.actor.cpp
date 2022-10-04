/*
 * TenantManagementConcurrencyWorkload.actor.cpp
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
#include "fdbclient/ClusterConnectionMemoryRecord.h"
#include "fdbclient/FDBOptions.g.h"
#include "fdbclient/GenericManagementAPI.actor.h"
#include "fdbclient/MetaclusterManagement.actor.h"
#include "fdbclient/ReadYourWrites.h"
#include "fdbclient/TenantManagement.actor.h"
#include "fdbclient/ThreadSafeTransaction.h"
#include "fdbrpc/simulator.h"
#include "fdbserver/workloads/MetaclusterConsistency.actor.h"
#include "fdbserver/workloads/TenantConsistency.actor.h"
#include "fdbserver/workloads/workloads.actor.h"
#include "fdbserver/Knobs.h"
#include "flow/Error.h"
#include "flow/IRandom.h"
#include "flow/flow.h"
#include "flow/actorcompiler.h" // This must be the last #include.

struct TenantManagementConcurrencyWorkload : TestWorkload {
	const TenantName tenantNamePrefix = "tenant_management_concurrency_workload_"_sr;
	const Key testParametersKey = "test_parameters"_sr;

	int maxTenants;
	int maxTenantGroups;
	double testDuration;
	bool useMetacluster;

	Reference<IDatabase> mvDb;
	Database dataDb;

	TenantManagementConcurrencyWorkload(WorkloadContext const& wcx) : TestWorkload(wcx) {
		maxTenants = std::min<int>(1e8 - 1, getOption(options, "maxTenants"_sr, 100));
		maxTenantGroups = std::min<int>(2 * maxTenants, getOption(options, "maxTenantGroups"_sr, 20));
		testDuration = getOption(options, "testDuration"_sr, 120.0);

		if (clientId == 0) {
			useMetacluster = deterministicRandom()->coinflip();
		} else {
			// Other clients read the metacluster state from the database
			useMetacluster = false;
		}
	}

	std::string description() const override { return "TenantManagementConcurrency"; }

	void disableFailureInjectionWorkloads(std::set<std::string>& out) const override {
		out.insert("MachineAttritionWorkload");
	}

	struct TestParameters {
		constexpr static FileIdentifier file_identifier = 14350843;

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

		return _setup(cx, this);
	}
	ACTOR static Future<Void> _setup(Database cx, TenantManagementConcurrencyWorkload* self) {
		Reference<IDatabase> threadSafeHandle =
		    wait(unsafeThreadFutureToFuture(ThreadSafeDatabase::createFromExistingDatabase(cx)));

		MultiVersionApi::api->selectApiVersion(cx->apiVersion.version());
		self->mvDb = MultiVersionDatabase::debugCreateFromExistingDatabase(threadSafeHandle);

		if (self->useMetacluster && self->clientId == 0) {
			wait(success(MetaclusterAPI::createMetacluster(cx.getReference(), "management_cluster"_sr)));

			DataClusterEntry entry;
			entry.capacity.numTenantGroups = 1e9;
			wait(MetaclusterAPI::registerCluster(self->mvDb, "cluster1"_sr, g_simulator->extraDatabases[0], entry));
		}

		state Transaction tr(cx);
		if (self->clientId == 0) {
			// Send test parameters to the other clients
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
			// Read the tenant subspace chosen and saved by client 0
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

		return Void();
	}

	TenantName chooseTenantName() {
		TenantName tenant(
		    format("%s%08d", tenantNamePrefix.toString().c_str(), deterministicRandom()->randomInt(0, maxTenants)));

		return tenant;
	}

	Optional<TenantGroupName> chooseTenantGroup() {
		Optional<TenantGroupName> tenantGroup;
		if (deterministicRandom()->coinflip()) {
			tenantGroup =
			    TenantGroupNameRef(format("tenantgroup%08d", deterministicRandom()->randomInt(0, maxTenantGroups)));
		}

		return tenantGroup;
	}

	ACTOR static Future<Void> createTenant(TenantManagementConcurrencyWorkload* self) {
		state TenantName tenant = self->chooseTenantName();
		state TenantMapEntry entry;
		entry.tenantGroup = self->chooseTenantGroup();

		try {
			loop {
				Future<Void> createFuture =
				    self->useMetacluster ? MetaclusterAPI::createTenant(self->mvDb, tenant, entry)
				                         : success(TenantAPI::createTenant(self->dataDb.getReference(), tenant, entry));
				Optional<Void> result = wait(timeout(createFuture, 30));
				if (result.present()) {
					break;
				}
			}

			return Void();
		} catch (Error& e) {
			if (e.code() == error_code_tenant_removed) {
				ASSERT(self->useMetacluster);
			} else if (e.code() != error_code_tenant_already_exists && e.code() != error_code_cluster_no_capacity) {
				TraceEvent(SevError, "CreateTenantFailure").error(e).detail("TenantName", tenant);
				ASSERT(false);
			}

			return Void();
		}
	}

	ACTOR static Future<Void> deleteTenant(TenantManagementConcurrencyWorkload* self) {
		state TenantName tenant = self->chooseTenantName();

		try {
			loop {
				Future<Void> deleteFuture = self->useMetacluster
				                                ? MetaclusterAPI::deleteTenant(self->mvDb, tenant)
				                                : TenantAPI::deleteTenant(self->dataDb.getReference(), tenant);
				Optional<Void> result = wait(timeout(deleteFuture, 30));

				if (result.present()) {
					break;
				}
			}

			return Void();
		} catch (Error& e) {
			if (e.code() != error_code_tenant_not_found) {
				TraceEvent(SevError, "DeleteTenantFailure").error(e).detail("TenantName", tenant);
			}
			return Void();
		}
	}

	ACTOR static Future<Void> configureImpl(TenantManagementConcurrencyWorkload* self,
	                                        TenantName tenant,
	                                        std::map<Standalone<StringRef>, Optional<Value>> configParams) {
		if (self->useMetacluster) {
			wait(MetaclusterAPI::configureTenant(self->mvDb, tenant, configParams));
		} else {
			state Reference<ReadYourWritesTransaction> tr = self->dataDb->createTransaction();
			loop {
				try {
					tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
					TenantMapEntry entry = wait(TenantAPI::getTenantTransaction(tr, tenant));
					TenantMapEntry updatedEntry = entry;
					for (auto param : configParams) {
						updatedEntry.configure(param.first, param.second);
					}
					wait(TenantAPI::configureTenantTransaction(tr, tenant, entry, updatedEntry));
					wait(buggifiedCommit(tr, BUGGIFY_WITH_PROB(0.1)));
					break;
				} catch (Error& e) {
					wait(tr->onError(e));
				}
			}
		}

		return Void();
	}

	ACTOR static Future<Void> configureTenant(TenantManagementConcurrencyWorkload* self) {
		state TenantName tenant = self->chooseTenantName();
		state std::map<Standalone<StringRef>, Optional<Value>> configParams;
		configParams["tenant_group"_sr] = self->chooseTenantGroup();

		try {
			loop {
				Optional<Void> result = wait(timeout(configureImpl(self, tenant, configParams), 30));

				if (result.present()) {
					break;
				}
			}

			return Void();
		} catch (Error& e) {
			if (e.code() != error_code_tenant_not_found && e.code() != error_code_invalid_tenant_state) {
				TraceEvent(SevError, "ConfigureTenantFailure").error(e).detail("TenantName", tenant);
			}
			return Void();
		}
	}

	ACTOR static Future<Void> renameTenant(TenantManagementConcurrencyWorkload* self) {
		state TenantName oldTenant = self->chooseTenantName();
		state TenantName newTenant = self->chooseTenantName();

		try {
			loop {
				Future<Void> renameFuture =
				    self->useMetacluster ? MetaclusterAPI::renameTenant(self->mvDb, oldTenant, newTenant)
				                         : TenantAPI::renameTenant(self->dataDb.getReference(), oldTenant, newTenant);
				Optional<Void> result = wait(timeout(renameFuture, 30));

				if (result.present()) {
					break;
				}
			}

			return Void();
		} catch (Error& e) {
			if (e.code() == error_code_invalid_tenant_state || e.code() == error_code_tenant_removed ||
			    e.code() == error_code_cluster_no_capacity) {
				ASSERT(self->useMetacluster);
			} else if (e.code() != error_code_tenant_not_found && e.code() != error_code_tenant_already_exists) {
				TraceEvent(SevError, "RenameTenantFailure")
				    .error(e)
				    .detail("OldTenant", oldTenant)
				    .detail("NewTenant", newTenant);
			}
			return Void();
		}
	}

	Future<Void> start(Database const& cx) override { return _start(cx, this); }
	ACTOR static Future<Void> _start(Database cx, TenantManagementConcurrencyWorkload* self) {
		state double start = now();

		// Run a random sequence of tenant management operations for the duration of the test
		while (now() < start + self->testDuration) {
			state int operation = deterministicRandom()->randomInt(0, 4);
			if (operation == 0) {
				wait(createTenant(self));
			} else if (operation == 1) {
				wait(deleteTenant(self));
			} else if (operation == 2) {
				wait(configureTenant(self));
			} else if (operation == 3) {
				wait(renameTenant(self));
			}
		}

		return Void();
	}

	Future<bool> check(Database const& cx) override { return _check(cx, this); }
	ACTOR static Future<bool> _check(Database cx, TenantManagementConcurrencyWorkload* self) {
		if (self->useMetacluster) {
			// The metacluster consistency check runs the tenant consistency check for each cluster
			state MetaclusterConsistencyCheck<IDatabase> metaclusterConsistencyCheck(
			    self->mvDb, AllowPartialMetaclusterOperations::True);
			wait(metaclusterConsistencyCheck.run());
		} else {
			state TenantConsistencyCheck<DatabaseContext> tenantConsistencyCheck(self->dataDb.getReference());
			wait(tenantConsistencyCheck.run());
		}

		return true;
	}

	void getMetrics(std::vector<PerfMetric>& m) override {}
};

WorkloadFactory<TenantManagementConcurrencyWorkload> TenantManagementConcurrencyWorkloadFactory(
    "TenantManagementConcurrency");
