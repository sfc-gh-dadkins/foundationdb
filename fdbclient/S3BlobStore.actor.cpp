/*
 * S3BlobStore.actor.cpp
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

#include "fdbclient/S3BlobStore.h"

#include "md5/md5.h"
#include "libb64/encode.h"
#include "fdbclient/sha1/SHA1.h"
#include <time.h>
#include <iomanip>
#if defined(HAVE_WOLFSSL)
#include <wolfssl/options.h>
#endif
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#if defined(HAVE_WOLFSSL)
#undef SHA1 // wolfSSL will will shadow FDB SHA1.h
#endif
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string.hpp>
#include "flow/IAsyncFile.h"
#include "flow/Hostname.h"
#include "flow/UnitTest.h"
#include "rapidxml/rapidxml.hpp"
#ifdef BUILD_AWS_BACKUP
#include "fdbclient/FDBAWSCredentialsProvider.h"
#endif

#include "flow/actorcompiler.h" // has to be last include

using namespace rapidxml;

json_spirit::mObject S3BlobStoreEndpoint::Stats::getJSON() {
	json_spirit::mObject o;

	o["requests_failed"] = requests_failed;
	o["requests_successful"] = requests_successful;
	o["bytes_sent"] = bytes_sent;

	return o;
}

S3BlobStoreEndpoint::Stats S3BlobStoreEndpoint::Stats::operator-(const Stats& rhs) {
	Stats r;
	r.requests_failed = requests_failed - rhs.requests_failed;
	r.requests_successful = requests_successful - rhs.requests_successful;
	r.bytes_sent = bytes_sent - rhs.bytes_sent;
	return r;
}

S3BlobStoreEndpoint::Stats S3BlobStoreEndpoint::s_stats;

S3BlobStoreEndpoint::BlobKnobs::BlobKnobs() {
	secure_connection = 1;
	connect_tries = CLIENT_KNOBS->BLOBSTORE_CONNECT_TRIES;
	connect_timeout = CLIENT_KNOBS->BLOBSTORE_CONNECT_TIMEOUT;
	max_connection_life = CLIENT_KNOBS->BLOBSTORE_MAX_CONNECTION_LIFE;
	request_tries = CLIENT_KNOBS->BLOBSTORE_REQUEST_TRIES;
	request_timeout_min = CLIENT_KNOBS->BLOBSTORE_REQUEST_TIMEOUT_MIN;
	requests_per_second = CLIENT_KNOBS->BLOBSTORE_REQUESTS_PER_SECOND;
	concurrent_requests = CLIENT_KNOBS->BLOBSTORE_CONCURRENT_REQUESTS;
	list_requests_per_second = CLIENT_KNOBS->BLOBSTORE_LIST_REQUESTS_PER_SECOND;
	write_requests_per_second = CLIENT_KNOBS->BLOBSTORE_WRITE_REQUESTS_PER_SECOND;
	read_requests_per_second = CLIENT_KNOBS->BLOBSTORE_READ_REQUESTS_PER_SECOND;
	delete_requests_per_second = CLIENT_KNOBS->BLOBSTORE_DELETE_REQUESTS_PER_SECOND;
	multipart_max_part_size = CLIENT_KNOBS->BLOBSTORE_MULTIPART_MAX_PART_SIZE;
	multipart_min_part_size = CLIENT_KNOBS->BLOBSTORE_MULTIPART_MIN_PART_SIZE;
	concurrent_uploads = CLIENT_KNOBS->BLOBSTORE_CONCURRENT_UPLOADS;
	concurrent_lists = CLIENT_KNOBS->BLOBSTORE_CONCURRENT_LISTS;
	concurrent_reads_per_file = CLIENT_KNOBS->BLOBSTORE_CONCURRENT_READS_PER_FILE;
	concurrent_writes_per_file = CLIENT_KNOBS->BLOBSTORE_CONCURRENT_WRITES_PER_FILE;
	enable_read_cache = CLIENT_KNOBS->BLOBSTORE_ENABLE_READ_CACHE;
	read_block_size = CLIENT_KNOBS->BLOBSTORE_READ_BLOCK_SIZE;
	read_ahead_blocks = CLIENT_KNOBS->BLOBSTORE_READ_AHEAD_BLOCKS;
	read_cache_blocks_per_file = CLIENT_KNOBS->BLOBSTORE_READ_CACHE_BLOCKS_PER_FILE;
	max_send_bytes_per_second = CLIENT_KNOBS->BLOBSTORE_MAX_SEND_BYTES_PER_SECOND;
	max_recv_bytes_per_second = CLIENT_KNOBS->BLOBSTORE_MAX_RECV_BYTES_PER_SECOND;
	sdk_auth = false;
}

bool S3BlobStoreEndpoint::BlobKnobs::set(StringRef name, int value) {
#define TRY_PARAM(n, sn)                                                                                               \
	if (name == #n || name == #sn) {                                                                                   \
		n = value;                                                                                                     \
		return true;                                                                                                   \
	}
	TRY_PARAM(secure_connection, sc)
	TRY_PARAM(connect_tries, ct);
	TRY_PARAM(connect_timeout, cto);
	TRY_PARAM(max_connection_life, mcl);
	TRY_PARAM(request_tries, rt);
	TRY_PARAM(request_timeout_min, rtom);
	// TODO: For backward compatibility because request_timeout was renamed to request_timeout_min
	if (name == "request_timeout"_sr || name == "rto"_sr) {
		request_timeout_min = value;
		return true;
	}
	TRY_PARAM(requests_per_second, rps);
	TRY_PARAM(list_requests_per_second, lrps);
	TRY_PARAM(write_requests_per_second, wrps);
	TRY_PARAM(read_requests_per_second, rrps);
	TRY_PARAM(delete_requests_per_second, drps);
	TRY_PARAM(concurrent_requests, cr);
	TRY_PARAM(multipart_max_part_size, maxps);
	TRY_PARAM(multipart_min_part_size, minps);
	TRY_PARAM(concurrent_uploads, cu);
	TRY_PARAM(concurrent_lists, cl);
	TRY_PARAM(concurrent_reads_per_file, crpf);
	TRY_PARAM(concurrent_writes_per_file, cwpf);
	TRY_PARAM(enable_read_cache, erc);
	TRY_PARAM(read_block_size, rbs);
	TRY_PARAM(read_ahead_blocks, rab);
	TRY_PARAM(read_cache_blocks_per_file, rcb);
	TRY_PARAM(max_send_bytes_per_second, sbps);
	TRY_PARAM(max_recv_bytes_per_second, rbps);
	TRY_PARAM(sdk_auth, sa);
#undef TRY_PARAM
	return false;
}

// Returns an S3 Blob URL parameter string that specifies all of the non-default options for the endpoint using option
// short names.
std::string S3BlobStoreEndpoint::BlobKnobs::getURLParameters() const {
	static BlobKnobs defaults;
	std::string r;
#define _CHECK_PARAM(n, sn)                                                                                            \
	if (n != defaults.n) {                                                                                             \
		r += format("%s%s=%d", r.empty() ? "" : "&", #sn, n);                                                          \
	}
	_CHECK_PARAM(secure_connection, sc);
	_CHECK_PARAM(connect_tries, ct);
	_CHECK_PARAM(connect_timeout, cto);
	_CHECK_PARAM(max_connection_life, mcl);
	_CHECK_PARAM(request_tries, rt);
	_CHECK_PARAM(request_timeout_min, rto);
	_CHECK_PARAM(requests_per_second, rps);
	_CHECK_PARAM(list_requests_per_second, lrps);
	_CHECK_PARAM(write_requests_per_second, wrps);
	_CHECK_PARAM(read_requests_per_second, rrps);
	_CHECK_PARAM(delete_requests_per_second, drps);
	_CHECK_PARAM(concurrent_requests, cr);
	_CHECK_PARAM(multipart_max_part_size, maxps);
	_CHECK_PARAM(multipart_min_part_size, minps);
	_CHECK_PARAM(concurrent_uploads, cu);
	_CHECK_PARAM(concurrent_lists, cl);
	_CHECK_PARAM(concurrent_reads_per_file, crpf);
	_CHECK_PARAM(concurrent_writes_per_file, cwpf);
	_CHECK_PARAM(enable_read_cache, erc);
	_CHECK_PARAM(read_block_size, rbs);
	_CHECK_PARAM(read_ahead_blocks, rab);
	_CHECK_PARAM(read_cache_blocks_per_file, rcb);
	_CHECK_PARAM(max_send_bytes_per_second, sbps);
	_CHECK_PARAM(max_recv_bytes_per_second, rbps);
#undef _CHECK_PARAM
	return r;
}

std::string guessRegionFromDomain(std::string domain) {
	static const std::vector<const char*> knownServices = { "s3.", "cos.", "oss-", "obs." };
	boost::algorithm::to_lower(domain);

	for (int i = 0; i < knownServices.size(); ++i) {
		const char* service = knownServices[i];

		std::size_t p = domain.find(service);

		if (p == std::string::npos || (p >= 1 && domain[p - 1] != '.')) {
			// eg. 127.0.0.1, example.com, s3-service.example.com, mys3.example.com
			continue;
		}

		StringRef h(domain.c_str() + p);

		if (!h.startsWith("oss-"_sr)) {
			h.eat(service); // ignore s3 service
		}

		return h.eat(".").toString();
	}

	return "";
}

Reference<S3BlobStoreEndpoint> S3BlobStoreEndpoint::fromString(const std::string& url,
                                                               const Optional<std::string>& proxy,
                                                               std::string* resourceFromURL,
                                                               std::string* error,
                                                               ParametersT* ignored_parameters) {
	if (resourceFromURL)
		resourceFromURL->clear();

	try {
		StringRef t(url);
		StringRef prefix = t.eat("://");
		if (prefix != "blobstore"_sr)
			throw format("Invalid blobstore URL prefix '%s'", prefix.toString().c_str());

		Optional<std::string> proxyHost, proxyPort;
		if (proxy.present()) {
			StringRef proxyRef(proxy.get());
			if (proxy.get().find("://") != std::string::npos) {
				StringRef proxyPrefix = proxyRef.eat("://");
				if (proxyPrefix != "http"_sr) {
					throw format("Invalid proxy URL prefix '%s'. Either don't use a prefix, or use http://",
					             proxyPrefix.toString().c_str());
				}
			}
			std::string proxyBody = proxyRef.eat().toString();
			if (!Hostname::isHostname(proxyBody) && !NetworkAddress::parseOptional(proxyBody).present()) {
				throw format("'%s' is not a valid value for proxy. Format should be either IP:port or host:port.",
				             proxyBody.c_str());
			}
			StringRef p(proxyBody);
			proxyHost = p.eat(":").toString();
			proxyPort = p.eat().toString();
		}

		Optional<StringRef> cred;
		if (url.find("@") != std::string::npos) {
			cred = t.eat("@");
		}
		uint8_t foundSeparator = 0;
		StringRef hostPort = t.eatAny("/?", &foundSeparator);
		StringRef resource;
		if (foundSeparator == '/') {
			resource = t.eat("?");
		}

		// hostPort is at least a host or IP address, optionally followed by :portNumber or :serviceName
		StringRef h(hostPort);
		StringRef host = h.eat(":");
		if (host.size() == 0)
			throw std::string("host cannot be empty");

		StringRef service = h.eat();

		std::string region = guessRegionFromDomain(host.toString());

		BlobKnobs knobs;
		HTTP::Headers extraHeaders;
		while (1) {
			StringRef name = t.eat("=");
			if (name.size() == 0)
				break;
			StringRef value = t.eat("&");

			// Special case for header
			if (name == "header"_sr) {
				StringRef originalValue = value;
				StringRef headerFieldName = value.eat(":");
				StringRef headerFieldValue = value;
				if (headerFieldName.size() == 0 || headerFieldValue.size() == 0) {
					throw format("'%s' is not a valid value for '%s' parameter.  Format is <FieldName>:<FieldValue> "
					             "where strings are not empty.",
					             originalValue.toString().c_str(),
					             name.toString().c_str());
				}
				std::string& fieldValue = extraHeaders[headerFieldName.toString()];
				// RFC 2616 section 4.2 says header field names can repeat but only if it is valid to concatenate their
				// values with comma separation
				if (!fieldValue.empty()) {
					fieldValue.append(",");
				}
				fieldValue.append(headerFieldValue.toString());
				continue;
			}

			// overwrite s3 region from parameter
			if (name == "region"_sr) {
				region = value.toString();
				continue;
			}

			// See if the parameter is a knob
			// First try setting a dummy value (all knobs are currently numeric) just to see if this parameter is known
			// to S3BlobStoreEndpoint. If it is, then we will set it to a good value or throw below, so the dummy set
			// has no bad side effects.
			bool known = knobs.set(name, 0);

			// If the parameter is not known to S3BlobStoreEndpoint then throw unless there is an ignored_parameters set
			// to add it to
			if (!known) {
				if (ignored_parameters == nullptr) {
					throw format("%s is not a valid parameter name", name.toString().c_str());
				}
				(*ignored_parameters)[name.toString()] = value.toString();
				continue;
			}

			// The parameter is known to S3BlobStoreEndpoint so it must be numeric and valid.
			char* valueEnd;
			int ivalue = strtol(value.toString().c_str(), &valueEnd, 10);
			if (*valueEnd || (ivalue == 0 && value.toString() != "0"))
				throw format("%s is not a valid value for %s", value.toString().c_str(), name.toString().c_str());

			// It should not be possible for this set to fail now since the dummy set above had to have worked.
			ASSERT(knobs.set(name, ivalue));
		}

		if (resourceFromURL != nullptr)
			*resourceFromURL = resource.toString();

		Optional<S3BlobStoreEndpoint::Credentials> creds;
		if (cred.present()) {
			StringRef c(cred.get());
			StringRef key = c.eat(":");
			StringRef secret = c.eat(":");
			StringRef securityToken = c.eat();
			creds = S3BlobStoreEndpoint::Credentials{ key.toString(), secret.toString(), securityToken.toString() };
		}

		if (region.empty() && CLIENT_KNOBS->HTTP_REQUEST_AWS_V4_HEADER) {
			throw std::string(
			    "Failed to get region from host or parameter in url, region is required for aws v4 signature");
		}

		return makeReference<S3BlobStoreEndpoint>(
		    host.toString(), service.toString(), region, proxyHost, proxyPort, creds, knobs, extraHeaders);

	} catch (std::string& err) {
		if (error != nullptr)
			*error = err;
		TraceEvent(SevWarnAlways, "S3BlobStoreEndpointBadURL")
		    .suppressFor(60)
		    .detail("Description", err)
		    .detail("Format", getURLFormat())
		    .detail("URL", url);
		throw backup_invalid_url();
	}
}

std::string S3BlobStoreEndpoint::getResourceURL(std::string resource, std::string params) const {
	std::string hostPort = host;
	if (!service.empty()) {
		hostPort.append(":");
		hostPort.append(service);
	}

	// If secret isn't being looked up from credentials files then it was passed explicitly in the URL so show it here.
	std::string credsString;
	if (credentials.present()) {
		if (!lookupKey) {
			credsString = credentials.get().key;
		}
		if (!lookupSecret) {
			credsString += ":" + credentials.get().secret;
		}
		if (!lookupSecret) {
			credsString +=
			    credentials.get().securityToken.empty()
			        ? std::string(":") + credentials.get().secret
			        : std::string(":") + credentials.get().secret + std::string(":") + credentials.get().securityToken;
		}
		credsString += "@";
	}

	std::string r = format("blobstore://%s%s/%s", credsString.c_str(), hostPort.c_str(), resource.c_str());

	// Get params that are deviations from knob defaults
	std::string knobParams = knobs.getURLParameters();
	if (!knobParams.empty()) {
		if (!params.empty()) {
			params.append("&");
		}
		params.append(knobParams);
	}

	for (const auto& [k, v] : extraHeaders) {
		if (!params.empty()) {
			params.append("&");
		}
		params.append("header=");
		params.append(k);
		params.append(":");
		params.append(v);
	}

	if (!params.empty())
		r.append("?").append(params);

	return r;
}

std::string constructResourcePath(Reference<S3BlobStoreEndpoint> b,
                                  const std::string& bucket,
                                  const std::string& object) {
	std::string resource;

	if (b->getHost().find(bucket + ".") != 0) {
		resource += std::string("/") + bucket; // not virtual hosting mode
	}

	if (!object.empty()) {
		resource += "/";
		resource += object;
	}

	return resource;
}

ACTOR Future<bool> bucketExists_impl(Reference<S3BlobStoreEndpoint> b, std::string bucket) {
	wait(b->requestRateRead->getAllowance(1));

	std::string resource = constructResourcePath(b, bucket, "");
	HTTP::Headers headers;

	Reference<HTTP::Response> r = wait(b->doRequest("HEAD", resource, headers, nullptr, 0, { 200, 404 }));
	return r->code == 200;
}

Future<bool> S3BlobStoreEndpoint::bucketExists(std::string const& bucket) {
	return bucketExists_impl(Reference<S3BlobStoreEndpoint>::addRef(this), bucket);
}

ACTOR Future<bool> objectExists_impl(Reference<S3BlobStoreEndpoint> b, std::string bucket, std::string object) {
	wait(b->requestRateRead->getAllowance(1));

	std::string resource = constructResourcePath(b, bucket, object);
	HTTP::Headers headers;

	Reference<HTTP::Response> r = wait(b->doRequest("HEAD", resource, headers, nullptr, 0, { 200, 404 }));
	return r->code == 200;
}

Future<bool> S3BlobStoreEndpoint::objectExists(std::string const& bucket, std::string const& object) {
	return objectExists_impl(Reference<S3BlobStoreEndpoint>::addRef(this), bucket, object);
}

ACTOR Future<Void> deleteObject_impl(Reference<S3BlobStoreEndpoint> b, std::string bucket, std::string object) {
	wait(b->requestRateDelete->getAllowance(1));

	std::string resource = constructResourcePath(b, bucket, object);
	HTTP::Headers headers;
	// 200 or 204 means object successfully deleted, 404 means it already doesn't exist, so any of those are considered
	// successful
	Reference<HTTP::Response> r = wait(b->doRequest("DELETE", resource, headers, nullptr, 0, { 200, 204, 404 }));

	// But if the object already did not exist then the 'delete' is assumed to be successful but a warning is logged.
	if (r->code == 404) {
		TraceEvent(SevWarnAlways, "S3BlobStoreEndpointDeleteObjectMissing")
		    .detail("Host", b->host)
		    .detail("Bucket", bucket)
		    .detail("Object", object);
	}

	return Void();
}

Future<Void> S3BlobStoreEndpoint::deleteObject(std::string const& bucket, std::string const& object) {
	return deleteObject_impl(Reference<S3BlobStoreEndpoint>::addRef(this), bucket, object);
}

ACTOR Future<Void> deleteRecursively_impl(Reference<S3BlobStoreEndpoint> b,
                                          std::string bucket,
                                          std::string prefix,
                                          int* pNumDeleted,
                                          int64_t* pBytesDeleted) {
	state PromiseStream<S3BlobStoreEndpoint::ListResult> resultStream;
	// Start a recursive parallel listing which will send results to resultStream as they are received
	state Future<Void> done = b->listObjectsStream(bucket, resultStream, prefix, '/', std::numeric_limits<int>::max());
	// Wrap done in an actor which will send end_of_stream since listObjectsStream() does not (so that many calls can
	// write to the same stream)
	done = map(done, [=](Void) mutable {
		resultStream.sendError(end_of_stream());
		return Void();
	});

	state std::list<Future<Void>> deleteFutures;
	try {
		loop {
			choose {
				// Throw if done throws, otherwise don't stop until end_of_stream
				when(wait(done)) { done = Never(); }

				when(S3BlobStoreEndpoint::ListResult list = waitNext(resultStream.getFuture())) {
					for (auto& object : list.objects) {
						deleteFutures.push_back(map(b->deleteObject(bucket, object.name), [=](Void) -> Void {
							if (pNumDeleted != nullptr) {
								++*pNumDeleted;
							}
							if (pBytesDeleted != nullptr) {
								*pBytesDeleted += object.size;
							}
							return Void();
						}));
					}
				}
			}

			// This is just a precaution to avoid having too many outstanding delete actors waiting to run
			while (deleteFutures.size() > CLIENT_KNOBS->BLOBSTORE_CONCURRENT_REQUESTS) {
				wait(deleteFutures.front());
				deleteFutures.pop_front();
			}
		}
	} catch (Error& e) {
		if (e.code() != error_code_end_of_stream)
			throw;
	}

	while (deleteFutures.size() > 0) {
		wait(deleteFutures.front());
		deleteFutures.pop_front();
	}

	return Void();
}

Future<Void> S3BlobStoreEndpoint::deleteRecursively(std::string const& bucket,
                                                    std::string prefix,
                                                    int* pNumDeleted,
                                                    int64_t* pBytesDeleted) {
	return deleteRecursively_impl(
	    Reference<S3BlobStoreEndpoint>::addRef(this), bucket, prefix, pNumDeleted, pBytesDeleted);
}

ACTOR Future<Void> createBucket_impl(Reference<S3BlobStoreEndpoint> b, std::string bucket) {
	wait(b->requestRateWrite->getAllowance(1));

	bool exists = wait(b->bucketExists(bucket));
	if (!exists) {
		std::string resource = constructResourcePath(b, bucket, "");
		HTTP::Headers headers;

		std::string region = b->getRegion();
		if (region.empty()) {
			Reference<HTTP::Response> r = wait(b->doRequest("PUT", resource, headers, nullptr, 0, { 200, 409 }));
		} else {
			UnsentPacketQueue packets;
			StringRef body(format("<CreateBucketConfiguration xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">"
			                      "  <LocationConstraint>%s</LocationConstraint>"
			                      "</CreateBucketConfiguration>",
			                      region.c_str()));
			PacketWriter pw(packets.getWriteBuffer(), nullptr, Unversioned());
			pw.serializeBytes(body);

			Reference<HTTP::Response> r =
			    wait(b->doRequest("PUT", resource, headers, &packets, body.size(), { 200, 409 }));
		}
	}
	return Void();
}

Future<Void> S3BlobStoreEndpoint::createBucket(std::string const& bucket) {
	return createBucket_impl(Reference<S3BlobStoreEndpoint>::addRef(this), bucket);
}

ACTOR Future<int64_t> objectSize_impl(Reference<S3BlobStoreEndpoint> b, std::string bucket, std::string object) {
	wait(b->requestRateRead->getAllowance(1));

	std::string resource = constructResourcePath(b, bucket, object);
	HTTP::Headers headers;

	Reference<HTTP::Response> r = wait(b->doRequest("HEAD", resource, headers, nullptr, 0, { 200, 404 }));
	if (r->code == 404)
		throw file_not_found();
	return r->contentLen;
}

Future<int64_t> S3BlobStoreEndpoint::objectSize(std::string const& bucket, std::string const& object) {
	return objectSize_impl(Reference<S3BlobStoreEndpoint>::addRef(this), bucket, object);
}

// Try to read a file, parse it as JSON, and return the resulting document.
// It will NOT throw if any errors are encountered, it will just return an empty
// JSON object and will log trace events for the errors encountered.
ACTOR Future<Optional<json_spirit::mObject>> tryReadJSONFile(std::string path) {
	state std::string content;

	// Event type to be logged in the event of an exception
	state const char* errorEventType = "BlobCredentialFileError";

	try {
		state Reference<IAsyncFile> f = wait(IAsyncFileSystem::filesystem()->open(
		    path, IAsyncFile::OPEN_NO_AIO | IAsyncFile::OPEN_READONLY | IAsyncFile::OPEN_UNCACHED, 0));
		state int64_t size = wait(f->size());
		state Standalone<StringRef> buf = makeString(size);
		int r = wait(f->read(mutateString(buf), size, 0));
		ASSERT(r == size);
		content = buf.toString();

		// Any exceptions from hehre forward are parse failures
		errorEventType = "BlobCredentialFileParseFailed";
		json_spirit::mValue json;
		json_spirit::read_string(content, json);
		if (json.type() == json_spirit::obj_type)
			return json.get_obj();
		else
			TraceEvent(SevWarn, "BlobCredentialFileNotJSONObject").suppressFor(60).detail("File", path);

	} catch (Error& e) {
		if (e.code() != error_code_actor_cancelled)
			TraceEvent(SevWarn, errorEventType).errorUnsuppressed(e).suppressFor(60).detail("File", path);
	}

	return Optional<json_spirit::mObject>();
}

// If the credentials expire, the connection will eventually fail and be discarded from the pool, and then a new
// connection will be constructed, which will call this again to get updated credentials
static S3BlobStoreEndpoint::Credentials getSecretSdk() {
#ifdef BUILD_AWS_BACKUP
	double elapsed = -timer_monotonic();
	Aws::Auth::AWSCredentials awsCreds = FDBAWSCredentialsProvider::getAwsCredentials();
	elapsed += timer_monotonic();

	if (awsCreds.IsEmpty()) {
		TraceEvent(SevWarn, "S3BlobStoreAWSCredsEmpty");
		throw backup_auth_missing();
	}

	S3BlobStoreEndpoint::Credentials fdbCreds;
	fdbCreds.key = awsCreds.GetAWSAccessKeyId();
	fdbCreds.secret = awsCreds.GetAWSSecretKey();
	fdbCreds.securityToken = awsCreds.GetSessionToken();

	TraceEvent("S3BlobStoreGotSdkCredentials").suppressFor(60).detail("Duration", elapsed);

	return fdbCreds;
#else
	TraceEvent(SevError, "S3BlobStoreNoSDK");
	throw backup_auth_missing();
#endif
}

ACTOR Future<Void> updateSecret_impl(Reference<S3BlobStoreEndpoint> b) {
	if (b->knobs.sdk_auth) {
		b->credentials = getSecretSdk();
		return Void();
	}
	std::vector<std::string>* pFiles = (std::vector<std::string>*)g_network->global(INetwork::enBlobCredentialFiles);
	if (pFiles == nullptr)
		return Void();

	if (!b->credentials.present()) {
		return Void();
	}

	state std::vector<Future<Optional<json_spirit::mObject>>> reads;
	for (auto& f : *pFiles)
		reads.push_back(tryReadJSONFile(f));

	wait(waitForAll(reads));

	std::string accessKey = b->lookupKey ? "" : b->credentials.get().key;
	std::string credentialsFileKey = accessKey + "@" + b->host;

	int invalid = 0;

	for (auto& f : reads) {
		// If value not present then the credentials file wasn't readable or valid.  Continue to check other results.
		if (!f.get().present()) {
			++invalid;
			continue;
		}

		JSONDoc doc(f.get().get());
		if (doc.has("accounts") && doc.last().type() == json_spirit::obj_type) {
			JSONDoc accounts(doc.last().get_obj());
			if (accounts.has(credentialsFileKey, false) && accounts.last().type() == json_spirit::obj_type) {
				JSONDoc account(accounts.last());
				S3BlobStoreEndpoint::Credentials creds = b->credentials.get();
				if (b->lookupKey) {
					std::string apiKey;
					if (account.tryGet("api_key", apiKey))
						creds.key = apiKey;
					else
						continue;
				}
				if (b->lookupSecret) {
					std::string secret;
					if (account.tryGet("secret", secret))
						creds.secret = secret;
					else
						continue;
				}
				std::string token;
				if (account.tryGet("token", token))
					creds.securityToken = token;
				b->credentials = creds;
				return Void();
			}
		}
	}

	// If any sources were invalid
	if (invalid > 0)
		throw backup_auth_unreadable();

	// All sources were valid but didn't contain the desired info
	throw backup_auth_missing();
}

Future<Void> S3BlobStoreEndpoint::updateSecret() {
	return updateSecret_impl(Reference<S3BlobStoreEndpoint>::addRef(this));
}

ACTOR Future<S3BlobStoreEndpoint::ReusableConnection> connect_impl(Reference<S3BlobStoreEndpoint> b) {
	// First try to get a connection from the pool
	while (!b->connectionPool.empty()) {
		S3BlobStoreEndpoint::ReusableConnection rconn = b->connectionPool.front();
		b->connectionPool.pop();

		// If the connection expires in the future then return it
		if (rconn.expirationTime > now()) {
			TraceEvent("S3BlobStoreEndpointReusingConnected")
			    .suppressFor(60)
			    .detail("RemoteEndpoint", rconn.conn->getPeerAddress())
			    .detail("ExpiresIn", rconn.expirationTime - now());
			return rconn;
		}
	}
	std::string host = b->host, service = b->service;
	if (service.empty()) {
		if (b->useProxy) {
			fprintf(stderr, "ERROR: Port can't be empty when using HTTP proxy.\n");
			throw connection_failed();
		}
		service = b->knobs.secure_connection ? "https" : "http";
	}
	bool isTLS = b->knobs.secure_connection == 1;
	state Reference<IConnection> conn;
	if (b->useProxy) {
		if (isTLS) {
			Reference<IConnection> _conn =
			    wait(HTTP::proxyConnect(host, service, b->proxyHost.get(), b->proxyPort.get()));
			conn = _conn;
		} else {
			host = b->proxyHost.get();
			service = b->proxyPort.get();
			Reference<IConnection> _conn = wait(INetworkConnections::net()->connect(host, service, false));
			conn = _conn;
		}
	} else {
		wait(store(conn, INetworkConnections::net()->connect(host, service, isTLS)));
	}
	wait(conn->connectHandshake());

	TraceEvent("S3BlobStoreEndpointNewConnection")
	    .suppressFor(60)
	    .detail("RemoteEndpoint", conn->getPeerAddress())
	    .detail("ExpiresIn", b->knobs.max_connection_life);

	if (b->lookupKey || b->lookupSecret || b->knobs.sdk_auth)
		wait(b->updateSecret());

	return S3BlobStoreEndpoint::ReusableConnection({ conn, now() + b->knobs.max_connection_life });
}

Future<S3BlobStoreEndpoint::ReusableConnection> S3BlobStoreEndpoint::connect() {
	return connect_impl(Reference<S3BlobStoreEndpoint>::addRef(this));
}

void S3BlobStoreEndpoint::returnConnection(ReusableConnection& rconn) {
	// If it expires in the future then add it to the pool in the front
	if (rconn.expirationTime > now())
		connectionPool.push(rconn);
	rconn.conn = Reference<IConnection>();
}

std::string awsCanonicalURI(const std::string& resource, std::vector<std::string>& queryParameters, bool isV4) {
	StringRef resourceRef(resource);
	resourceRef.eat("/");
	std::string canonicalURI("/" + resourceRef.toString());
	size_t q = canonicalURI.find_last_of('?');
	if (q != canonicalURI.npos)
		canonicalURI.resize(q);
	if (isV4) {
		canonicalURI = HTTP::awsV4URIEncode(canonicalURI, false);
	} else {
		canonicalURI = HTTP::urlEncode(canonicalURI);
	}

	// Create the canonical query string
	std::string queryString;
	q = resource.find_last_of('?');
	if (q != queryString.npos)
		queryString = resource.substr(q + 1);

	StringRef qStr(queryString);
	StringRef queryParameter;
	while ((queryParameter = qStr.eat("&")) != StringRef()) {
		StringRef param = queryParameter.eat("=");
		StringRef value = queryParameter.eat();

		if (isV4) {
			queryParameters.push_back(HTTP::awsV4URIEncode(param.toString(), true) + "=" +
			                          HTTP::awsV4URIEncode(value.toString(), true));
		} else {
			queryParameters.push_back(HTTP::urlEncode(param.toString()) + "=" + HTTP::urlEncode(value.toString()));
		}
	}

	return canonicalURI;
}

// Do a request, get a Response.
// Request content is provided as UnsentPacketQueue *pContent which will be depleted as bytes are sent but the queue
// itself must live for the life of this actor and be destroyed by the caller
ACTOR Future<Reference<HTTP::Response>> doRequest_impl(Reference<S3BlobStoreEndpoint> bstore,
                                                       std::string verb,
                                                       std::string resource,
                                                       HTTP::Headers headers,
                                                       UnsentPacketQueue* pContent,
                                                       int contentLen,
                                                       std::set<unsigned int> successCodes) {
	state UnsentPacketQueue contentCopy;

	headers["Content-Length"] = format("%d", contentLen);
	headers["Host"] = bstore->host;
	headers["Accept"] = "application/xml";

	// Merge extraHeaders into headers
	for (const auto& [k, v] : bstore->extraHeaders) {
		std::string& fieldValue = headers[k];
		if (!fieldValue.empty()) {
			fieldValue.append(",");
		}
		fieldValue.append(v);
	}

	// For requests with content to upload, the request timeout should be at least twice the amount of time
	// it would take to upload the content given the upload bandwidth and concurrency limits.
	int bandwidthThisRequest = 1 + bstore->knobs.max_send_bytes_per_second / bstore->knobs.concurrent_uploads;
	int contentUploadSeconds = contentLen / bandwidthThisRequest;
	state int requestTimeout = std::max(bstore->knobs.request_timeout_min, 3 * contentUploadSeconds);

	wait(bstore->concurrentRequests.take());
	state FlowLock::Releaser globalReleaser(bstore->concurrentRequests, 1);

	state int maxTries = std::min(bstore->knobs.request_tries, bstore->knobs.connect_tries);
	state int thisTry = 1;
	state double nextRetryDelay = 2.0;

	loop {
		state Optional<Error> err;
		state Optional<NetworkAddress> remoteAddress;
		state bool connectionEstablished = false;
		state Reference<HTTP::Response> r;
		state std::string canonicalURI = resource;

		try {
			// Start connecting
			Future<S3BlobStoreEndpoint::ReusableConnection> frconn = bstore->connect();

			// Make a shallow copy of the queue by calling addref() on each buffer in the chain and then prepending that
			// chain to contentCopy
			contentCopy.discardAll();
			if (pContent != nullptr) {
				PacketBuffer* pFirst = pContent->getUnsent();
				PacketBuffer* pLast = nullptr;
				for (PacketBuffer* p = pFirst; p != nullptr; p = p->nextPacketBuffer()) {
					p->addref();
					// Also reset the sent count on each buffer
					p->bytes_sent = 0;
					pLast = p;
				}
				contentCopy.prependWriteBuffer(pFirst, pLast);
			}

			// Finish connecting, do request
			state S3BlobStoreEndpoint::ReusableConnection rconn =
			    wait(timeoutError(frconn, bstore->knobs.connect_timeout));
			connectionEstablished = true;

			// Finish/update the request headers (which includes Date header)
			// This must be done AFTER the connection is ready because if credentials are coming from disk they are
			// refreshed when a new connection is established and setAuthHeaders() would need the updated secret.
			if (bstore->credentials.present() && !bstore->credentials.get().securityToken.empty())
				headers["x-amz-security-token"] = bstore->credentials.get().securityToken;
			if (CLIENT_KNOBS->HTTP_REQUEST_AWS_V4_HEADER) {
				bstore->setV4AuthHeaders(verb, resource, headers);
			} else {
				bstore->setAuthHeaders(verb, resource, headers);
			}

			std::vector<std::string> queryParameters;
			canonicalURI = awsCanonicalURI(resource, queryParameters, CLIENT_KNOBS->HTTP_REQUEST_AWS_V4_HEADER);
			if (!queryParameters.empty()) {
				canonicalURI += "?";
				canonicalURI += boost::algorithm::join(queryParameters, "&");
			}

			if (bstore->useProxy && bstore->knobs.secure_connection == 0) {
				// Has to be in absolute-form.
				canonicalURI = "http://" + bstore->host + ":" + bstore->service + canonicalURI;
			}

			remoteAddress = rconn.conn->getPeerAddress();
			wait(bstore->requestRate->getAllowance(1));
			Reference<HTTP::Response> _r = wait(timeoutError(HTTP::doRequest(rconn.conn,
			                                                                 verb,
			                                                                 canonicalURI,
			                                                                 headers,
			                                                                 &contentCopy,
			                                                                 contentLen,
			                                                                 bstore->sendRate,
			                                                                 &bstore->s_stats.bytes_sent,
			                                                                 bstore->recvRate),
			                                                 requestTimeout));
			r = _r;

			// Since the response was parsed successfully (which is why we are here) reuse the connection unless we
			// received the "Connection: close" header.
			if (r->headers["Connection"] != "close")
				bstore->returnConnection(rconn);
			rconn.conn.clear();

		} catch (Error& e) {
			if (e.code() == error_code_actor_cancelled)
				throw;
			err = e;
		}

		// If err is not present then r is valid.
		// If r->code is in successCodes then record the successful request and return r.
		if (!err.present() && successCodes.count(r->code) != 0) {
			bstore->s_stats.requests_successful++;
			return r;
		}

		// Otherwise, this request is considered failed.  Update failure count.
		bstore->s_stats.requests_failed++;

		// All errors in err are potentially retryable as well as certain HTTP response codes...
		bool retryable = err.present() || r->code == 500 || r->code == 502 || r->code == 503 || r->code == 429;

		// But only if our previous attempt was not the last allowable try.
		retryable = retryable && (thisTry < maxTries);

		TraceEvent event(SevWarn,
		                 retryable ? "S3BlobStoreEndpointRequestFailedRetryable" : "S3BlobStoreEndpointRequestFailed");

		// Attach err to trace event if present, otherwise extract some stuff from the response
		if (err.present()) {
			event.errorUnsuppressed(err.get());
		}
		event.suppressFor(60);
		if (!err.present()) {
			event.detail("ResponseCode", r->code);
		}

		event.detail("ConnectionEstablished", connectionEstablished);

		if (remoteAddress.present())
			event.detail("RemoteEndpoint", remoteAddress.get());
		else
			event.detail("RemoteHost", bstore->host);

		event.detail("Verb", verb).detail("Resource", resource).detail("ThisTry", thisTry);

		// If r is not valid or not code 429 then increment the try count.  429's will not count against the attempt
		// limit.
		if (!r || r->code != 429)
			++thisTry;

		// We will wait delay seconds before the next retry, start with nextRetryDelay.
		double delay = nextRetryDelay;
		// Double but limit the *next* nextRetryDelay.
		nextRetryDelay = std::min(nextRetryDelay * 2, 60.0);

		if (retryable) {
			// If r is valid then obey the Retry-After response header if present.
			if (r) {
				auto iRetryAfter = r->headers.find("Retry-After");
				if (iRetryAfter != r->headers.end()) {
					event.detail("RetryAfterHeader", iRetryAfter->second);
					char* pEnd;
					double retryAfter = strtod(iRetryAfter->second.c_str(), &pEnd);
					if (*pEnd) // If there were other characters then don't trust the parsed value, use a probably safe
					           // value of 5 minutes.
						retryAfter = 300;
					// Update delay
					delay = std::max(delay, retryAfter);
				}
			}

			// Log the delay then wait.
			event.detail("RetryDelay", delay);
			wait(::delay(delay));
		} else {
			// We can't retry, so throw something.

			// This error code means the authentication header was not accepted, likely the account or key is wrong.
			if (r && r->code == 406)
				throw http_not_accepted();

			if (r && r->code == 401)
				throw http_auth_failed();

			// Recognize and throw specific errors
			if (err.present()) {
				int code = err.get().code();

				// If we get a timed_out error during the the connect() phase, we'll call that connection_failed despite
				// the fact that there was technically never a 'connection' to begin with.  It differentiates between an
				// active connection timing out vs a connection timing out, though not between an active connection
				// failing vs connection attempt failing.
				// TODO:  Add more error types?
				if (code == error_code_timed_out && !connectionEstablished)
					throw connection_failed();

				if (code == error_code_timed_out || code == error_code_connection_failed ||
				    code == error_code_lookup_failed)
					throw err.get();
			}

			throw http_request_failed();
		}
	}
}

Future<Reference<HTTP::Response>> S3BlobStoreEndpoint::doRequest(std::string const& verb,
                                                                 std::string const& resource,
                                                                 const HTTP::Headers& headers,
                                                                 UnsentPacketQueue* pContent,
                                                                 int contentLen,
                                                                 std::set<unsigned int> successCodes) {
	return doRequest_impl(
	    Reference<S3BlobStoreEndpoint>::addRef(this), verb, resource, headers, pContent, contentLen, successCodes);
}

ACTOR Future<Void> listObjectsStream_impl(Reference<S3BlobStoreEndpoint> bstore,
                                          std::string bucket,
                                          PromiseStream<S3BlobStoreEndpoint::ListResult> results,
                                          Optional<std::string> prefix,
                                          Optional<char> delimiter,
                                          int maxDepth,
                                          std::function<bool(std::string const&)> recurseFilter) {
	// Request 1000 keys at a time, the maximum allowed
	state std::string resource = constructResourcePath(bstore, bucket, "");

	resource.append("/?max-keys=1000");
	if (prefix.present())
		resource.append("&prefix=").append(prefix.get());
	if (delimiter.present())
		resource.append("&delimiter=").append(std::string(1, delimiter.get()));
	resource.append("&marker=");
	state std::string lastFile;
	state bool more = true;

	state std::vector<Future<Void>> subLists;

	while (more) {
		wait(bstore->concurrentLists.take());
		state FlowLock::Releaser listReleaser(bstore->concurrentLists, 1);

		HTTP::Headers headers;
		state std::string fullResource = resource + lastFile;
		lastFile.clear();
		Reference<HTTP::Response> r = wait(bstore->doRequest("GET", fullResource, headers, nullptr, 0, { 200 }));
		listReleaser.release();

		try {
			S3BlobStoreEndpoint::ListResult listResult;
			xml_document<> doc;

			// Copy content because rapidxml will modify it during parse
			std::string content = r->content;
			doc.parse<0>((char*)content.c_str());

			// There should be exactly one node
			xml_node<>* result = doc.first_node();
			if (result == nullptr || strcmp(result->name(), "ListBucketResult") != 0) {
				throw http_bad_response();
			}

			xml_node<>* n = result->first_node();
			while (n != nullptr) {
				const char* name = n->name();
				if (strcmp(name, "IsTruncated") == 0) {
					const char* val = n->value();
					if (strcmp(val, "true") == 0) {
						more = true;
					} else if (strcmp(val, "false") == 0) {
						more = false;
					} else {
						throw http_bad_response();
					}
				} else if (strcmp(name, "Contents") == 0) {
					S3BlobStoreEndpoint::ObjectInfo object;

					xml_node<>* key = n->first_node("Key");
					if (key == nullptr) {
						throw http_bad_response();
					}
					object.name = key->value();

					xml_node<>* size = n->first_node("Size");
					if (size == nullptr) {
						throw http_bad_response();
					}
					object.size = strtoull(size->value(), nullptr, 10);

					listResult.objects.push_back(object);
				} else if (strcmp(name, "CommonPrefixes") == 0) {
					xml_node<>* prefixNode = n->first_node("Prefix");
					while (prefixNode != nullptr) {
						const char* prefix = prefixNode->value();
						// If recursing, queue a sub-request, otherwise add the common prefix to the result.
						if (maxDepth > 0) {
							// If there is no recurse filter or the filter returns true then start listing the subfolder
							if (!recurseFilter || recurseFilter(prefix)) {
								subLists.push_back(bstore->listObjectsStream(
								    bucket, results, prefix, delimiter, maxDepth - 1, recurseFilter));
							}
							// Since prefix will not be in the final listResult below we have to set lastFile here in
							// case it's greater than the last object
							lastFile = prefix;
						} else {
							listResult.commonPrefixes.push_back(prefix);
						}

						prefixNode = prefixNode->next_sibling("Prefix");
					}
				}

				n = n->next_sibling();
			}

			results.send(listResult);

			if (more) {
				// lastFile will be the last commonprefix for which a sublist was started, if any.
				// If there are any objects and the last one is greater than lastFile then make it the new lastFile.
				if (!listResult.objects.empty() && lastFile < listResult.objects.back().name) {
					lastFile = listResult.objects.back().name;
				}
				// If there are any common prefixes and the last one is greater than lastFile then make it the new
				// lastFile.
				if (!listResult.commonPrefixes.empty() && lastFile < listResult.commonPrefixes.back()) {
					lastFile = listResult.commonPrefixes.back();
				}

				// If lastFile is empty at this point, something has gone wrong.
				if (lastFile.empty()) {
					TraceEvent(SevWarn, "S3BlobStoreEndpointListNoNextMarker")
					    .suppressFor(60)
					    .detail("Resource", fullResource);
					throw http_bad_response();
				}
			}
		} catch (Error& e) {
			if (e.code() != error_code_actor_cancelled)
				TraceEvent(SevWarn, "S3BlobStoreEndpointListResultParseError")
				    .errorUnsuppressed(e)
				    .suppressFor(60)
				    .detail("Resource", fullResource);
			throw http_bad_response();
		}
	}

	wait(waitForAll(subLists));

	return Void();
}

Future<Void> S3BlobStoreEndpoint::listObjectsStream(std::string const& bucket,
                                                    PromiseStream<ListResult> results,
                                                    Optional<std::string> prefix,
                                                    Optional<char> delimiter,
                                                    int maxDepth,
                                                    std::function<bool(std::string const&)> recurseFilter) {
	return listObjectsStream_impl(
	    Reference<S3BlobStoreEndpoint>::addRef(this), bucket, results, prefix, delimiter, maxDepth, recurseFilter);
}

ACTOR Future<S3BlobStoreEndpoint::ListResult> listObjects_impl(Reference<S3BlobStoreEndpoint> bstore,
                                                               std::string bucket,
                                                               Optional<std::string> prefix,
                                                               Optional<char> delimiter,
                                                               int maxDepth,
                                                               std::function<bool(std::string const&)> recurseFilter) {
	state S3BlobStoreEndpoint::ListResult results;
	state PromiseStream<S3BlobStoreEndpoint::ListResult> resultStream;
	state Future<Void> done =
	    bstore->listObjectsStream(bucket, resultStream, prefix, delimiter, maxDepth, recurseFilter);
	// Wrap done in an actor which sends end_of_stream because list does not so that many lists can write to the same
	// stream
	done = map(done, [=](Void) mutable {
		resultStream.sendError(end_of_stream());
		return Void();
	});

	try {
		loop {
			choose {
				// Throw if done throws, otherwise don't stop until end_of_stream
				when(wait(done)) { done = Never(); }

				when(S3BlobStoreEndpoint::ListResult info = waitNext(resultStream.getFuture())) {
					results.commonPrefixes.insert(
					    results.commonPrefixes.end(), info.commonPrefixes.begin(), info.commonPrefixes.end());
					results.objects.insert(results.objects.end(), info.objects.begin(), info.objects.end());
				}
			}
		}
	} catch (Error& e) {
		if (e.code() != error_code_end_of_stream)
			throw;
	}

	return results;
}

Future<S3BlobStoreEndpoint::ListResult> S3BlobStoreEndpoint::listObjects(
    std::string const& bucket,
    Optional<std::string> prefix,
    Optional<char> delimiter,
    int maxDepth,
    std::function<bool(std::string const&)> recurseFilter) {
	return listObjects_impl(
	    Reference<S3BlobStoreEndpoint>::addRef(this), bucket, prefix, delimiter, maxDepth, recurseFilter);
}

ACTOR Future<std::vector<std::string>> listBuckets_impl(Reference<S3BlobStoreEndpoint> bstore) {
	state std::string resource = "/?marker=";
	state std::string lastName;
	state bool more = true;
	state std::vector<std::string> buckets;

	while (more) {
		wait(bstore->concurrentLists.take());
		state FlowLock::Releaser listReleaser(bstore->concurrentLists, 1);

		HTTP::Headers headers;
		state std::string fullResource = resource + lastName;
		Reference<HTTP::Response> r = wait(bstore->doRequest("GET", fullResource, headers, nullptr, 0, { 200 }));
		listReleaser.release();

		try {
			xml_document<> doc;

			// Copy content because rapidxml will modify it during parse
			std::string content = r->content;
			doc.parse<0>((char*)content.c_str());

			// There should be exactly one node
			xml_node<>* result = doc.first_node();
			if (result == nullptr || strcmp(result->name(), "ListAllMyBucketsResult") != 0) {
				throw http_bad_response();
			}

			more = false;
			xml_node<>* truncated = result->first_node("IsTruncated");
			if (truncated != nullptr && strcmp(truncated->value(), "true") == 0) {
				more = true;
			}

			xml_node<>* bucketsNode = result->first_node("Buckets");
			if (bucketsNode != nullptr) {
				xml_node<>* bucketNode = bucketsNode->first_node("Bucket");
				while (bucketNode != nullptr) {
					xml_node<>* nameNode = bucketNode->first_node("Name");
					if (nameNode == nullptr) {
						throw http_bad_response();
					}
					const char* name = nameNode->value();
					buckets.push_back(name);

					bucketNode = bucketNode->next_sibling("Bucket");
				}
			}

			if (more) {
				lastName = buckets.back();
			}

		} catch (Error& e) {
			if (e.code() != error_code_actor_cancelled)
				TraceEvent(SevWarn, "S3BlobStoreEndpointListBucketResultParseError")
				    .errorUnsuppressed(e)
				    .suppressFor(60)
				    .detail("Resource", fullResource);
			throw http_bad_response();
		}
	}

	return buckets;
}

Future<std::vector<std::string>> S3BlobStoreEndpoint::listBuckets() {
	return listBuckets_impl(Reference<S3BlobStoreEndpoint>::addRef(this));
}

std::string S3BlobStoreEndpoint::hmac_sha1(Credentials const& creds, std::string const& msg) {
	std::string key = creds.secret;

	// Hash key to shorten it if it is longer than SHA1 block size
	if (key.size() > 64) {
		key = SHA1::from_string(key);
	}

	// Pad key up to SHA1 block size if needed
	key.append(64 - key.size(), '\0');

	std::string kipad = key;
	for (int i = 0; i < 64; ++i)
		kipad[i] ^= '\x36';

	std::string kopad = key;
	for (int i = 0; i < 64; ++i)
		kopad[i] ^= '\x5c';

	kipad.append(msg);
	std::string hkipad = SHA1::from_string(kipad);
	kopad.append(hkipad);
	return SHA1::from_string(kopad);
}

std::string sha256_hex(std::string str) {
	unsigned char hash[SHA256_DIGEST_LENGTH];
	SHA256_CTX sha256;
	SHA256_Init(&sha256);
	SHA256_Update(&sha256, str.c_str(), str.size());
	SHA256_Final(hash, &sha256);
	std::stringstream ss;
	for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
		ss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
	}
	return ss.str();
}

std::string hmac_sha256_hex(std::string key, std::string msg) {
	unsigned char hash[32];

	HMAC_CTX* hmac = HMAC_CTX_new();
	HMAC_Init_ex(hmac, &key[0], key.length(), EVP_sha256(), NULL);
	HMAC_Update(hmac, (unsigned char*)&msg[0], msg.length());
	unsigned int len = 32;
	HMAC_Final(hmac, hash, &len);
	HMAC_CTX_free(hmac);

	std::stringstream ss;
	ss << std::hex << std::setfill('0');
	for (int i = 0; i < len; i++) {
		ss << std::hex << std::setw(2) << (unsigned int)hash[i];
	}
	return (ss.str());
}

std::string hmac_sha256(std::string key, std::string msg) {
	unsigned char hash[32];

	HMAC_CTX* hmac = HMAC_CTX_new();
	HMAC_Init_ex(hmac, &key[0], key.length(), EVP_sha256(), NULL);
	HMAC_Update(hmac, (unsigned char*)&msg[0], msg.length());
	unsigned int len = 32;
	HMAC_Final(hmac, hash, &len);
	HMAC_CTX_free(hmac);

	std::stringstream ss;
	ss << std::setfill('0');
	for (int i = 0; i < len; i++) {
		ss << hash[i];
	}
	return (ss.str());
}

// Date and Time parameters are used for unit testing
void S3BlobStoreEndpoint::setV4AuthHeaders(std::string const& verb,
                                           std::string const& resource,
                                           HTTP::Headers& headers,
                                           std::string date,
                                           std::string datestamp) {
	if (!credentials.present()) {
		return;
	}
	Credentials creds = credentials.get();
	// std::cout << "========== Starting===========" << std::endl;
	std::string accessKey = creds.key;
	std::string secretKey = creds.secret;
	// Create a date for headers and the credential string
	std::string amzDate;
	std::string dateStamp;
	if (date.empty() || datestamp.empty()) {
		time_t ts;
		time(&ts);
		char dateBuf[20];
		// ISO 8601 format YYYYMMDD'T'HHMMSS'Z'
		strftime(dateBuf, 20, "%Y%m%dT%H%M%SZ", gmtime(&ts));
		amzDate = dateBuf;
		strftime(dateBuf, 20, "%Y%m%d", gmtime(&ts));
		dateStamp = dateBuf;
	} else {
		amzDate = date;
		dateStamp = datestamp;
	}

	// ************* TASK 1: CREATE A CANONICAL REQUEST *************
	// Create Create canonical URI--the part of the URI from domain to query string (use '/' if no path)
	std::vector<std::string> queryParameters;
	std::string canonicalURI = awsCanonicalURI(resource, queryParameters, true);

	std::string canonicalQueryString;
	if (!queryParameters.empty()) {
		std::sort(queryParameters.begin(), queryParameters.end());
		canonicalQueryString = boost::algorithm::join(queryParameters, "&");
	}

	using namespace boost::algorithm;
	// Create the canonical headers and signed headers
	ASSERT(!headers["Host"].empty());
	// Using unsigned payload here and adding content-md5 to the signed headers. It may be better to also include sha256
	// sum for added security.
	headers["x-amz-content-sha256"] = "UNSIGNED-PAYLOAD";
	headers["x-amz-date"] = amzDate;
	std::vector<std::pair<std::string, std::string>> headersList;
	headersList.push_back({ "host", trim_copy(headers["Host"]) + "\n" });
	if (headers.find("Content-Type") != headers.end())
		headersList.push_back({ "content-type", trim_copy(headers["Content-Type"]) + "\n" });
	if (headers.find("Content-MD5") != headers.end())
		headersList.push_back({ "content-md5", trim_copy(headers["Content-MD5"]) + "\n" });
	for (auto h : headers) {
		if (StringRef(h.first).startsWith("x-amz"_sr))
			headersList.push_back({ to_lower_copy(h.first), trim_copy(h.second) + "\n" });
	}
	std::sort(headersList.begin(), headersList.end());
	std::string canonicalHeaders;
	std::string signedHeaders;
	for (auto& i : headersList) {
		canonicalHeaders += i.first + ":" + i.second;
		signedHeaders += i.first + ";";
	}
	signedHeaders.pop_back();
	std::string canonicalRequest = verb + "\n" + canonicalURI + "\n" + canonicalQueryString + "\n" + canonicalHeaders +
	                               "\n" + signedHeaders + "\n" + headers["x-amz-content-sha256"];

	// ************* TASK 2: CREATE THE STRING TO SIGN*************
	std::string algorithm = "AWS4-HMAC-SHA256";
	std::string credentialScope = dateStamp + "/" + region + "/s3/" + "aws4_request";
	std::string stringToSign =
	    algorithm + "\n" + amzDate + "\n" + credentialScope + "\n" + sha256_hex(canonicalRequest);

	// ************* TASK 3: CALCULATE THE SIGNATURE *************
	// Create the signing key using the function defined above.
	std::string signingKey =
	    hmac_sha256(hmac_sha256(hmac_sha256(hmac_sha256("AWS4" + secretKey, dateStamp), region), "s3"), "aws4_request");
	// Sign the string_to_sign using the signing_key
	std::string signature = hmac_sha256_hex(signingKey, stringToSign);
	// ************* TASK 4: ADD SIGNING INFORMATION TO THE Header *************
	std::string authorizationHeader = algorithm + " " + "Credential=" + accessKey + "/" + credentialScope + ", " +
	                                  "SignedHeaders=" + signedHeaders + ", " + "Signature=" + signature;
	headers["Authorization"] = authorizationHeader;
}

void S3BlobStoreEndpoint::setAuthHeaders(std::string const& verb, std::string const& resource, HTTP::Headers& headers) {
	if (!credentials.present()) {
		return;
	}
	Credentials creds = credentials.get();

	std::string& date = headers["Date"];

	char dateBuf[64];
	time_t ts;
	time(&ts);
	strftime(dateBuf, 64, "%a, %d %b %Y %H:%M:%S GMT", gmtime(&ts));
	date = dateBuf;

	std::string msg;
	msg.append(verb);
	msg.append("\n");
	auto contentMD5 = headers.find("Content-MD5");
	if (contentMD5 != headers.end())
		msg.append(contentMD5->second);
	msg.append("\n");
	auto contentType = headers.find("Content-Type");
	if (contentType != headers.end())
		msg.append(contentType->second);
	msg.append("\n");
	msg.append(date);
	msg.append("\n");
	for (auto h : headers) {
		StringRef name = h.first;
		if (name.startsWith("x-amz"_sr) || name.startsWith("x-icloud"_sr)) {
			msg.append(h.first);
			msg.append(":");
			msg.append(h.second);
			msg.append("\n");
		}
	}

	msg.append(resource);
	if (verb == "GET") {
		size_t q = resource.find_last_of('?');
		if (q != resource.npos)
			msg.resize(msg.size() - (resource.size() - q));
	}

	std::string sig = base64::encoder::from_string(hmac_sha1(creds, msg));
	// base64 encoded blocks end in \n so remove it.
	sig.resize(sig.size() - 1);
	std::string auth = "AWS ";
	auth.append(creds.key);
	auth.append(":");
	auth.append(sig);
	headers["Authorization"] = auth;
}

ACTOR Future<std::string> readEntireFile_impl(Reference<S3BlobStoreEndpoint> bstore,
                                              std::string bucket,
                                              std::string object) {
	wait(bstore->requestRateRead->getAllowance(1));

	std::string resource = constructResourcePath(bstore, bucket, object);
	HTTP::Headers headers;
	Reference<HTTP::Response> r = wait(bstore->doRequest("GET", resource, headers, nullptr, 0, { 200, 404 }));
	if (r->code == 404)
		throw file_not_found();
	return r->content;
}

Future<std::string> S3BlobStoreEndpoint::readEntireFile(std::string const& bucket, std::string const& object) {
	return readEntireFile_impl(Reference<S3BlobStoreEndpoint>::addRef(this), bucket, object);
}

ACTOR Future<Void> writeEntireFileFromBuffer_impl(Reference<S3BlobStoreEndpoint> bstore,
                                                  std::string bucket,
                                                  std::string object,
                                                  UnsentPacketQueue* pContent,
                                                  int contentLen,
                                                  std::string contentMD5) {
	if (contentLen > bstore->knobs.multipart_max_part_size)
		throw file_too_large();

	wait(bstore->requestRateWrite->getAllowance(1));
	wait(bstore->concurrentUploads.take());
	state FlowLock::Releaser uploadReleaser(bstore->concurrentUploads, 1);

	std::string resource = constructResourcePath(bstore, bucket, object);
	HTTP::Headers headers;
	// Send MD5 sum for content so blobstore can verify it
	headers["Content-MD5"] = contentMD5;
	if (!CLIENT_KNOBS->BLOBSTORE_ENCRYPTION_TYPE.empty())
		headers["x-amz-server-side-encryption"] = CLIENT_KNOBS->BLOBSTORE_ENCRYPTION_TYPE;
	state Reference<HTTP::Response> r =
	    wait(bstore->doRequest("PUT", resource, headers, pContent, contentLen, { 200 }));

	// For uploads, Blobstore returns an MD5 sum of uploaded content so check it.
	if (!r->verifyMD5(false, contentMD5))
		throw checksum_failed();

	return Void();
}

ACTOR Future<Void> writeEntireFile_impl(Reference<S3BlobStoreEndpoint> bstore,
                                        std::string bucket,
                                        std::string object,
                                        std::string content) {
	state UnsentPacketQueue packets;
	PacketWriter pw(packets.getWriteBuffer(content.size()), nullptr, Unversioned());
	pw.serializeBytes(content);
	if (content.size() > bstore->knobs.multipart_max_part_size)
		throw file_too_large();

	// Yield because we may have just had to copy several MB's into packet buffer chain and next we have to calculate an
	// MD5 sum of it.
	// TODO:  If this actor is used to send large files then combine the summing and packetization into a loop with a
	// yield() every 20k or so.
	wait(yield());

	MD5_CTX sum;
	::MD5_Init(&sum);
	::MD5_Update(&sum, content.data(), content.size());
	std::string sumBytes;
	sumBytes.resize(16);
	::MD5_Final((unsigned char*)sumBytes.data(), &sum);
	std::string contentMD5 = base64::encoder::from_string(sumBytes);
	contentMD5.resize(contentMD5.size() - 1);

	wait(writeEntireFileFromBuffer_impl(bstore, bucket, object, &packets, content.size(), contentMD5));
	return Void();
}

Future<Void> S3BlobStoreEndpoint::writeEntireFile(std::string const& bucket,
                                                  std::string const& object,
                                                  std::string const& content) {
	return writeEntireFile_impl(Reference<S3BlobStoreEndpoint>::addRef(this), bucket, object, content);
}

Future<Void> S3BlobStoreEndpoint::writeEntireFileFromBuffer(std::string const& bucket,
                                                            std::string const& object,
                                                            UnsentPacketQueue* pContent,
                                                            int contentLen,
                                                            std::string const& contentMD5) {
	return writeEntireFileFromBuffer_impl(
	    Reference<S3BlobStoreEndpoint>::addRef(this), bucket, object, pContent, contentLen, contentMD5);
}

ACTOR Future<int> readObject_impl(Reference<S3BlobStoreEndpoint> bstore,
                                  std::string bucket,
                                  std::string object,
                                  void* data,
                                  int length,
                                  int64_t offset) {
	if (length <= 0)
		return 0;
	wait(bstore->requestRateRead->getAllowance(1));

	std::string resource = constructResourcePath(bstore, bucket, object);
	HTTP::Headers headers;
	headers["Range"] = format("bytes=%lld-%lld", offset, offset + length - 1);
	Reference<HTTP::Response> r = wait(bstore->doRequest("GET", resource, headers, nullptr, 0, { 200, 206, 404 }));
	if (r->code == 404)
		throw file_not_found();
	if (r->contentLen !=
	    r->content.size()) // Double check that this wasn't a header-only response, probably unnecessary
		throw io_error();
	// Copy the output bytes, server could have sent more or less bytes than requested so copy at most length bytes
	memcpy(data, r->content.data(), std::min<int64_t>(r->contentLen, length));
	return r->contentLen;
}

Future<int> S3BlobStoreEndpoint::readObject(std::string const& bucket,
                                            std::string const& object,
                                            void* data,
                                            int length,
                                            int64_t offset) {
	return readObject_impl(Reference<S3BlobStoreEndpoint>::addRef(this), bucket, object, data, length, offset);
}

ACTOR static Future<std::string> beginMultiPartUpload_impl(Reference<S3BlobStoreEndpoint> bstore,
                                                           std::string bucket,
                                                           std::string object) {
	wait(bstore->requestRateWrite->getAllowance(1));

	std::string resource = constructResourcePath(bstore, bucket, object);
	resource += "?uploads";
	HTTP::Headers headers;
	if (!CLIENT_KNOBS->BLOBSTORE_ENCRYPTION_TYPE.empty())
		headers["x-amz-server-side-encryption"] = CLIENT_KNOBS->BLOBSTORE_ENCRYPTION_TYPE;
	Reference<HTTP::Response> r = wait(bstore->doRequest("POST", resource, headers, nullptr, 0, { 200 }));

	try {
		xml_document<> doc;
		// Copy content because rapidxml will modify it during parse
		std::string content = r->content;

		doc.parse<0>((char*)content.c_str());

		// There should be exactly one node
		xml_node<>* result = doc.first_node();
		if (result != nullptr && strcmp(result->name(), "InitiateMultipartUploadResult") == 0) {
			xml_node<>* id = result->first_node("UploadId");
			if (id != nullptr) {
				return id->value();
			}
		}
	} catch (...) {
	}
	throw http_bad_response();
}

Future<std::string> S3BlobStoreEndpoint::beginMultiPartUpload(std::string const& bucket, std::string const& object) {
	return beginMultiPartUpload_impl(Reference<S3BlobStoreEndpoint>::addRef(this), bucket, object);
}

ACTOR Future<std::string> uploadPart_impl(Reference<S3BlobStoreEndpoint> bstore,
                                          std::string bucket,
                                          std::string object,
                                          std::string uploadID,
                                          unsigned int partNumber,
                                          UnsentPacketQueue* pContent,
                                          int contentLen,
                                          std::string contentMD5) {
	wait(bstore->requestRateWrite->getAllowance(1));
	wait(bstore->concurrentUploads.take());
	state FlowLock::Releaser uploadReleaser(bstore->concurrentUploads, 1);

	std::string resource = constructResourcePath(bstore, bucket, object);
	resource += format("?partNumber=%d&uploadId=%s", partNumber, uploadID.c_str());
	HTTP::Headers headers;
	// Send MD5 sum for content so blobstore can verify it
	headers["Content-MD5"] = contentMD5;
	state Reference<HTTP::Response> r =
	    wait(bstore->doRequest("PUT", resource, headers, pContent, contentLen, { 200 }));
	// TODO:  In the event that the client times out just before the request completes (so the client is unaware) then
	// the next retry will see error 400.  That could be detected and handled gracefully by retrieving the etag for the
	// successful request.

	// For uploads, Blobstore returns an MD5 sum of uploaded content so check it.
	if (!r->verifyMD5(false, contentMD5))
		throw checksum_failed();

	// No etag -> bad response.
	std::string etag = r->headers["ETag"];
	if (etag.empty())
		throw http_bad_response();

	return etag;
}

Future<std::string> S3BlobStoreEndpoint::uploadPart(std::string const& bucket,
                                                    std::string const& object,
                                                    std::string const& uploadID,
                                                    unsigned int partNumber,
                                                    UnsentPacketQueue* pContent,
                                                    int contentLen,
                                                    std::string const& contentMD5) {
	return uploadPart_impl(Reference<S3BlobStoreEndpoint>::addRef(this),
	                       bucket,
	                       object,
	                       uploadID,
	                       partNumber,
	                       pContent,
	                       contentLen,
	                       contentMD5);
}

ACTOR Future<Void> finishMultiPartUpload_impl(Reference<S3BlobStoreEndpoint> bstore,
                                              std::string bucket,
                                              std::string object,
                                              std::string uploadID,
                                              S3BlobStoreEndpoint::MultiPartSetT parts) {
	state UnsentPacketQueue part_list; // NonCopyable state var so must be declared at top of actor
	wait(bstore->requestRateWrite->getAllowance(1));

	std::string manifest = "<CompleteMultipartUpload>";
	for (auto& p : parts)
		manifest += format("<Part><PartNumber>%d</PartNumber><ETag>%s</ETag></Part>\n", p.first, p.second.c_str());
	manifest += "</CompleteMultipartUpload>";

	std::string resource = constructResourcePath(bstore, bucket, object);
	resource += format("?uploadId=%s", uploadID.c_str());
	HTTP::Headers headers;
	PacketWriter pw(part_list.getWriteBuffer(manifest.size()), nullptr, Unversioned());
	pw.serializeBytes(manifest);
	Reference<HTTP::Response> r =
	    wait(bstore->doRequest("POST", resource, headers, &part_list, manifest.size(), { 200 }));
	// TODO:  In the event that the client times out just before the request completes (so the client is unaware) then
	// the next retry will see error 400.  That could be detected and handled gracefully by HEAD'ing the object before
	// upload to get its (possibly nonexistent) eTag, then if an error 400 is seen then retrieve the eTag again and if
	// it has changed then consider the finish complete.
	return Void();
}

Future<Void> S3BlobStoreEndpoint::finishMultiPartUpload(std::string const& bucket,
                                                        std::string const& object,
                                                        std::string const& uploadID,
                                                        MultiPartSetT const& parts) {
	return finishMultiPartUpload_impl(Reference<S3BlobStoreEndpoint>::addRef(this), bucket, object, uploadID, parts);
}

TEST_CASE("/backup/s3/v4headers") {
	S3BlobStoreEndpoint::Credentials creds{ "AKIAIOSFODNN7EXAMPLE", "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY", "" };
	// GET without query parameters
	{
		S3BlobStoreEndpoint s3("s3.amazonaws.com", "443", "amazonaws", "proxy", "port", creds);
		std::string verb("GET");
		std::string resource("/test.txt");
		HTTP::Headers headers;
		headers["Host"] = "s3.amazonaws.com";
		s3.setV4AuthHeaders(verb, resource, headers, "20130524T000000Z", "20130524");
		ASSERT(headers["Authorization"] ==
		       "AWS4-HMAC-SHA256 Credential=AKIAIOSFODNN7EXAMPLE/20130524/amazonaws/s3/aws4_request, "
		       "SignedHeaders=host;x-amz-content-sha256;x-amz-date, "
		       "Signature=c6037f4b174f2019d02d7085a611cef8adfe1efe583e220954dc85d59cd31ba3");
		ASSERT(headers["x-amz-date"] == "20130524T000000Z");
	}

	// GET with query parameters
	{
		S3BlobStoreEndpoint s3("s3.amazonaws.com", "443", "amazonaws", "proxy", "port", creds);
		std::string verb("GET");
		std::string resource("/test/examplebucket?Action=DescribeRegions&Version=2013-10-15");
		HTTP::Headers headers;
		headers["Host"] = "s3.amazonaws.com";
		s3.setV4AuthHeaders(verb, resource, headers, "20130524T000000Z", "20130524");
		ASSERT(headers["Authorization"] ==
		       "AWS4-HMAC-SHA256 Credential=AKIAIOSFODNN7EXAMPLE/20130524/amazonaws/s3/aws4_request, "
		       "SignedHeaders=host;x-amz-content-sha256;x-amz-date, "
		       "Signature=426f04e71e191fbc30096c306fe1b11ce8f026a7be374541862bbee320cce71c");
		ASSERT(headers["x-amz-date"] == "20130524T000000Z");
	}

	// POST
	{
		S3BlobStoreEndpoint s3("s3.us-west-2.amazonaws.com", "443", "us-west-2", "proxy", "port", creds);
		std::string verb("POST");
		std::string resource("/simple.json");
		HTTP::Headers headers;
		headers["Host"] = "s3.us-west-2.amazonaws.com";
		headers["Content-Type"] = "Application/x-amz-json-1.0";
		s3.setV4AuthHeaders(verb, resource, headers, "20130524T000000Z", "20130524");
		ASSERT(headers["Authorization"] ==
		       "AWS4-HMAC-SHA256 Credential=AKIAIOSFODNN7EXAMPLE/20130524/us-west-2/s3/aws4_request, "
		       "SignedHeaders=content-type;host;x-amz-content-sha256;x-amz-date, "
		       "Signature=cf095e36bed9cd3139c2e8b3e20c296a79d8540987711bf3a0d816b19ae00314");
		ASSERT(headers["x-amz-date"] == "20130524T000000Z");
		ASSERT(headers["Host"] == "s3.us-west-2.amazonaws.com");
		ASSERT(headers["Content-Type"] == "Application/x-amz-json-1.0");
	}

	return Void();
}
