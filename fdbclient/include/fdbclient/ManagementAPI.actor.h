/*
 * ManagementAPI.actor.h
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

#pragma once
#if defined(NO_INTELLISENSE) && !defined(FDBCLIENT_MANAGEMENT_API_ACTOR_G_H)
#define FDBCLIENT_MANAGEMENT_API_ACTOR_G_H
#include "fdbclient/ManagementAPI.actor.g.h"
#elif !defined(FDBCLIENT_MANAGEMENT_API_ACTOR_H)
#define FDBCLIENT_MANAGEMENT_API_ACTOR_H

/* This file defines "management" interfaces for configuration, coordination changes, and
the inclusion and exclusion of servers. It is used to implement fdbcli management commands
and by test workloads that simulate such. It isn't exposed to C clients or anywhere outside
our code base and doesn't need to be versioned. It doesn't do anything you can't do with the
standard API and some knowledge of the contents of the system key space.
*/

#include <string>
#include <map>
#include "fdbclient/GenericManagementAPI.actor.h"
#include "fdbclient/NativeAPI.actor.h"
#include "fdbclient/ReadYourWrites.h"
#include "fdbclient/DatabaseConfiguration.h"
#include "fdbclient/MonitorLeader.h"
#include "flow/actorcompiler.h" // has to be last include

ACTOR Future<DatabaseConfiguration> getDatabaseConfiguration(Transaction* tr);
ACTOR Future<DatabaseConfiguration> getDatabaseConfiguration(Database cx);
ACTOR Future<Void> waitForFullReplication(Database cx);

struct IQuorumChange : ReferenceCounted<IQuorumChange> {
	virtual ~IQuorumChange() {}
	virtual Future<std::vector<NetworkAddress>> getDesiredCoordinators(Transaction* tr,
	                                                                   std::vector<NetworkAddress> oldCoordinators,
	                                                                   Reference<IClusterConnectionRecord>,
	                                                                   CoordinatorsResult&) = 0;
	virtual std::string getDesiredClusterKeyName() const { return std::string(); }
};

// Change to use the given set of coordination servers
ACTOR Future<Optional<CoordinatorsResult>> changeQuorumChecker(Transaction* tr,
                                                               ClusterConnectionString* conn,
                                                               std::string newName,
                                                               bool disableConfigDB);
ACTOR Future<CoordinatorsResult> changeQuorum(Database cx, Reference<IQuorumChange> change);
Reference<IQuorumChange> autoQuorumChange(int desired = -1);
Reference<IQuorumChange> nameQuorumChange(std::string const& name, Reference<IQuorumChange> const& other);

// Exclude the given set of servers from use as state servers.  Returns as soon as the change is durable, without
// necessarily waiting for the servers to be evacuated.  A NetworkAddress with a port of 0 means all servers on the
// given IP.
ACTOR Future<Void> excludeServers(Database cx, std::vector<AddressExclusion> servers, bool failed = false);
void excludeServers(Transaction& tr, std::vector<AddressExclusion>& servers, bool failed = false);

// Exclude the servers matching the given set of localities from use as state servers.  Returns as soon as the change
// is durable, without necessarily waiting for the servers to be evacuated.
ACTOR Future<Void> excludeLocalities(Database cx, std::unordered_set<std::string> localities, bool failed = false);
void excludeLocalities(Transaction& tr, std::unordered_set<std::string> localities, bool failed = false);

// Remove the given servers from the exclusion list.  A NetworkAddress with a port of 0 means all servers on the given
// IP.  A NetworkAddress() means all servers (don't exclude anything)
ACTOR Future<Void> includeServers(Database cx, std::vector<AddressExclusion> servers, bool failed = false);

// Remove the given localities from the exclusion list.
ACTOR Future<Void> includeLocalities(Database cx,
                                     std::vector<std::string> localities,
                                     bool failed = false,
                                     bool includeAll = false);

// Set the process class of processes with the given address.  A NetworkAddress with a port of 0 means all servers on
// the given IP.
ACTOR Future<Void> setClass(Database cx, AddressExclusion server, ProcessClass processClass);

// Get the current list of excluded servers
ACTOR Future<std::vector<AddressExclusion>> getExcludedServers(Database cx);
ACTOR Future<std::vector<AddressExclusion>> getExcludedServers(Transaction* tr);

