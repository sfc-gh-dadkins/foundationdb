/*
 * PhysicalShardMove.cpp
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

#include "fdbclient/ManagementAPI.actor.h"
#include "fdbclient/NativeAPI.actor.h"
#include "fdbrpc/simulator.h"
#include "fdbserver/IKeyValueStore.h"
#include "fdbserver/ServerCheckpoint.actor.h"
#include "fdbserver/MoveKeys.actor.h"
#include "fdbserver/QuietDatabase.h"
#include "fdbserver/workloads/workloads.actor.h"
#include "flow/Error.h"
#include "flow/IRandom.h"
#include "flow/flow.h"
#include <cstdint>
#include <limits>

#include "flow/actorcompiler.h" // This must be the last #include.

namespace {
std::string printValue(const ErrorOr<Optional<Value>>& value) {
	if (value.isError()) {
		return value.getError().name();
	}
	return value.get().present() ? value.get().get().toString() : "Value Not Found.";
}
} // namespace

struct PhysicalShardMoveWorkLoad : TestWorkload {
	FlowLock startMoveKeysParallelismLock;
	FlowLock finishMoveKeysParallelismLock;
	FlowLock cleanUpDataMoveParallelismLock;
	const bool enabled;
	bool pass;

	PhysicalShardMoveWorkLoad(WorkloadContext const& wcx) : TestWorkload(wcx), enabled(!clientId), pass(true) {}

	void validationFailed(ErrorOr<Optional<Value>> expectedValue, ErrorOr<Optional<Value>> actualValue) {
		TraceEvent(SevError, "TestFailed")
		    .detail("ExpectedValue", printValue(expectedValue))
		    .detail("ActualValue", printValue(actualValue));
		pass = false;
	}

	std::string description() const override { return "PhysicalShardMove"; }

	Future<Void> setup(Database const& cx) override { return Void(); }

	Future<Void> start(Database const& cx) override {
		if (!enabled) {
			return Void();
		}
		return _start(this, cx);
	}

	void disableFailureInjectionWorkloads(std::set<std::string>& out) const override { out.insert("MoveKeysWorkload"); }

	ACTOR Future<Void> _start(PhysicalShardMoveWorkLoad* self, Database cx) {
		int ignore = wait(setDDMode(cx, 0));
		state std::map<Key, Value> kvs({ { "TestKeyA"_sr, "TestValueA"_sr },
		                                 { "TestKeyB"_sr, "TestValueB"_sr },
		                                 { "TestKeyC"_sr, "TestValueC"_sr },
		                                 { "TestKeyD"_sr, "TestValueD"_sr },
		                                 { "TestKeyE"_sr, "TestValueE"_sr },
		                                 { "TestKeyF"_sr, "TestValueF"_sr } });

		Version _ = wait(self->populateData(self, cx, &kvs));

		TraceEvent("TestValueWritten").log();

		state std::unordered_set<UID> excludes;
		state std::unordered_set<UID> includes;
		state int teamSize = 1;
		std::vector<UID> teamA = wait(self->moveShard(self,
		                                              cx,
		                                              deterministicRandom()->randomUniqueID(),
		                                              KeyRangeRef("TestKeyA"_sr, "TestKeyF"_sr),
		                                              teamSize,
		                                              includes,
		                                              excludes));
		excludes.insert(teamA.begin(), teamA.end());

		state uint64_t sh0 = deterministicRandom()->randomUInt64();
		state uint64_t sh1 = deterministicRandom()->randomUInt64();
		state uint64_t sh2 = deterministicRandom()->randomUInt64();

		// Move range [TestKeyA, TestKeyB) to sh0.
		state std::vector<UID> teamA = wait(self->moveShard(self,
		                                                    cx,
		                                                    UID(sh0, deterministicRandom()->randomUInt64()),
		                                                    KeyRangeRef("TestKeyA"_sr, "TestKeyB"_sr),
		                                                    teamSize,
		                                                    includes,
		                                                    excludes));

		// Move range [TestKeyB, TestKeyC) to sh1, on the same server.
		includes.insert(teamA.begin(), teamA.end());
		state std::vector<UID> teamB = wait(self->moveShard(self,
		                                                    cx,
		                                                    UID(sh1, deterministicRandom()->randomUInt64()),
		                                                    KeyRangeRef("TestKeyB"_sr, "TestKeyC"_sr),
		                                                    teamSize,
		                                                    includes,
		                                                    excludes));
		ASSERT(std::equal(teamA.begin(), teamA.end(), teamB.begin()));

		state int teamIdx = 0;
		for (teamIdx = 0; teamIdx < teamA.size(); ++teamIdx) {
			std::vector<StorageServerShard> shards =
			    wait(self->getStorageServerShards(cx, teamA[teamIdx], KeyRangeRef("TestKeyA"_sr, "TestKeyC"_sr)));
			ASSERT(shards.size() == 2);
			ASSERT(shards[0].desiredId == sh0);
			ASSERT(shards[1].desiredId == sh1);
			TraceEvent("TestStorageServerShards", teamA[teamIdx]).detail("Shards", describe(shards));
		}

		state std::vector<UID> teamC = wait(self->moveShard(self,
		                                                    cx,
		                                                    UID(sh2, deterministicRandom()->randomUInt64()),
		                                                    KeyRangeRef("TestKeyB"_sr, "TestKeyC"_sr),
		                                                    teamSize,
		                                                    includes,
		                                                    excludes));
		ASSERT(std::equal(teamA.begin(), teamA.end(), teamC.begin()));

		for (teamIdx = 0; teamIdx < teamA.size(); ++teamIdx) {
			std::vector<StorageServerShard> shards =
			    wait(self->getStorageServerShards(cx, teamA[teamIdx], KeyRangeRef("TestKeyA"_sr, "TestKeyC"_sr)));
			ASSERT(shards.size() == 2);
			ASSERT(shards[0].desiredId == sh0);
			ASSERT(shards[1].id == sh1);
			ASSERT(shards[1].desiredId == sh2);
			TraceEvent("TestStorageServerShards", teamA[teamIdx]).detail("Shards", describe(shards));
		}

		wait(self->validateData(self, cx, KeyRangeRef("TestKeyA"_sr, "TestKeyF"_sr), &kvs));
		TraceEvent("TestValueVerified").log();

		int ignore = wait(setDDMode(cx, 1));
		return Void();
	}

	ACTOR Future<Version> populateData(PhysicalShardMoveWorkLoad* self, Database cx, std::map<Key, Value>* kvs) {
		state Reference<ReadYourWritesTransaction> tr = makeReference<ReadYourWritesTransaction>(cx);
		state Version version;
		loop {
			state UID debugID = deterministicRandom()->randomUniqueID();
			try {
				tr->debugTransaction(debugID);
				for (const auto& [key, value] : *kvs) {
					tr->set(key, value);
				}
				wait(tr->commit());
				version = tr->getCommittedVersion();
				break;
			} catch (Error& e) {
				TraceEvent("TestCommitError").errorUnsuppressed(e);
				wait(tr->onError(e));
			}
		}

		TraceEvent("PopulateTestDataDone")
		    .detail("CommitVersion", tr->getCommittedVersion())
		    .detail("DebugID", debugID);

		return version;
	}

	ACTOR Future<Void> validateData(PhysicalShardMoveWorkLoad* self,
	                                Database cx,
	                                KeyRange range,
	                                std::map<Key, Value>* kvs) {
		state Transaction tr(cx);
		loop {
			state UID debugID = deterministicRandom()->randomUniqueID();
			try {
				tr.debugTransaction(debugID);
				RangeResult res = wait(tr.getRange(range, CLIENT_KNOBS->TOO_MANY));
				ASSERT(!res.more && res.size() < CLIENT_KNOBS->TOO_MANY);

				for (const auto& kv : res) {
					ASSERT((*kvs)[kv.key] == kv.value);
				}
				break;
			} catch (Error& e) {
				TraceEvent("TestCommitError").errorUnsuppressed(e);
				wait(tr.onError(e));
			}
		}

		TraceEvent("ValidateTestDataDone").detail("DebugID", debugID);

		return Void();
	}

	ACTOR Future<Void> readAndVerify(PhysicalShardMoveWorkLoad* self,
	                                 Database cx,
	                                 Key key,
	                                 ErrorOr<Optional<Value>> expectedValue) {
		state Transaction tr(cx);
		tr.setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);

		loop {
			try {
				state Version readVersion = wait(tr.getReadVersion());
				state Optional<Value> res = wait(timeoutError(tr.get(key), 30.0));
				const bool equal = !expectedValue.isError() && res == expectedValue.get();
				if (!equal) {
					self->validationFailed(expectedValue, ErrorOr<Optional<Value>>(res));
				}
				break;
			} catch (Error& e) {
				TraceEvent("TestReadError").errorUnsuppressed(e);
				if (expectedValue.isError() && expectedValue.getError().code() == e.code()) {
					break;
				}
				wait(tr.onError(e));
			}
		}

		TraceEvent("TestReadSuccess").detail("Version", readVersion);

		return Void();
	}

	ACTOR Future<Version> writeAndVerify(PhysicalShardMoveWorkLoad* self, Database cx, Key key, Optional<Value> value) {
		// state Transaction tr(cx);
		state Reference<ReadYourWritesTransaction> tr = makeReference<ReadYourWritesTransaction>(cx);
		state Version version;
		loop {
			state UID debugID = deterministicRandom()->randomUniqueID();
			try {
				tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
				tr->debugTransaction(debugID);
				if (value.present()) {
					tr->set(key, value.get());
					tr->set("Test?"_sr, value.get());
					tr->set(key, value.get());
				} else {
					tr->clear(key);
				}
				wait(timeoutError(tr->commit(), 30.0));
				version = tr->getCommittedVersion();
				break;
			} catch (Error& e) {
				TraceEvent("TestCommitError").errorUnsuppressed(e);
				wait(tr->onError(e));
			}
		}

		TraceEvent("TestCommitSuccess").detail("CommitVersion", tr->getCommittedVersion()).detail("DebugID", debugID);

		wait(self->readAndVerify(self, cx, key, value));

		return version;
	}

	// Move keys to a random selected team consisting of a single SS, after disabling DD, so that keys won't be
	// kept in the new team until DD is enabled.
	// Returns the address of the single SS of the new team.
	ACTOR Future<std::vector<UID>> moveShard(PhysicalShardMoveWorkLoad* self,
	                                         Database cx,
	                                         UID dataMoveId,
	                                         KeyRange keys,
	                                         int teamSize,
	                                         std::unordered_set<UID> includes,
	                                         std::unordered_set<UID> excludes) {
		// Disable DD to avoid DD undoing of our move.
		int ignore = wait(setDDMode(cx, 0));

		// Pick a random SS as the dest, keys will reside on a single server after the move.
		std::vector<StorageServerInterface> interfs = wait(getStorageServers(cx));
		ASSERT(interfs.size() > teamSize - includes.size());
		while (includes.size() < teamSize) {
			const auto& interf = interfs[deterministicRandom()->randomInt(0, interfs.size())];
			if (excludes.count(interf.uniqueID) == 0 && includes.count(interf.uniqueID) == 0) {
				includes.insert(interf.uniqueID);
			}
		}

		state std::vector<UID> dests(includes.begin(), includes.end());
		state UID owner = deterministicRandom()->randomUniqueID();
		// state Key ownerKey = "\xff/moveKeysLock/Owner"_sr;
		state DDEnabledState ddEnabledState;

		state Transaction tr(cx);

		loop {
			try {
				TraceEvent("TestMoveShard").detail("Range", keys.toString());
				state MoveKeysLock moveKeysLock = wait(takeMoveKeysLock(cx, owner));

				tr.setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
				state RangeResult dataMoves = wait(tr.getRange(dataMoveKeys, CLIENT_KNOBS->TOO_MANY));
				Version readVersion = wait(tr.getReadVersion());
				TraceEvent("TestMoveShardReadDataMoves")
				    .detail("DataMoves", dataMoves.size())
				    .detail("ReadVersion", readVersion);
				state int i = 0;
				for (; i < dataMoves.size(); ++i) {
					UID dataMoveId = decodeDataMoveKey(dataMoves[i].key);
					state DataMoveMetaData dataMove = decodeDataMoveValue(dataMoves[i].value);
					ASSERT(dataMoveId == dataMove.id);
					TraceEvent("TestCancelDataMoveBegin").detail("DataMove", dataMove.toString());
					wait(cleanUpDataMove(cx,
					                     dataMoveId,
					                     moveKeysLock,
					                     &self->cleanUpDataMoveParallelismLock,
					                     dataMove.range,
					                     &ddEnabledState));
					TraceEvent("TestCancelDataMoveEnd").detail("DataMove", dataMove.toString());
				}

				TraceEvent("TestMoveShardStartMoveKeys").detail("DataMove", dataMoveId);
				wait(moveKeys(cx,
				              MoveKeysParams{ dataMoveId,
				                              keys,
				                              dests,
				                              dests,
				                              moveKeysLock,
				                              Promise<Void>(),
				                              &self->startMoveKeysParallelismLock,
				                              &self->finishMoveKeysParallelismLock,
				                              false,
				                              deterministicRandom()->randomUniqueID(), // for logging only
				                              &ddEnabledState }));
				break;
			} catch (Error& e) {
				if (e.code() == error_code_movekeys_conflict) {
					// Conflict on moveKeysLocks with the current running DD is expected, just retry.
					tr.reset();
				} else {
					wait(tr.onError(e));
				}
			}
		}

		TraceEvent("TestMoveShardComplete").detail("Range", keys.toString()).detail("NewTeam", describe(dests));

		return dests;
	}

	ACTOR Future<std::vector<StorageServerShard>> getStorageServerShards(Database cx, UID ssId, KeyRange range) {
		state Transaction tr(cx);
		loop {
			try {
				Optional<Value> serverListValue = wait(tr.get(serverListKeyFor(ssId)));
				ASSERT(serverListValue.present());
				state StorageServerInterface ssi = decodeServerListValue(serverListValue.get());
				GetShardStateRequest req(range, GetShardStateRequest::READABLE, true);
				GetShardStateReply rep = wait(ssi.getShardState.getReply(req, TaskPriority::DefaultEndpoint));
				return rep.shards;
			} catch (Error& e) {
				wait(tr.onError(e));
			}
		}
	}

	Future<bool> check(Database const& cx) override { return pass; }

	void getMetrics(std::vector<PerfMetric>& m) override {}
};

WorkloadFactory<PhysicalShardMoveWorkLoad> PhysicalShardMoveWorkLoadFactory("PhysicalShardMove");