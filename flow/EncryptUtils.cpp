/*
 * EncryptUtils.cpp
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

#include "flow/EncryptUtils.h"
#include "flow/IRandom.h"
#include "flow/Knobs.h"
#include "flow/Trace.h"

#include <boost/algorithm/string.hpp>
#include <boost/format.hpp>

const EncryptCipherDomainName FDB_SYSTEM_KEYSPACE_ENCRYPT_DOMAIN_NAME = "FdbSystemKeyspaceEncryptDomain"_sr;
const EncryptCipherDomainName FDB_DEFAULT_ENCRYPT_DOMAIN_NAME = "FdbDefaultEncryptDomain"_sr;
const EncryptCipherDomainName FDB_ENCRYPT_HEADER_DOMAIN_NAME = "FdbEncryptHeaderDomain"_sr;

EncryptCipherMode encryptModeFromString(const std::string& modeStr) {
	if (modeStr == "NONE") {
		return ENCRYPT_CIPHER_MODE_NONE;
	} else if (modeStr == "AES-256-CTR") {
		return ENCRYPT_CIPHER_MODE_AES_256_CTR;
	} else {
		TraceEvent("EncryptModeFromString").log();
		throw not_implemented();
	}
}

std::string getEncryptDbgTraceKey(std::string_view prefix,
                                  EncryptCipherDomainId domainId,
                                  StringRef domainName,
                                  Optional<EncryptCipherBaseKeyId> baseCipherId) {
	// Construct the TraceEvent field key ensuring its uniqueness and compliance to TraceEvent field validator and log
	// parsing tools
	std::string dName = domainName.toString();
	// Underscores are invalid in trace event detail name.
	boost::replace_all(dName, "_", "-");
	if (baseCipherId.present()) {
		boost::format fmter("%s.%lld.%s.%llu");
		return boost::str(boost::format(fmter % prefix % domainId % dName % baseCipherId.get()));
	} else {
		boost::format fmter("%s.%lld.%s");
		return boost::str(boost::format(fmter % prefix % domainId % dName));
	}
}

std::string getEncryptDbgTraceKeyWithTS(std::string_view prefix,
                                        EncryptCipherDomainId domainId,
                                        StringRef domainName,
                                        EncryptCipherBaseKeyId baseCipherId,
                                        int64_t refAfterTS,
                                        int64_t expAfterTS) {
	// Construct the TraceEvent field key ensuring its uniqueness and compliance to TraceEvent field validator and log
	// parsing tools
	std::string dName = domainName.toString();
	// Underscores are invalid in trace event detail name.
	boost::replace_all(dName, "_", "-");
	boost::format fmter("%s.%lld.%s.%llu.%lld.%lld");
	return boost::str(boost::format(fmter % prefix % domainId % dName % baseCipherId % refAfterTS % expAfterTS));
}

int getEncryptHeaderAuthTokenSize(int algo) {
	switch (algo) {
	case ENCRYPT_HEADER_AUTH_TOKEN_ALGO_HMAC_SHA:
		return 32;
	case ENCRYPT_HEADER_AUTH_TOKEN_ALGO_AES_CMAC:
		return 16;
	default:
		throw not_implemented();
	}
}

bool isEncryptHeaderAuthTokenAlgoValid(const EncryptAuthTokenAlgo algo) {
	return algo >= EncryptAuthTokenAlgo::ENCRYPT_HEADER_AUTH_TOKEN_ALGO_NONE &&
	       algo < EncryptAuthTokenAlgo::ENCRYPT_HEADER_AUTH_TOKEN_ALGO_LAST;
}

bool isEncryptHeaderAuthTokenModeValid(const EncryptAuthTokenMode mode) {
	return mode >= EncryptAuthTokenMode::ENCRYPT_HEADER_AUTH_TOKEN_MODE_NONE &&
	       mode < EncryptAuthTokenMode::ENCRYPT_HEADER_AUTH_TOKEN_LAST;
}

bool isEncryptHeaderAuthTokenDetailsValid(const EncryptAuthTokenMode mode, const EncryptAuthTokenAlgo algo) {
	if (!isEncryptHeaderAuthTokenModeValid(mode) || !isEncryptHeaderAuthTokenAlgoValid(algo) ||
	    (mode == EncryptAuthTokenMode::ENCRYPT_HEADER_AUTH_TOKEN_MODE_NONE &&
	     algo != EncryptAuthTokenAlgo::ENCRYPT_HEADER_AUTH_TOKEN_ALGO_NONE) ||
	    (mode != EncryptAuthTokenMode::ENCRYPT_HEADER_AUTH_TOKEN_MODE_NONE &&
	     algo == EncryptAuthTokenAlgo::ENCRYPT_HEADER_AUTH_TOKEN_ALGO_NONE)) {
		return false;
	}
	return true;
}

// Routine enables mapping EncryptHeader authTokenAlgo for a given authTokenMode; rules followed are:
// 1. AUTH_TOKEN_NONE overrides authTokenAlgo configuration (as expected)
// 2. AuthToken mode governed by the FLOW_KNOBS->ENCRYPT_HEADER_AUTH_TOKEN_ALGO
EncryptAuthTokenAlgo getAuthTokenAlgoFromMode(const EncryptAuthTokenMode mode) {
	EncryptAuthTokenAlgo algo;

	if (mode == EncryptAuthTokenMode::ENCRYPT_HEADER_AUTH_TOKEN_MODE_NONE) {
		// TOKEN_MODE_NONE overrides authTokenAlgo
		algo = EncryptAuthTokenAlgo::ENCRYPT_HEADER_AUTH_TOKEN_ALGO_NONE;
	} else {
		algo = (EncryptAuthTokenAlgo)FLOW_KNOBS->ENCRYPT_HEADER_AUTH_TOKEN_ALGO;
		// Ensure cluster authTokenAlgo sanity
		if (algo == EncryptAuthTokenAlgo::ENCRYPT_HEADER_AUTH_TOKEN_ALGO_NONE) {
			TraceEvent(SevWarn, "AuthTokenAlgoMisconfiguration").detail("Algo", algo).detail("Mode", mode);
			throw not_implemented();
		}
	}
	ASSERT(isEncryptHeaderAuthTokenDetailsValid(mode, algo));
	return algo;
}

EncryptAuthTokenMode getRandomAuthTokenMode() {
	std::vector<EncryptAuthTokenMode> modes = { EncryptAuthTokenMode::ENCRYPT_HEADER_AUTH_TOKEN_MODE_NONE,
		                                        EncryptAuthTokenMode::ENCRYPT_HEADER_AUTH_TOKEN_MODE_SINGLE,
		                                        EncryptAuthTokenMode::ENCRYPT_HEADER_AUTH_TOKEN_MODE_MULTI };
	int idx = deterministicRandom()->randomInt(0, modes.size());
	return modes[idx];
}

EncryptAuthTokenAlgo getRandomAuthTokenAlgo() {
	EncryptAuthTokenAlgo algo = deterministicRandom()->coinflip()
	                                ? EncryptAuthTokenAlgo::ENCRYPT_HEADER_AUTH_TOKEN_ALGO_AES_CMAC
	                                : EncryptAuthTokenAlgo::ENCRYPT_HEADER_AUTH_TOKEN_ALGO_HMAC_SHA;

	return algo;
}