// Get the current list of excluded localities
ACTOR Future<std::vector<std::string>> getExcludedLocalities(Database cx);
ACTOR Future<std::vector<std::string>> getExcludedLocalities(Transaction* tr);

std::set<AddressExclusion> getAddressesByLocality(const std::vector<ProcessData>& workers, const std::string& locality);

// Check for the given, previously excluded servers to be evacuated (no longer used for state).  If waitForExclusion is
// true, this actor returns once it is safe to shut down all such machines without impacting fault tolerance, until and
// unless any of them are explicitly included with includeServers()
ACTOR Future<std::set<NetworkAddress>> checkForExcludingServers(Database cx,
                                                                std::vector<AddressExclusion> servers,
                                                                bool waitForAllExcluded);
ACTOR Future<bool> checkForExcludingServersTxActor(ReadYourWritesTransaction* tr,
                                                   std::set<AddressExclusion>* exclusions,
                                                   std::set<NetworkAddress>* inProgressExclusion);

// Gets a list of all workers in the cluster (excluding testers)
ACTOR Future<std::vector<ProcessData>> getWorkers(Database cx);
ACTOR Future<std::vector<ProcessData>> getWorkers(Transaction* tr);

ACTOR Future<Void> timeKeeperSetDisable(Database cx);

ACTOR Future<Void> lockDatabase(Transaction* tr, UID id);
ACTOR Future<Void> lockDatabase(Reference<ReadYourWritesTransaction> tr, UID id);
ACTOR Future<Void> lockDatabase(Database cx, UID id);

ACTOR Future<Void> unlockDatabase(Transaction* tr, UID id);
ACTOR Future<Void> unlockDatabase(Reference<ReadYourWritesTransaction> tr, UID id);
ACTOR Future<Void> unlockDatabase(Database cx, UID id);

ACTOR Future<Void> checkDatabaseLock(Transaction* tr, UID id);
ACTOR Future<Void> checkDatabaseLock(Reference<ReadYourWritesTransaction> tr, UID id);

ACTOR Future<Void> updateChangeFeed(Transaction* tr, Key rangeID, ChangeFeedStatus status, KeyRange range = KeyRange());
ACTOR Future<Void> updateChangeFeed(Reference<ReadYourWritesTransaction> tr,
                                    Key rangeID,
                                    ChangeFeedStatus status,
                                    KeyRange range = KeyRange());
ACTOR Future<Void> updateChangeFeed(Database cx, Key rangeID, ChangeFeedStatus status, KeyRange range = KeyRange());

ACTOR Future<Void> advanceVersion(Database cx, Version v);

ACTOR Future<int> setDDMode(Database cx, int mode);

ACTOR Future<Void> forceRecovery(Reference<IClusterConnectionRecord> clusterFile, Standalone<StringRef> dcId);

// Start an audit on range of the specific type.
ACTOR Future<UID> auditStorage(Reference<IClusterConnectionRecord> clusterFile, KeyRange range, AuditType type);

ACTOR Future<Void> printHealthyZone(Database cx);
ACTOR Future<bool> clearHealthyZone(Database cx, bool printWarning = false, bool clearSSFailureZoneString = false);
ACTOR Future<bool> setHealthyZone(Database cx, StringRef zoneId, double seconds, bool printWarning = false);

ACTOR Future<Void> waitForPrimaryDC(Database cx, StringRef dcId);

// Gets the cluster connection string
ACTOR Future<Optional<ClusterConnectionString>> getConnectionString(Database cx);

void schemaCoverage(std::string const& spath, bool covered = true);
bool schemaMatch(json_spirit::mValue const& schema,
                 json_spirit::mValue const& result,
                 std::string& errorStr,
                 Severity sev = SevError,
                 bool checkCoverage = false,
                 std::string path = std::string(),
                 std::string schema_path = std::string());

// execute payload in 'snapCmd' on all the coordinators, TLogs and
// storage nodes
ACTOR Future<Void> mgmtSnapCreate(Database cx, Standalone<StringRef> snapCmd, UID snapUID);

// Set and get the storage quota per tenant
void setStorageQuota(Transaction& tr, StringRef tenantName, int64_t quota);
ACTOR Future<Optional<int64_t>> getStorageQuota(Transaction* tr, StringRef tenantName);

#include "flow/unactorcompiler.h"
#endif
