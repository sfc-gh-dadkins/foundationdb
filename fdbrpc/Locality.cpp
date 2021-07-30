/*
 * Locality.cpp
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

#include "fdbrpc/Locality.h"

const UID LocalityData::UNSET_ID = UID(0x0ccb4e0feddb5583, 0x010f6b77d9d10ece);
const StringRef LocalityData::keyProcessId = LiteralStringRef("processid");
const StringRef LocalityData::keyZoneId = LiteralStringRef("zoneid");
const StringRef LocalityData::keyDcId = LiteralStringRef("dcid");
const StringRef LocalityData::keyMachineId = LiteralStringRef("machineid");
const StringRef LocalityData::keyDataHallId = LiteralStringRef("data_hall");
const StringRef LocalityData::ExcludeLocalityKeyMachineIdPrefix = LiteralStringRef("locality_machineid:");
const StringRef LocalityData::ExcludeLocalityPrefix = LiteralStringRef("locality_");

ProcessClass::Fitness ProcessClass::machineClassFitness(ClusterRole role) const {
	switch (role) {
	case ProcessClass::Storage:
		switch (_class) {
		case ProcessClass::StorageClass:
			return ProcessClass::BestFit;
		case ProcessClass::UnsetClass:
			return ProcessClass::UnsetFit;
		case ProcessClass::TransactionClass:
			return ProcessClass::WorstFit;
		case ProcessClass::LogClass:
			return ProcessClass::WorstFit;
		case ProcessClass::CoordinatorClass:
		case ProcessClass::TesterClass:
		case ProcessClass::StorageCacheClass:
			return ProcessClass::NeverAssign;
		default:
			return ProcessClass::NeverAssign;
		}
	case ProcessClass::TLog:
		switch (_class) {
		case ProcessClass::LogClass:
			return ProcessClass::BestFit;
		case ProcessClass::TransactionClass:
			return ProcessClass::GoodFit;
		case ProcessClass::UnsetClass:
			return ProcessClass::UnsetFit;
		case ProcessClass::StorageClass:
			return ProcessClass::WorstFit;
		case ProcessClass::CoordinatorClass:
		case ProcessClass::TesterClass:
		case ProcessClass::StorageCacheClass:
			return ProcessClass::NeverAssign;
		default:
			return ProcessClass::NeverAssign;
		}
	case ProcessClass::CommitProxy: // Resolver, Master, CommitProxy, and GrvProxy need to be the same besides best fit
		switch (_class) {
		case ProcessClass::CommitProxyClass:
			return ProcessClass::BestFit;
		case ProcessClass::StatelessClass:
			return ProcessClass::GoodFit;
		case ProcessClass::UnsetClass:
			return ProcessClass::UnsetFit;
		case ProcessClass::TransactionClass:
			return ProcessClass::OkayFit;
		case ProcessClass::CoordinatorClass:
		case ProcessClass::TesterClass:
		case ProcessClass::StorageCacheClass:
			return ProcessClass::NeverAssign;
		default:
			return ProcessClass::WorstFit;
		}
	case ProcessClass::GrvProxy: // Resolver, Master, CommitProxy, and GrvProxy need to be the same besides best fit
		switch (_class) {
		case ProcessClass::GrvProxyClass:
			return ProcessClass::BestFit;
		case ProcessClass::StatelessClass:
			return ProcessClass::GoodFit;
		case ProcessClass::UnsetClass:
			return ProcessClass::UnsetFit;
		case ProcessClass::TransactionClass:
			return ProcessClass::OkayFit;
		case ProcessClass::CoordinatorClass:
		case ProcessClass::TesterClass:
		case ProcessClass::StorageCacheClass:
			return ProcessClass::NeverAssign;
		default:
			return ProcessClass::WorstFit;
		}
	case ProcessClass::Master: // Resolver, Master, CommitProxy, and GrvProxy need to be the same besides best fit
		switch (_class) {
		case ProcessClass::MasterClass:
			return ProcessClass::BestFit;
		case ProcessClass::StatelessClass:
			return ProcessClass::GoodFit;
		case ProcessClass::UnsetClass:
			return ProcessClass::UnsetFit;
		case ProcessClass::TransactionClass:
			return ProcessClass::OkayFit;
		case ProcessClass::CoordinatorClass:
		case ProcessClass::TesterClass:
		case ProcessClass::StorageCacheClass:
			return ProcessClass::NeverAssign;
		default:
			return ProcessClass::WorstFit;
		}
	case ProcessClass::Resolver: // Resolver, Master, CommitProxy, and GrvProxy need to be the same besides best fit
		switch (_class) {
		case ProcessClass::ResolutionClass:
			return ProcessClass::BestFit;
		case ProcessClass::StatelessClass:
			return ProcessClass::GoodFit;
		case ProcessClass::UnsetClass:
			return ProcessClass::UnsetFit;
		case ProcessClass::TransactionClass:
			return ProcessClass::OkayFit;
		case ProcessClass::CoordinatorClass:
		case ProcessClass::TesterClass:
		case ProcessClass::StorageCacheClass:
			return ProcessClass::NeverAssign;
		default:
			return ProcessClass::WorstFit;
		}
	case ProcessClass::VersionIndexer:
		switch (_class) {
		case ProcessClass::VersionIndexerClass:
			return ProcessClass::BestFit;
		case ProcessClass::StatelessClass:
			return ProcessClass::GoodFit;
		case ProcessClass::UnsetClass:
			return ProcessClass::UnsetFit;
		case ProcessClass::TransactionClass:
			return ProcessClass::OkayFit;
		case ProcessClass::CoordinatorClass:
		case ProcessClass::TesterClass:
		case ProcessClass::StorageCacheClass:
			return ProcessClass::NeverAssign;
		default:
			return ProcessClass::WorstFit;
		}
	case ProcessClass::LogRouter:
		switch (_class) {
		case ProcessClass::LogRouterClass:
			return ProcessClass::BestFit;
		case ProcessClass::StatelessClass:
			return ProcessClass::GoodFit;
		case ProcessClass::UnsetClass:
			return ProcessClass::UnsetFit;
		case ProcessClass::TransactionClass:
			return ProcessClass::OkayFit;
		case ProcessClass::CoordinatorClass:
		case ProcessClass::TesterClass:
		case ProcessClass::StorageCacheClass:
			return ProcessClass::NeverAssign;
		default:
			return ProcessClass::WorstFit;
		}
	case ProcessClass::Backup:
		switch (_class) {
		case ProcessClass::BackupClass:
			return ProcessClass::BestFit;
		case ProcessClass::StatelessClass:
		case ProcessClass::LogRouterClass:
			return ProcessClass::GoodFit;
		case ProcessClass::UnsetClass:
			return ProcessClass::UnsetFit;
		case ProcessClass::TransactionClass:
			return ProcessClass::OkayFit;
		case ProcessClass::CoordinatorClass:
		case ProcessClass::MasterClass:
		case ProcessClass::TesterClass:
			return ProcessClass::NeverAssign;
		default:
			return ProcessClass::WorstFit;
		}
	case ProcessClass::ClusterController:
		switch (_class) {
		case ProcessClass::ClusterControllerClass:
			return ProcessClass::BestFit;
		case ProcessClass::StatelessClass:
			return ProcessClass::GoodFit;
		case ProcessClass::UnsetClass:
			return ProcessClass::UnsetFit;
		case ProcessClass::MasterClass:
			return ProcessClass::OkayFit;
		case ProcessClass::ResolutionClass:
			return ProcessClass::OkayFit;
		case ProcessClass::VersionIndexerClass:
			return ProcessClass::OkayFit;
		case ProcessClass::TransactionClass:
			return ProcessClass::OkayFit;
		case ProcessClass::CommitProxyClass:
			return ProcessClass::OkayFit;
		case ProcessClass::GrvProxyClass:
			return ProcessClass::OkayFit;
		case ProcessClass::LogRouterClass:
			return ProcessClass::OkayFit;
		case ProcessClass::CoordinatorClass:
		case ProcessClass::TesterClass:
		case ProcessClass::StorageCacheClass:
			return ProcessClass::NeverAssign;
		default:
			return ProcessClass::WorstFit;
		}
	case ProcessClass::DataDistributor:
		switch (_class) {
		case ProcessClass::DataDistributorClass:
			return ProcessClass::BestFit;
		case ProcessClass::StatelessClass:
			return ProcessClass::GoodFit;
		case ProcessClass::UnsetClass:
			return ProcessClass::UnsetFit;
		case ProcessClass::MasterClass:
			return ProcessClass::OkayFit;
		case ProcessClass::CoordinatorClass:
		case ProcessClass::TesterClass:
		case ProcessClass::StorageCacheClass:
			return ProcessClass::NeverAssign;
		default:
			return ProcessClass::WorstFit;
		}
	case ProcessClass::Ratekeeper:
		switch (_class) {
		case ProcessClass::RatekeeperClass:
			return ProcessClass::BestFit;
		case ProcessClass::StatelessClass:
			return ProcessClass::GoodFit;
		case ProcessClass::UnsetClass:
			return ProcessClass::UnsetFit;
		case ProcessClass::MasterClass:
			return ProcessClass::OkayFit;
		case ProcessClass::CoordinatorClass:
		case ProcessClass::TesterClass:
		case ProcessClass::StorageCacheClass:
			return ProcessClass::NeverAssign;
		default:
			return ProcessClass::WorstFit;
		}
	case ProcessClass::StorageCache:
		switch (_class) {
		case ProcessClass::StorageCacheClass:
			return ProcessClass::BestFit;
		default:
			return ProcessClass::NeverAssign;
		}
	default:
		return ProcessClass::NeverAssign;
	}
}

LBDistance::Type loadBalanceDistance(LocalityData const& loc1, LocalityData const& loc2, NetworkAddress const& addr2) {
	if (FLOW_KNOBS->LOAD_BALANCE_ZONE_ID_LOCALITY_ENABLED && loc1.zoneId().present() &&
	    loc1.zoneId() == loc2.zoneId()) {
		return LBDistance::SAME_MACHINE;
	}
	// FIXME: add this back in when load balancing works with local requests
	// if ( g_network->isAddressOnThisHost( addr2 ) )
	//	return LBDistance::SAME_MACHINE;
	if (FLOW_KNOBS->LOAD_BALANCE_DC_ID_LOCALITY_ENABLED && loc1.dcId().present() && loc1.dcId() == loc2.dcId()) {
		return LBDistance::SAME_DC;
	}
	return LBDistance::DISTANT;
}

StringRef to_string(ProcessClass::ClusterRole role) {
	switch (role) {
	case ProcessClass::Storage:
		return "Storage"_sr;
		break;
	case ProcessClass::TLog:
		return "TLog"_sr;
		break;
	case ProcessClass::CommitProxy:
		return "CommitProxy"_sr;
		break;
	case ProcessClass::GrvProxy:
		return "GrvProxy"_sr;
		break;
	case ProcessClass::Master:
		return "Master"_sr;
		break;
	case ProcessClass::Resolver:
		return "Resolver"_sr;
		break;
	case ProcessClass::LogRouter:
		return "LogRouter"_sr;
		break;
	case ProcessClass::ClusterController:
		return "ClusterController"_sr;
		break;
	case ProcessClass::DataDistributor:
		return "DataDistributor"_sr;
		break;
	case ProcessClass::Ratekeeper:
		return "Ratekeeper"_sr;
		break;
	case ProcessClass::StorageCache:
		return "StorageCache"_sr;
		break;
	case ProcessClass::Backup:
		return "Backup"_sr;
		break;
	case ProcessClass::VersionIndexer:
		return "VersionIndexer"_sr;
		break;
	case ProcessClass::NoRole:
		return "NoRole"_sr;
		break;
	}
	UNSTOPPABLE_ASSERT(false);
}
