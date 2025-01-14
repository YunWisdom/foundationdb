/*
 * MasterProxyServer.actor.cpp
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

#include "fdbclient/Atomic.h"
#include "fdbclient/DatabaseConfiguration.h"
#include "fdbclient/FDBTypes.h"
#include "fdbclient/KeyRangeMap.h"
#include "fdbclient/Knobs.h"
#include "fdbclient/MasterProxyInterface.h"
#include "fdbclient/NativeAPI.actor.h"
#include "fdbclient/Notified.h"
#include "fdbclient/SystemData.h"
#include "fdbrpc/sim_validation.h"
#include "fdbserver/ApplyMetadataMutation.h"
#include "fdbserver/ConflictSet.h"
#include "fdbserver/DataDistributorInterface.h"
#include "fdbserver/FDBExecHelper.actor.h"
#include "fdbserver/IKeyValueStore.h"
#include "fdbserver/Knobs.h"
#include "fdbserver/LatencyBandConfig.h"
#include "fdbserver/LogSystem.h"
#include "fdbserver/LogSystemDiskQueueAdapter.h"
#include "fdbserver/MasterInterface.h"
#include "fdbserver/RecoveryState.h"
#include "fdbserver/ServerDBInfo.h"
#include "fdbserver/WaitFailure.h"
#include "fdbserver/WorkerInterface.actor.h"
#include "flow/ActorCollection.h"
#include "flow/Knobs.h"
#include "flow/Stats.h"
#include "flow/TDMetric.actor.h"
#include "flow/actorcompiler.h"  // This must be the last #include.

struct ProxyStats {
	CounterCollection cc;
	Counter txnStartIn, txnStartOut, txnStartBatch;
	Counter txnSystemPriorityStartIn, txnSystemPriorityStartOut;
	Counter txnBatchPriorityStartIn, txnBatchPriorityStartOut;
	Counter txnDefaultPriorityStartIn, txnDefaultPriorityStartOut;
	Counter txnCommitIn, txnCommitVersionAssigned, txnCommitResolving, txnCommitResolved, txnCommitOut, txnCommitOutSuccess;
	Counter txnConflicts;
	Counter commitBatchIn, commitBatchOut;
	Counter mutationBytes;
	Counter mutations;
	Counter conflictRanges;
	Counter keyServerLocationRequests;
	Version lastCommitVersionAssigned;

	LatencyBands commitLatencyBands;
	LatencyBands grvLatencyBands;

	Future<Void> logger;

	explicit ProxyStats(UID id, Version* pVersion, NotifiedVersion* pCommittedVersion, int64_t *commitBatchesMemBytesCountPtr)
	  : cc("ProxyStats", id.toString()),
		txnStartIn("TxnStartIn", cc), txnStartOut("TxnStartOut", cc), txnStartBatch("TxnStartBatch", cc), txnSystemPriorityStartIn("TxnSystemPriorityStartIn", cc), txnSystemPriorityStartOut("TxnSystemPriorityStartOut", cc), txnBatchPriorityStartIn("TxnBatchPriorityStartIn", cc), txnBatchPriorityStartOut("TxnBatchPriorityStartOut", cc),
		txnDefaultPriorityStartIn("TxnDefaultPriorityStartIn", cc), txnDefaultPriorityStartOut("TxnDefaultPriorityStartOut", cc), txnCommitIn("TxnCommitIn", cc),	txnCommitVersionAssigned("TxnCommitVersionAssigned", cc), txnCommitResolving("TxnCommitResolving", cc), txnCommitResolved("TxnCommitResolved", cc), txnCommitOut("TxnCommitOut", cc),
		txnCommitOutSuccess("TxnCommitOutSuccess", cc), txnConflicts("TxnConflicts", cc), commitBatchIn("CommitBatchIn", cc), commitBatchOut("CommitBatchOut", cc), mutationBytes("MutationBytes", cc), mutations("Mutations", cc), conflictRanges("ConflictRanges", cc), keyServerLocationRequests("KeyServerLocationRequests", cc), 
		lastCommitVersionAssigned(0), commitLatencyBands("CommitLatencyMetrics", id, SERVER_KNOBS->STORAGE_LOGGING_DELAY), grvLatencyBands("GRVLatencyMetrics", id, SERVER_KNOBS->STORAGE_LOGGING_DELAY)
	{
		specialCounter(cc, "LastAssignedCommitVersion", [this](){return this->lastCommitVersionAssigned;});
		specialCounter(cc, "Version", [pVersion](){return *pVersion; });
		specialCounter(cc, "CommittedVersion", [pCommittedVersion](){ return pCommittedVersion->get(); });
		specialCounter(cc, "CommitBatchesMemBytesCount", [commitBatchesMemBytesCountPtr]() { return *commitBatchesMemBytesCountPtr; });
		logger = traceCounters("ProxyMetrics", id, SERVER_KNOBS->WORKER_LOGGING_INTERVAL, &cc, "ProxyMetrics");
	}
};

ACTOR Future<Void> getRate(UID myID, Reference<AsyncVar<ServerDBInfo>> db, int64_t* inTransactionCount, int64_t* inBatchTransactionCount, double* outTransactionRate,
						   double* outBatchTransactionRate, GetHealthMetricsReply* healthMetricsReply, GetHealthMetricsReply* detailedHealthMetricsReply) {
	state Future<Void> nextRequestTimer = Never();
	state Future<Void> leaseTimeout = Never();
	state Future<GetRateInfoReply> reply = Never();
	state double lastDetailedReply = 0.0; // request detailed metrics immediately
	state bool expectingDetailedReply = false;
	state int64_t lastTC = 0;

	if (db->get().ratekeeper.present()) nextRequestTimer = Void();
	loop choose {
		when ( wait( db->onChange() ) ) {
			if ( db->get().ratekeeper.present() ) {
				TraceEvent("ProxyRatekeeperChanged", myID)
				.detail("RKID", db->get().ratekeeper.get().id());
				nextRequestTimer = Void();  // trigger GetRate request
			} else {
				TraceEvent("ProxyRatekeeperDied", myID);
				nextRequestTimer = Never();
				reply = Never();
			}
		}
		when ( wait( nextRequestTimer ) ) {
			nextRequestTimer = Never();
			bool detailed = now() - lastDetailedReply > SERVER_KNOBS->DETAILED_METRIC_UPDATE_RATE;
			reply = brokenPromiseToNever(db->get().ratekeeper.get().getRateInfo.getReply(GetRateInfoRequest(myID, *inTransactionCount, *inBatchTransactionCount, detailed)));
			expectingDetailedReply = detailed;
		}
		when ( GetRateInfoReply rep = wait(reply) ) {
			reply = Never();
			*outTransactionRate = rep.transactionRate;
			*outBatchTransactionRate = rep.batchTransactionRate;
			//TraceEvent("MasterProxyRate", myID).detail("Rate", rep.transactionRate).detail("BatchRate", rep.batchTransactionRate).detail("Lease", rep.leaseDuration).detail("ReleasedTransactions", *inTransactionCount - lastTC);
			lastTC = *inTransactionCount;
			leaseTimeout = delay(rep.leaseDuration);
			nextRequestTimer = delayJittered(rep.leaseDuration / 2);
			healthMetricsReply->update(rep.healthMetrics, expectingDetailedReply, true);
			if (expectingDetailedReply) {
				detailedHealthMetricsReply->update(rep.healthMetrics, true, true);
				lastDetailedReply = now();
			}
		}
		when ( wait( leaseTimeout ) ) {
			*outTransactionRate = 0;
			*outBatchTransactionRate = 0;
			//TraceEvent("MasterProxyRate", myID).detail("Rate", 0).detail("BatchRate", 0).detail("Lease", "Expired");
			leaseTimeout = Never();
		}
	}
}

ACTOR Future<Void> queueTransactionStartRequests(
	std::priority_queue< std::pair<GetReadVersionRequest, int64_t>, std::vector< std::pair<GetReadVersionRequest, int64_t> > > *transactionQueue,
	FutureStream<GetReadVersionRequest> readVersionRequests,
	PromiseStream<Void> GRVTimer, double *lastGRVTime,
	double *GRVBatchTime, FutureStream<double> replyTimes,
	ProxyStats* stats) 
{
	state int64_t counter = 0;
	loop choose{
		when(GetReadVersionRequest req = waitNext(readVersionRequests)) {
			if (req.debugID.present())
				g_traceBatch.addEvent("TransactionDebug", req.debugID.get().first(), "MasterProxyServer.queueTransactionStartRequests.Before");

			stats->txnStartIn += req.transactionCount;
			if (req.priority() >= GetReadVersionRequest::PRIORITY_SYSTEM_IMMEDIATE)
				stats->txnSystemPriorityStartIn += req.transactionCount;
			else if (req.priority() >= GetReadVersionRequest::PRIORITY_DEFAULT)
				stats->txnDefaultPriorityStartIn += req.transactionCount;
			else
				stats->txnBatchPriorityStartIn += req.transactionCount;

			if (transactionQueue->empty()) {
				if (now() - *lastGRVTime > *GRVBatchTime)
					*lastGRVTime = now() - *GRVBatchTime;

				forwardPromise(GRVTimer, delayJittered(*GRVBatchTime - (now() - *lastGRVTime), TaskPriority::ProxyGRVTimer));
			}

			transactionQueue->push(std::make_pair(req, counter--));
		}
		// dynamic batching monitors reply latencies
		when(double reply_latency = waitNext(replyTimes)) {
			double target_latency = reply_latency * SERVER_KNOBS->START_TRANSACTION_BATCH_INTERVAL_LATENCY_FRACTION;
			*GRVBatchTime = 
				std::max(SERVER_KNOBS->START_TRANSACTION_BATCH_INTERVAL_MIN, 
					std::min(SERVER_KNOBS->START_TRANSACTION_BATCH_INTERVAL_MAX, 
						target_latency * SERVER_KNOBS->START_TRANSACTION_BATCH_INTERVAL_SMOOTHER_ALPHA + *GRVBatchTime * (1-SERVER_KNOBS->START_TRANSACTION_BATCH_INTERVAL_SMOOTHER_ALPHA)));
		}
	}
}

ACTOR void discardCommit(UID id, Future<LogSystemDiskQueueAdapter::CommitMessage> fcm, Future<Void> dummyCommitState) {
	ASSERT(!dummyCommitState.isReady());
	LogSystemDiskQueueAdapter::CommitMessage cm = wait(fcm);
	TraceEvent("Discarding", id).detail("Count", cm.messages.size());
	cm.acknowledge.send(Void());
	ASSERT(dummyCommitState.isReady());
}

DESCR struct SingleKeyMutation {
	Standalone<StringRef> shardBegin;
	Standalone<StringRef> shardEnd;
	int64_t tag1;
	int64_t tag2;
	int64_t tag3;
};

struct ProxyCommitData {
	UID dbgid;
	int64_t commitBatchesMemBytesCount;
	ProxyStats stats;
	MasterInterface master;
	vector<ResolverInterface> resolvers;
	LogSystemDiskQueueAdapter* logAdapter;
	Reference<ILogSystem> logSystem;
	IKeyValueStore* txnStateStore;
	NotifiedVersion committedVersion; // Provided that this recovery has succeeded or will succeed, this version is fully committed (durable)
	Version minKnownCommittedVersion; // No version smaller than this one will be used as the known committed version during recovery 
	Version version;  // The version at which txnStateStore is up to date
	Promise<Void> validState;  // Set once txnStateStore and version are valid
	double lastVersionTime;
	KeyRangeMap<std::set<Key>> vecBackupKeys;
	uint64_t commitVersionRequestNumber;
	uint64_t mostRecentProcessedRequestNumber;
	KeyRangeMap<Deque<std::pair<Version,int>>> keyResolvers;
	KeyRangeMap<ServerCacheInfo> keyInfo;
	std::map<Key, applyMutationsData> uid_applyMutationsData;
	bool firstProxy;
	double lastCoalesceTime;
	bool locked;
	Optional<Value> metadataVersion;
	double commitBatchInterval;

	int64_t localCommitBatchesStarted;
	NotifiedVersion latestLocalCommitBatchResolving;
	NotifiedVersion latestLocalCommitBatchLogging;

	PromiseStream<Void> commitBatchStartNotifications;
	PromiseStream<Future<GetCommitVersionReply>> commitBatchVersions;  // 1:1 with commitBatchStartNotifications
	RequestStream<GetReadVersionRequest> getConsistentReadVersion;
	RequestStream<CommitTransactionRequest> commit;
	Database cx;
	Reference<AsyncVar<ServerDBInfo>> db;
	EventMetricHandle<SingleKeyMutation> singleKeyMutationEvent;

	std::map<UID, Reference<StorageInfo>> storageCache;
	std::map<Tag, Version> tag_popped;
	Deque<std::pair<Version, Version>> txsPopVersions;
	Version lastTxsPop;
	bool popRemoteTxs;
	vector<Standalone<StringRef>> whitelistedBinPathVec;

	Optional<LatencyBandConfig> latencyBandConfig;

	//The tag related to a storage server rarely change, so we keep a vector of tags for each key range to be slightly more CPU efficient.
	//When a tag related to a storage server does change, we empty out all of these vectors to signify they must be repopulated.
	//We do not repopulate them immediately to avoid a slow task.
	const vector<Tag>& tagsForKey(StringRef key) {
		auto& tags = keyInfo[key].tags;
		if(!tags.size()) {
			auto& r = keyInfo.rangeContaining(key).value();
			for(auto info : r.src_info) {
				r.tags.push_back(info->tag);
			}
			for(auto info : r.dest_info) {
				r.tags.push_back(info->tag);
			}
			uniquify(r.tags);
			return r.tags;
		}
		return tags;
	}

	ProxyCommitData(UID dbgid, MasterInterface master, RequestStream<GetReadVersionRequest> getConsistentReadVersion, Version recoveryTransactionVersion, RequestStream<CommitTransactionRequest> commit, Reference<AsyncVar<ServerDBInfo>> db, bool firstProxy)
		: dbgid(dbgid), stats(dbgid, &version, &committedVersion, &commitBatchesMemBytesCount), master(master),
			logAdapter(NULL), txnStateStore(NULL), popRemoteTxs(false),
			committedVersion(recoveryTransactionVersion), version(0), minKnownCommittedVersion(0),
			lastVersionTime(0), commitVersionRequestNumber(1), mostRecentProcessedRequestNumber(0),
			getConsistentReadVersion(getConsistentReadVersion), commit(commit), lastCoalesceTime(0),
			localCommitBatchesStarted(0), locked(false), commitBatchInterval(SERVER_KNOBS->COMMIT_TRANSACTION_BATCH_INTERVAL_MIN),
			firstProxy(firstProxy), cx(openDBOnServer(db, TaskPriority::DefaultEndpoint, true, true)), db(db),
			singleKeyMutationEvent(LiteralStringRef("SingleKeyMutation")), commitBatchesMemBytesCount(0), lastTxsPop(0)
	{}
};

struct ResolutionRequestBuilder {
	ProxyCommitData* self;
	vector<ResolveTransactionBatchRequest> requests;
	vector<vector<int>> transactionResolverMap;
	vector<CommitTransactionRef*> outTr;

	ResolutionRequestBuilder( ProxyCommitData* self, Version version, Version prevVersion, Version lastReceivedVersion) : self(self), requests(self->resolvers.size()) {
		for(auto& req : requests) {
			req.prevVersion = prevVersion;
			req.version = version;
			req.lastReceivedVersion = lastReceivedVersion;
		}
	}

	CommitTransactionRef& getOutTransaction(int resolver, Version read_snapshot) {
		CommitTransactionRef *& out = outTr[resolver];
		if (!out) {
			ResolveTransactionBatchRequest& request = requests[resolver];
			request.transactions.resize(request.arena, request.transactions.size() + 1);
			out = &request.transactions.back();
			out->read_snapshot = read_snapshot;
		}
		return *out;
	}

	void addTransaction(CommitTransactionRef& trIn, int transactionNumberInBatch) {
		// SOMEDAY: There are a couple of unnecessary O( # resolvers ) steps here
		outTr.assign(requests.size(), NULL);
		ASSERT( transactionNumberInBatch >= 0 && transactionNumberInBatch < 32768 );

		bool isTXNStateTransaction = false;
		for (auto & m : trIn.mutations) {
			if (m.type == MutationRef::SetVersionstampedKey) {
				transformVersionstampMutation( m, &MutationRef::param1, requests[0].version, transactionNumberInBatch );
				trIn.write_conflict_ranges.push_back( requests[0].arena, singleKeyRange( m.param1, requests[0].arena ) );
			} else if (m.type == MutationRef::SetVersionstampedValue) {
				transformVersionstampMutation( m, &MutationRef::param2, requests[0].version, transactionNumberInBatch );
			}
			if (isMetadataMutation(m)) {
				isTXNStateTransaction = true;
				getOutTransaction(0, trIn.read_snapshot).mutations.push_back(requests[0].arena, m);
			}
		}
		for(auto& r : trIn.read_conflict_ranges) {
			auto ranges = self->keyResolvers.intersectingRanges( r );
			std::set<int> resolvers;
			for(auto &ir : ranges) {
				auto& version_resolver = ir.value();
				for(int i = version_resolver.size()-1; i >= 0; i--) {
					resolvers.insert(version_resolver[i].second);
					if( version_resolver[i].first < trIn.read_snapshot )
						break;
				}
			}
			ASSERT(resolvers.size());
			for(int resolver : resolvers)
				getOutTransaction( resolver, trIn.read_snapshot ).read_conflict_ranges.push_back( requests[resolver].arena, r );
		}
		for(auto& r : trIn.write_conflict_ranges) {
			auto ranges = self->keyResolvers.intersectingRanges( r );
			std::set<int> resolvers;
			for(auto &ir : ranges)
				resolvers.insert(ir.value().back().second);
			ASSERT(resolvers.size());
			for(int resolver : resolvers)
				getOutTransaction( resolver, trIn.read_snapshot ).write_conflict_ranges.push_back( requests[resolver].arena, r );
		}
		if (isTXNStateTransaction)
			for (int r = 0; r<requests.size(); r++) {
				int transactionNumberInRequest = &getOutTransaction(r, trIn.read_snapshot) - requests[r].transactions.begin();
				requests[r].txnStateTransactions.push_back(requests[r].arena, transactionNumberInRequest);
			}

		vector<int> resolversUsed;
		for (int r = 0; r<outTr.size(); r++)
			if (outTr[r])
				resolversUsed.push_back(r);
		transactionResolverMap.push_back(std::move(resolversUsed));
	}
};

ACTOR Future<Void> commitBatcher(ProxyCommitData *commitData, PromiseStream<std::pair<std::vector<CommitTransactionRequest>, int> > out, FutureStream<CommitTransactionRequest> in, int desiredBytes, int64_t memBytesLimit) {
	wait(delayJittered(commitData->commitBatchInterval, TaskPriority::ProxyCommitBatcher));  

	state double lastBatch = 0;

	loop{
		state Future<Void> timeout;
		state std::vector<CommitTransactionRequest> batch;
		state int batchBytes = 0;

		if(SERVER_KNOBS->MAX_COMMIT_BATCH_INTERVAL <= 0) {
			timeout = Never();
		}
		else {
			timeout = delayJittered(SERVER_KNOBS->MAX_COMMIT_BATCH_INTERVAL, TaskPriority::ProxyCommitBatcher);
		}

		while(!timeout.isReady() && !(batch.size() == SERVER_KNOBS->COMMIT_TRANSACTION_BATCH_COUNT_MAX || batchBytes >= desiredBytes)) {
			choose{
				when(CommitTransactionRequest req = waitNext(in)) {
					int bytes = getBytes(req);

					// Drop requests if memory is under severe pressure
					if(commitData->commitBatchesMemBytesCount + bytes > memBytesLimit) {
						req.reply.sendError(proxy_memory_limit_exceeded());
						TraceEvent(SevWarnAlways, "ProxyCommitBatchMemoryThresholdExceeded").suppressFor(60).detail("MemBytesCount", commitData->commitBatchesMemBytesCount).detail("MemLimit", memBytesLimit);
						continue;
					}

					if (bytes > FLOW_KNOBS->PACKET_WARNING) {
						TraceEvent(!g_network->isSimulated() ? SevWarnAlways : SevWarn, "LargeTransaction")
						    .suppressFor(1.0)
						    .detail("Size", bytes)
						    .detail("Client", req.reply.getEndpoint().getPrimaryAddress());
					}
					++commitData->stats.txnCommitIn;

					if(req.debugID.present()) {
						g_traceBatch.addEvent("CommitDebug", req.debugID.get().first(), "MasterProxyServer.batcher");
					}

					if(!batch.size()) {
						commitData->commitBatchStartNotifications.send(Void());
						if(now() - lastBatch > commitData->commitBatchInterval) {
							timeout = delayJittered(SERVER_KNOBS->COMMIT_TRANSACTION_BATCH_INTERVAL_FROM_IDLE, TaskPriority::ProxyCommitBatcher);
						}
						else {
							timeout = delayJittered(commitData->commitBatchInterval - (now() - lastBatch), TaskPriority::ProxyCommitBatcher);
						}
					}

					if((batchBytes + bytes > CLIENT_KNOBS->TRANSACTION_SIZE_LIMIT || req.firstInBatch()) && batch.size()) {
						out.send({ batch, batchBytes });
						lastBatch = now();
						commitData->commitBatchStartNotifications.send(Void());
						timeout = delayJittered(commitData->commitBatchInterval, TaskPriority::ProxyCommitBatcher);
						batch = std::vector<CommitTransactionRequest>();
						batchBytes = 0;
					}

					batch.push_back(req);
					batchBytes += bytes;
					commitData->commitBatchesMemBytesCount += bytes;
				}
				when(wait(timeout)) {}
			}
		}
		out.send({ std::move(batch), batchBytes });
		lastBatch = now();
	}
}

void createWhitelistBinPathVec(const std::string& binPath, vector<Standalone<StringRef>>& binPathVec) {
	TraceEvent(SevDebug, "BinPathConverter").detail("Input", binPath);
	StringRef input(binPath);
	while (input != StringRef()) {
		StringRef token = input.eat(LiteralStringRef(","));
		if (token != StringRef()) {
			const uint8_t* ptr = token.begin();
			while (ptr != token.end() && *ptr == ' ') {
				ptr++;
			}
			if (ptr != token.end()) {
				Standalone<StringRef> newElement(token.substr(ptr - token.begin()));
				TraceEvent(SevDebug, "BinPathItem").detail("Element", newElement);
				binPathVec.push_back(newElement);
			}
		}
	}
	return;
}

bool isWhitelisted(const vector<Standalone<StringRef>>& binPathVec, StringRef binPath) {
	TraceEvent("BinPath").detail("Value", binPath);
	for (const auto& item : binPathVec) {
		TraceEvent("Element").detail("Value", item);
	}
	return std::find(binPathVec.begin(), binPathVec.end(), binPath) != binPathVec.end();
}

ACTOR Future<Void> commitBatch(
	ProxyCommitData* self,
	vector<CommitTransactionRequest> trs,
	int currentBatchMemBytesCount)
{
	state int64_t localBatchNumber = ++self->localCommitBatchesStarted;
	state LogPushData toCommit(self->logSystem);
	state double t1 = now();
	state Optional<UID> debugID;
	state bool forceRecovery = false;

	ASSERT(SERVER_KNOBS->MAX_READ_TRANSACTION_LIFE_VERSIONS <= SERVER_KNOBS->MAX_VERSIONS_IN_FLIGHT);  // since we are using just the former to limit the number of versions actually in flight!

	// Active load balancing runs at a very high priority (to obtain accurate estimate of memory used by commit batches) so we need to downgrade here
	wait(delay(0, TaskPriority::ProxyCommit));

	self->lastVersionTime = t1;

	++self->stats.commitBatchIn;

	for (int t = 0; t<trs.size(); t++) {
		if (trs[t].debugID.present()) {
			if (!debugID.present())
				debugID = nondeterministicRandom()->randomUniqueID();
			g_traceBatch.addAttach("CommitAttachID", trs[t].debugID.get().first(), debugID.get().first());
		}
	}

	if(localBatchNumber == 2 && !debugID.present() && self->firstProxy && !g_network->isSimulated()) {
		debugID = deterministicRandom()->randomUniqueID();
		TraceEvent("SecondCommitBatch", self->dbgid).detail("DebugID", debugID.get());
	}

	if (debugID.present())
		g_traceBatch.addEvent("CommitDebug", debugID.get().first(), "MasterProxyServer.commitBatch.Before");

	if (trs.empty()) {
		// We are sending an empty batch, so we have to trigger the version fetcher
		self->commitBatchStartNotifications.send(Void());
	}

	/////// Phase 1: Pre-resolution processing (CPU bound except waiting for a version # which is separately pipelined and *should* be available by now (unless empty commit); ordered; currently atomic but could yield)
	TEST(self->latestLocalCommitBatchResolving.get() < localBatchNumber-1); // Queuing pre-resolution commit processing 
	wait(self->latestLocalCommitBatchResolving.whenAtLeast(localBatchNumber-1));
	wait(yield());

	if (debugID.present())
		g_traceBatch.addEvent("CommitDebug", debugID.get().first(), "MasterProxyServer.commitBatch.GettingCommitVersion");

	Future<GetCommitVersionReply> fVersionReply = waitNext(self->commitBatchVersions.getFuture());
	GetCommitVersionReply versionReply = wait(fVersionReply);
	self->mostRecentProcessedRequestNumber = versionReply.requestNum;

	self->stats.txnCommitVersionAssigned += trs.size();
	self->stats.lastCommitVersionAssigned = versionReply.version;

	state Version commitVersion = versionReply.version;
	state Version prevVersion = versionReply.prevVersion;

	for(auto it : versionReply.resolverChanges) {
		auto rs = self->keyResolvers.modify(it.range);
		for(auto r = rs.begin(); r != rs.end(); ++r)
			r->value().emplace_back(versionReply.resolverChangesVersion,it.dest);
	}

	//TraceEvent("ProxyGotVer", self->dbgid).detail("Commit", commitVersion).detail("Prev", prevVersion);

	if (debugID.present())
		g_traceBatch.addEvent("CommitDebug", debugID.get().first(), "MasterProxyServer.commitBatch.GotCommitVersion");

	ResolutionRequestBuilder requests( self, commitVersion, prevVersion, self->version );
	int conflictRangeCount = 0;
	state int64_t maxTransactionBytes = 0;
	for (int t = 0; t<trs.size(); t++) {
		requests.addTransaction(trs[t].transaction, t);
		conflictRangeCount += trs[t].transaction.read_conflict_ranges.size() + trs[t].transaction.write_conflict_ranges.size();
		//TraceEvent("MPTransactionDump", self->dbgid).detail("Snapshot", trs[t].transaction.read_snapshot);
		//for(auto& m : trs[t].transaction.mutations)
		maxTransactionBytes = std::max<int64_t>(maxTransactionBytes, trs[t].transaction.expectedSize());
		//	TraceEvent("MPTransactionsDump", self->dbgid).detail("Mutation", m.toString());
	}
	self->stats.conflictRanges += conflictRangeCount;

	for (int r = 1; r<self->resolvers.size(); r++)
		ASSERT(requests.requests[r].txnStateTransactions.size() == requests.requests[0].txnStateTransactions.size());

	// Sending these requests is the fuzzy border between phase 1 and phase 2; it could conceivably overlap with resolution processing but is still using CPU
	self->stats.txnCommitResolving += trs.size();
	vector< Future<ResolveTransactionBatchReply> > replies;
	for (int r = 0; r<self->resolvers.size(); r++) {
		requests.requests[r].debugID = debugID;
		replies.push_back(brokenPromiseToNever(self->resolvers[r].resolve.getReply(requests.requests[r], TaskPriority::ProxyResolverReply)));
	}

	state vector<vector<int>> transactionResolverMap = std::move( requests.transactionResolverMap );

	ASSERT(self->latestLocalCommitBatchResolving.get() == localBatchNumber-1);
	self->latestLocalCommitBatchResolving.set(localBatchNumber);

	/////// Phase 2: Resolution (waiting on the network; pipelined)
	state vector<ResolveTransactionBatchReply> resolution = wait( getAll(replies) );

	if (debugID.present())
		g_traceBatch.addEvent("CommitDebug", debugID.get().first(), "MasterProxyServer.commitBatch.AfterResolution");

	////// Phase 3: Post-resolution processing (CPU bound except for very rare situations; ordered; currently atomic but doesn't need to be)
	TEST(self->latestLocalCommitBatchLogging.get() < localBatchNumber-1); // Queuing post-resolution commit processing 
	wait(self->latestLocalCommitBatchLogging.whenAtLeast(localBatchNumber-1));
	wait(yield());

	self->stats.txnCommitResolved += trs.size();

	if (debugID.present())
		g_traceBatch.addEvent("CommitDebug", debugID.get().first(), "MasterProxyServer.commitBatch.ProcessingMutations");

	state Arena arena;
	state bool isMyFirstBatch = !self->version;
	state Optional<Value> oldCoordinators = self->txnStateStore->readValue(coordinatorsKey).get();

	//TraceEvent("ResolutionResult", self->dbgid).detail("Sequence", sequence).detail("Version", commitVersion).detail("StateMutationProxies", resolution[0].stateMutations.size()).detail("WaitForResolution", now()-t1).detail("R0Committed", resolution[0].committed.size())
	//	.detail("Transactions", trs.size());

	for(int r=1; r<resolution.size(); r++) {
		ASSERT( resolution[r].stateMutations.size() == resolution[0].stateMutations.size() );
		for(int s=0; s<resolution[r].stateMutations.size(); s++)
			ASSERT( resolution[r].stateMutations[s].size() == resolution[0].stateMutations[s].size() );
	}

	// Compute and apply "metadata" effects of each other proxy's most recent batch
	bool initialState = isMyFirstBatch;
	state bool firstStateMutations = isMyFirstBatch;
	state vector< std::pair<Future<LogSystemDiskQueueAdapter::CommitMessage>, Future<Void>> > storeCommits;
	for (int versionIndex = 0; versionIndex < resolution[0].stateMutations.size(); versionIndex++) {
		// self->logAdapter->setNextVersion( ??? );  << Ideally we would be telling the log adapter that the pushes in this commit will be in the version at which these state mutations were committed by another proxy, but at present we don't have that information here.  So the disk queue may be unnecessarily conservative about popping.

		for (int transactionIndex = 0; transactionIndex < resolution[0].stateMutations[versionIndex].size() && !forceRecovery; transactionIndex++) {
			bool committed = true;
			for (int resolver = 0; resolver < resolution.size(); resolver++)
				committed = committed && resolution[resolver].stateMutations[versionIndex][transactionIndex].committed;
			if (committed)
				applyMetadataMutations( self->dbgid, arena, resolution[0].stateMutations[versionIndex][transactionIndex].mutations, self->txnStateStore, NULL, &forceRecovery, self->logSystem, 0, &self->vecBackupKeys, &self->keyInfo, self->firstProxy ? &self->uid_applyMutationsData : NULL, self->commit, self->cx, &self->committedVersion, &self->storageCache, &self->tag_popped);
			
			if( resolution[0].stateMutations[versionIndex][transactionIndex].mutations.size() && firstStateMutations ) {
				ASSERT(committed);
				firstStateMutations = false;
				forceRecovery = false;
			}
			//TraceEvent("MetadataTransaction", self->dbgid).detail("Committed", committed).detail("Mutations", resolution[0].stateMutations[versionIndex][transactionIndex].second.size()).detail("R1Mutations", resolution.back().stateMutations[versionIndex][transactionIndex].second.size());
		}
		//TraceEvent("MetadataBatch", self->dbgid).detail("Transactions", resolution[0].stateMutations[versionIndex].size());

		// These changes to txnStateStore will be committed by the other proxy, so we simply discard the commit message
		auto fcm = self->logAdapter->getCommitMessage();
		storeCommits.emplace_back(fcm, self->txnStateStore->commit());
		//discardCommit( dbgid, fcm, txnStateStore->commit() );

		if (initialState) {
			//TraceEvent("ResyncLog", dbgid);
			initialState = false;
			forceRecovery = false;
			self->txnStateStore->resyncLog();

			for (auto &p : storeCommits) {
				ASSERT(!p.second.isReady());
				p.first.get().acknowledge.send(Void());
				ASSERT(p.second.isReady());
			}
			storeCommits.clear();
		}
	}

	// Determine which transactions actually committed (conservatively) by combining results from the resolvers
	state vector<uint8_t> committed(trs.size());
	ASSERT(transactionResolverMap.size() == committed.size());
	vector<int> nextTr(resolution.size());
	for (int t = 0; t<trs.size(); t++) {
		uint8_t commit = ConflictBatch::TransactionCommitted;
		for (int r : transactionResolverMap[t])
		{
			commit = std::min(resolution[r].committed[nextTr[r]++], commit);
		}
		committed[t] = commit;
	}
	for (int r = 0; r<resolution.size(); r++)
		ASSERT(nextTr[r] == resolution[r].committed.size());

	self->logAdapter->setNextVersion(commitVersion);

	state Optional<Key> lockedKey = self->txnStateStore->readValue(databaseLockedKey).get();
	state bool locked = lockedKey.present() && lockedKey.get().size();

	state Optional<Key> mustContainSystemKey = self->txnStateStore->readValue(mustContainSystemMutationsKey).get();
	if(mustContainSystemKey.present() && mustContainSystemKey.get().size()) {
		for (int t = 0; t<trs.size(); t++) {
			if( committed[t] == ConflictBatch::TransactionCommitted ) {
				bool foundSystem = false;
				for(auto& m : trs[t].transaction.mutations) {
					if( ( m.type == MutationRef::ClearRange ? m.param2 : m.param1 ) >= nonMetadataSystemKeys.end) {
						foundSystem = true;
						break;
					}
				}
				if(!foundSystem) {
					committed[t] = ConflictBatch::TransactionConflict;
				}
			}
		}
	}

	if(forceRecovery) {
		wait( Future<Void>(Never()) );
	}

	// This first pass through committed transactions deals with "metadata" effects (modifications of txnStateStore, changes to storage servers' responsibilities)
	int t;
	state int commitCount = 0;
	for (t = 0; t < trs.size() && !forceRecovery; t++)
	{
		if (committed[t] == ConflictBatch::TransactionCommitted && (!locked || trs[t].isLockAware())) {
			commitCount++;
			applyMetadataMutations(self->dbgid, arena, trs[t].transaction.mutations, self->txnStateStore, &toCommit, &forceRecovery, self->logSystem, commitVersion+1, &self->vecBackupKeys, &self->keyInfo, self->firstProxy ? &self->uid_applyMutationsData : NULL, self->commit, self->cx, &self->committedVersion, &self->storageCache, &self->tag_popped);
		}
		if(firstStateMutations) {
			ASSERT(committed[t] == ConflictBatch::TransactionCommitted);
			firstStateMutations = false;
			forceRecovery = false;
		}
	}
	if (forceRecovery) {
		for (; t<trs.size(); t++)
			committed[t] = ConflictBatch::TransactionConflict;
		TraceEvent(SevWarn, "RestartingTxnSubsystem", self->dbgid).detail("Stage", "AwaitCommit");
	}

	lockedKey = self->txnStateStore->readValue(databaseLockedKey).get();
	state bool lockedAfter = lockedKey.present() && lockedKey.get().size();

	state Optional<Value> metadataVersionAfter = self->txnStateStore->readValue(metadataVersionKey).get();

	auto fcm = self->logAdapter->getCommitMessage();
	storeCommits.emplace_back(fcm, self->txnStateStore->commit());
	self->version = commitVersion;
	if (!self->validState.isSet()) self->validState.send(Void());
	ASSERT(commitVersion);

	if (!isMyFirstBatch && self->txnStateStore->readValue( coordinatorsKey ).get().get() != oldCoordinators.get()) {
		wait( brokenPromiseToNever( self->master.changeCoordinators.getReply( ChangeCoordinatorsRequest( self->txnStateStore->readValue( coordinatorsKey ).get().get() ) ) ) );
		ASSERT(false);   // ChangeCoordinatorsRequest should always throw
	}

	// This second pass through committed transactions assigns the actual mutations to the appropriate storage servers' tags
	state int mutationCount = 0;
	state int mutationBytes = 0;
	
	state std::map<Key, MutationListRef> logRangeMutations;
	state Arena logRangeMutationsArena;
	state uint32_t v = commitVersion / CLIENT_KNOBS->LOG_RANGE_BLOCK_SIZE;
	state int transactionNum = 0;
	state int yieldBytes = 0;

	for (; transactionNum<trs.size(); transactionNum++) {
		if (committed[transactionNum] == ConflictBatch::TransactionCommitted && (!locked || trs[transactionNum].isLockAware())) {
			state int mutationNum = 0;
			state VectorRef<MutationRef>* pMutations = &trs[transactionNum].transaction.mutations;
			for (; mutationNum < pMutations->size(); mutationNum++) {
				if(yieldBytes > SERVER_KNOBS->DESIRED_TOTAL_BYTES) {
					yieldBytes = 0;
					wait(yield());
				}

				auto& m = (*pMutations)[mutationNum];
				mutationCount++;
				mutationBytes += m.expectedSize();
				yieldBytes += m.expectedSize();
				// Determine the set of tags (responsible storage servers) for the mutation, splitting it
				// if necessary.  Serialize (splits of) the mutation into the message buffer and add the tags.

				if (isSingleKeyMutation((MutationRef::Type) m.type)) {
					auto& tags = self->tagsForKey(m.param1);
	
					if(self->singleKeyMutationEvent->enabled) {
						KeyRangeRef shard = self->keyInfo.rangeContaining(m.param1).range();
						self->singleKeyMutationEvent->tag1 = (int64_t)tags[0].id;
						self->singleKeyMutationEvent->tag2 = (int64_t)tags[1].id;
						self->singleKeyMutationEvent->tag3 = (int64_t)tags[2].id;
						self->singleKeyMutationEvent->shardBegin = shard.begin;
						self->singleKeyMutationEvent->shardEnd = shard.end;
						self->singleKeyMutationEvent->log();
					}

					if (debugMutation("ProxyCommit", commitVersion, m))
						TraceEvent("ProxyCommitTo", self->dbgid).detail("To", describe(tags)).detail("Mutation", m.toString()).detail("Version", commitVersion);
					toCommit.addTags(tags);
					toCommit.addTypedMessage(m);
				}
				else if (m.type == MutationRef::ClearRange) {
					auto ranges = self->keyInfo.intersectingRanges(KeyRangeRef(m.param1, m.param2));
					auto firstRange = ranges.begin();
					++firstRange;
					if (firstRange == ranges.end()) {
						// Fast path
						if (debugMutation("ProxyCommit", commitVersion, m))
							TraceEvent("ProxyCommitTo", self->dbgid).detail("To", describe(ranges.begin().value().tags)).detail("Mutation", m.toString()).detail("Version", commitVersion);

						ranges.begin().value().populateTags();
						toCommit.addTags(ranges.begin().value().tags);
					}
					else {
						TEST(true); //A clear range extends past a shard boundary
						std::set<Tag> allSources;
						for (auto r : ranges) {
							r.value().populateTags();
							allSources.insert(r.value().tags.begin(), r.value().tags.end());
						}
						if (debugMutation("ProxyCommit", commitVersion, m))
							TraceEvent("ProxyCommitTo", self->dbgid).detail("To", describe(allSources)).detail("Mutation", m.toString()).detail("Version", commitVersion);
						toCommit.addTags(allSources);
					}
					toCommit.addTypedMessage(m);
				} else
					UNREACHABLE();


				// Check on backing up key, if backup ranges are defined and a normal key
				if (self->vecBackupKeys.size() > 1 && (normalKeys.contains(m.param1) || m.param1 == metadataVersionKey)) {
					if (m.type != MutationRef::Type::ClearRange) {
						// Add the mutation to the relevant backup tag
						for (auto backupName : self->vecBackupKeys[m.param1]) {
							logRangeMutations[backupName].push_back_deep(logRangeMutationsArena, m);
						}
					}
					else {
						KeyRangeRef mutationRange(m.param1, m.param2);
						KeyRangeRef intersectionRange;

						// Identify and add the intersecting ranges of the mutation to the array of mutations to serialize
						for (auto backupRange : self->vecBackupKeys.intersectingRanges(mutationRange))
						{
							// Get the backup sub range
							const auto&		backupSubrange = backupRange.range();

							// Determine the intersecting range
							intersectionRange = mutationRange & backupSubrange;

							// Create the custom mutation for the specific backup tag
							MutationRef		backupMutation(MutationRef::Type::ClearRange, intersectionRange.begin, intersectionRange.end);

							// Add the mutation to the relevant backup tag
							for (auto backupName : backupRange.value()) {
								logRangeMutations[backupName].push_back_deep(logRangeMutationsArena, backupMutation);
							}
						}
					}
				}
			}
		}
	}

	// Serialize and backup the mutations as a single mutation
	if ((self->vecBackupKeys.size() > 1) && logRangeMutations.size()) {

		Key			val;
		MutationRef backupMutation;
		uint32_t*	partBuffer = NULL;

		// Serialize the log range mutations within the map
		for (auto& logRangeMutation : logRangeMutations)
		{
			BinaryWriter wr(Unversioned());

			// Serialize the log destination
			wr.serializeBytes( logRangeMutation.first );

			// Write the log keys and version information
			wr << (uint8_t)hashlittle(&v, sizeof(v), 0);
			wr << bigEndian64(commitVersion);

			backupMutation.type = MutationRef::SetValue;
			partBuffer = NULL;

			val = BinaryWriter::toValue(logRangeMutation.second, IncludeVersion());

			for (int part = 0; part * CLIENT_KNOBS->MUTATION_BLOCK_SIZE < val.size(); part++) {

				// Assign the second parameter as the part
				backupMutation.param2 = val.substr(part * CLIENT_KNOBS->MUTATION_BLOCK_SIZE,
					std::min(val.size() - part * CLIENT_KNOBS->MUTATION_BLOCK_SIZE, CLIENT_KNOBS->MUTATION_BLOCK_SIZE));

				// Write the last part of the mutation to the serialization, if the buffer is not defined
				if (!partBuffer) {
					// Serialize the part to the writer
					wr << bigEndian32(part);

					// Define the last buffer part
					partBuffer = (uint32_t*) ((char*) wr.getData() + wr.getLength() - sizeof(uint32_t));
				}
				else {
					*partBuffer = bigEndian32(part);
				}

				// Define the mutation type and and location
				backupMutation.param1 = wr.toValue();
				ASSERT( backupMutation.param1.startsWith(logRangeMutation.first) );  // We are writing into the configured destination
					
				auto& tags = self->tagsForKey(backupMutation.param1);
				toCommit.addTags(tags);
				toCommit.addTypedMessage(backupMutation);

//				if (debugMutation("BackupProxyCommit", commitVersion, backupMutation)) {
//					TraceEvent("BackupProxyCommitTo", self->dbgid).detail("To", describe(tags)).detail("BackupMutation", backupMutation.toString())
//						.detail("BackupMutationSize", val.size()).detail("Version", commitVersion).detail("DestPath", logRangeMutation.first)
//						.detail("PartIndex", part).detail("PartIndexEndian", bigEndian32(part)).detail("PartData", backupMutation.param1);
//				}
			}
		}
	}

	self->stats.mutations += mutationCount;
	self->stats.mutationBytes += mutationBytes;

	// Storage servers mustn't make durable versions which are not fully committed (because then they are impossible to roll back)
	// We prevent this by limiting the number of versions which are semi-committed but not fully committed to be less than the MVCC window
	while (self->committedVersion.get() < commitVersion - SERVER_KNOBS->MAX_READ_TRANSACTION_LIFE_VERSIONS) {
		// This should be *extremely* rare in the real world, but knob buggification should make it happen in simulation
		TEST(true);  // Semi-committed pipeline limited by MVCC window
		//TraceEvent("ProxyWaitingForCommitted", self->dbgid).detail("CommittedVersion", self->committedVersion.get()).detail("NeedToCommit", commitVersion);
		choose{
			when(wait(self->committedVersion.whenAtLeast(commitVersion - SERVER_KNOBS->MAX_READ_TRANSACTION_LIFE_VERSIONS))) {
				wait(yield());
				break; 
			}
			when(GetReadVersionReply v = wait(self->getConsistentReadVersion.getReply(GetReadVersionRequest(0, GetReadVersionRequest::PRIORITY_SYSTEM_IMMEDIATE | GetReadVersionRequest::FLAG_CAUSAL_READ_RISKY)))) {
				if(!v.newClientInfo.present() && v.version > self->committedVersion.get()) {
					self->locked = v.locked;
					self->metadataVersion = v.metadataVersion;
					self->committedVersion.set(v.version);
				}
				
				if (self->committedVersion.get() < commitVersion - SERVER_KNOBS->MAX_READ_TRANSACTION_LIFE_VERSIONS)
					wait(delay(SERVER_KNOBS->PROXY_SPIN_DELAY));
			}
		}
	}

	state LogSystemDiskQueueAdapter::CommitMessage msg = wait(storeCommits.back().first); // Should just be doing yields

	if (debugID.present())
		g_traceBatch.addEvent("CommitDebug", debugID.get().first(), "MasterProxyServer.commitBatch.AfterStoreCommits");

	// txnState (transaction subsystem state) tag: message extracted from log adapter
	bool firstMessage = true;
	for(auto m : msg.messages) {
		if(firstMessage) {
			toCommit.addTxsTag();
		}
		toCommit.addMessage(StringRef(m.begin(), m.size()), !firstMessage);
		firstMessage = false;
	}

	if ( prevVersion && commitVersion - prevVersion < SERVER_KNOBS->MAX_VERSIONS_IN_FLIGHT/2 )
		debug_advanceMaxCommittedVersion( UID(), commitVersion );  //< Is this valid?

	//TraceEvent("ProxyPush", self->dbgid).detail("PrevVersion", prevVersion).detail("Version", commitVersion)
	//	.detail("TransactionsSubmitted", trs.size()).detail("TransactionsCommitted", commitCount).detail("TxsPopTo", msg.popTo);

	if ( prevVersion && commitVersion - prevVersion < SERVER_KNOBS->MAX_VERSIONS_IN_FLIGHT/2 )
		debug_advanceMaxCommittedVersion(UID(), commitVersion);

	Future<Version> loggingComplete = self->logSystem->push( prevVersion, commitVersion, self->committedVersion.get(), self->minKnownCommittedVersion, toCommit, debugID );

	if (!forceRecovery) {
		ASSERT(self->latestLocalCommitBatchLogging.get() == localBatchNumber-1);
		self->latestLocalCommitBatchLogging.set(localBatchNumber);
	}

	/////// Phase 4: Logging (network bound; pipelined up to MAX_READ_TRANSACTION_LIFE_VERSIONS (limited by loop above))

	try {
		choose {
			when(Version ver = wait(loggingComplete)) {
				self->minKnownCommittedVersion = std::max(self->minKnownCommittedVersion, ver);
			}
			when(wait(self->committedVersion.whenAtLeast( commitVersion+1 ))) {}
		}
	} catch(Error &e) {
		if(e.code() == error_code_broken_promise) {
			throw master_tlog_failed();
		}
		throw;
	}
	wait(yield());

	if( self->popRemoteTxs && msg.popTo > ( self->txsPopVersions.size() ? self->txsPopVersions.back().second : self->lastTxsPop ) ) {
		if(self->txsPopVersions.size() >= SERVER_KNOBS->MAX_TXS_POP_VERSION_HISTORY) {
			TraceEvent(SevWarnAlways, "DiscardingTxsPopHistory").suppressFor(1.0);
			self->txsPopVersions.pop_front();
		}

		self->txsPopVersions.emplace_back(commitVersion, msg.popTo);
	}
	self->logSystem->popTxs(msg.popTo);

	/////// Phase 5: Replies (CPU bound; no particular order required, though ordered execution would be best for latency)
	if ( prevVersion && commitVersion - prevVersion < SERVER_KNOBS->MAX_VERSIONS_IN_FLIGHT/2 )
		debug_advanceMinCommittedVersion(UID(), commitVersion);

	//TraceEvent("ProxyPushed", self->dbgid).detail("PrevVersion", prevVersion).detail("Version", commitVersion);
	if (debugID.present())
		g_traceBatch.addEvent("CommitDebug", debugID.get().first(), "MasterProxyServer.commitBatch.AfterLogPush");

	for (auto &p : storeCommits) {
		ASSERT(!p.second.isReady());
		p.first.get().acknowledge.send(Void());
		ASSERT(p.second.isReady());
	}

	TEST(self->committedVersion.get() > commitVersion);   // A later version was reported committed first
	if( commitVersion > self->committedVersion.get() ) {
		self->locked = lockedAfter;
		self->metadataVersion = metadataVersionAfter;
		self->committedVersion.set(commitVersion);
	}

	if (forceRecovery) {
		TraceEvent(SevWarn, "RestartingTxnSubsystem", self->dbgid).detail("Stage", "ProxyShutdown");
		throw worker_removed();
	}

	// Send replies to clients
	double endTime = timer();
	for (int t = 0; t < trs.size(); t++) {
		if (committed[t] == ConflictBatch::TransactionCommitted && (!locked || trs[t].isLockAware())) {
			ASSERT_WE_THINK(commitVersion != invalidVersion);
			trs[t].reply.send(CommitID(commitVersion, t, metadataVersionAfter));
		}
		else if (committed[t] == ConflictBatch::TransactionTooOld) {
			trs[t].reply.sendError(transaction_too_old());
		}
		else {
			trs[t].reply.sendError(not_committed());
		}

		// TODO: filter if pipelined with large commit
		if(self->latencyBandConfig.present()) {
			bool filter = maxTransactionBytes > self->latencyBandConfig.get().commitConfig.maxCommitBytes.orDefault(std::numeric_limits<int>::max());
			self->stats.commitLatencyBands.addMeasurement(endTime - trs[t].requestTime, filter);
		}
	}

	++self->stats.commitBatchOut;
	self->stats.txnCommitOut += trs.size();
	self->stats.txnConflicts += trs.size() - commitCount;
	self->stats.txnCommitOutSuccess += commitCount;

	if(now() - self->lastCoalesceTime > SERVER_KNOBS->RESOLVER_COALESCE_TIME) {
		self->lastCoalesceTime = now();
		int lastSize = self->keyResolvers.size();
		auto rs = self->keyResolvers.ranges();
		Version oldestVersion = prevVersion - SERVER_KNOBS->MAX_WRITE_TRANSACTION_LIFE_VERSIONS;
		for(auto r = rs.begin(); r != rs.end(); ++r) {
			while(r->value().size() > 1 && r->value()[1].first < oldestVersion)
				r->value().pop_front();
			if(r->value().size() && r->value().front().first < oldestVersion)
				r->value().front().first = 0;
		}
		self->keyResolvers.coalesce(allKeys);
		if(self->keyResolvers.size() != lastSize)
			TraceEvent("KeyResolverSize", self->dbgid).detail("Size", self->keyResolvers.size());
	}

	// Dynamic batching for commits
	double target_latency = (now() - t1) * SERVER_KNOBS->COMMIT_TRANSACTION_BATCH_INTERVAL_LATENCY_FRACTION;
	self->commitBatchInterval = 
		std::max(SERVER_KNOBS->COMMIT_TRANSACTION_BATCH_INTERVAL_MIN, 
			std::min(SERVER_KNOBS->COMMIT_TRANSACTION_BATCH_INTERVAL_MAX, 
				target_latency * SERVER_KNOBS->COMMIT_TRANSACTION_BATCH_INTERVAL_SMOOTHER_ALPHA + self->commitBatchInterval * (1-SERVER_KNOBS->COMMIT_TRANSACTION_BATCH_INTERVAL_SMOOTHER_ALPHA)));


	self->commitBatchesMemBytesCount -= currentBatchMemBytesCount;
	ASSERT_ABORT(self->commitBatchesMemBytesCount >= 0);
	return Void();
}


ACTOR Future<GetReadVersionReply> getLiveCommittedVersion(ProxyCommitData* commitData, uint32_t flags, vector<MasterProxyInterface> *otherProxies, Optional<UID> debugID, int transactionCount, int systemTransactionCount, int defaultPriTransactionCount, int batchPriTransactionCount)
{
	// Returns a version which (1) is committed, and (2) is >= the latest version reported committed (by a commit response) when this request was sent
	// (1) The version returned is the committedVersion of some proxy at some point before the request returns, so it is committed.
	// (2) No proxy on our list reported committed a higher version before this request was received, because then its committedVersion would have been higher,
	//     and no other proxy could have already committed anything without first ending the epoch
	++commitData->stats.txnStartBatch;

	state vector<Future<GetReadVersionReply>> proxyVersions;
	for (auto const& p : *otherProxies)
		proxyVersions.push_back(brokenPromiseToNever(p.getRawCommittedVersion.getReply(GetRawCommittedVersionRequest(debugID), TaskPriority::TLogConfirmRunningReply)));

	if (!(flags&GetReadVersionRequest::FLAG_CAUSAL_READ_RISKY))
	{
		wait(commitData->logSystem->confirmEpochLive(debugID));
	}

	if (debugID.present())
		g_traceBatch.addEvent("TransactionDebug", debugID.get().first(), "MasterProxyServer.getLiveCommittedVersion.confirmEpochLive");

	vector<GetReadVersionReply> versions = wait(getAll(proxyVersions));
	GetReadVersionReply rep;
	rep.version = commitData->committedVersion.get();
	rep.locked = commitData->locked;
	rep.metadataVersion = commitData->metadataVersion;

	for (auto v : versions) {
		if(v.version > rep.version) {
			rep = v;
		}
	}

	if (debugID.present())
		g_traceBatch.addEvent("TransactionDebug", debugID.get().first(), "MasterProxyServer.getLiveCommittedVersion.After");

	commitData->stats.txnStartOut += transactionCount;
	commitData->stats.txnSystemPriorityStartOut += systemTransactionCount;
	commitData->stats.txnDefaultPriorityStartOut += defaultPriTransactionCount;
	commitData->stats.txnBatchPriorityStartOut += batchPriTransactionCount;

	return rep;
}

ACTOR Future<Void> fetchVersions(ProxyCommitData *commitData) {
	loop {
		waitNext(commitData->commitBatchStartNotifications.getFuture());
		GetCommitVersionRequest req(commitData->commitVersionRequestNumber++, commitData->mostRecentProcessedRequestNumber, commitData->dbgid);
		commitData->commitBatchVersions.send(brokenPromiseToNever(commitData->master.getCommitVersion.getReply(req)));
	}
}

struct TransactionRateInfo {
	double rate;
	double limit;

	TransactionRateInfo(double rate) : rate(rate), limit(0) {}

	void reset(double elapsed) {
		limit = std::min(0.0,limit) + std::min(rate * elapsed, SERVER_KNOBS->START_TRANSACTION_MAX_TRANSACTIONS_TO_START);
	}

	bool canStart(int64_t numAlreadyStarted) {
		return numAlreadyStarted < limit;
	}

	void updateBudget(int64_t numStarted) {
		limit -= numStarted;
	}
};

ACTOR Future<Void> sendGrvReplies(Future<GetReadVersionReply> replyFuture, std::vector<GetReadVersionRequest> requests, ProxyStats *stats) {
	GetReadVersionReply reply = wait(replyFuture);
	double end = timer();
	for(GetReadVersionRequest const& request : requests) {
		stats->grvLatencyBands.addMeasurement(end - request.requestTime);
		request.reply.send(reply);
	}

	return Void();
}

ACTOR static Future<Void> transactionStarter(
	MasterProxyInterface proxy,
	Reference<AsyncVar<ServerDBInfo>> db,
	PromiseStream<Future<Void>> addActor,
	ProxyCommitData* commitData, GetHealthMetricsReply* healthMetricsReply,
	GetHealthMetricsReply* detailedHealthMetricsReply)
{
	state double lastGRVTime = 0;
	state PromiseStream<Void> GRVTimer;
	state double GRVBatchTime = SERVER_KNOBS->START_TRANSACTION_BATCH_INTERVAL_MIN;

	state int64_t transactionCount = 0;
	state int64_t batchTransactionCount = 0;
	state TransactionRateInfo normalRateInfo(10);
	state TransactionRateInfo batchRateInfo(0);

	state std::priority_queue<std::pair<GetReadVersionRequest, int64_t>, std::vector<std::pair<GetReadVersionRequest, int64_t>>> transactionQueue;
	state vector<MasterProxyInterface> otherProxies;

	state PromiseStream<double> replyTimes;
	addActor.send(getRate(proxy.id(), db, &transactionCount, &batchTransactionCount, &normalRateInfo.rate, &batchRateInfo.rate, healthMetricsReply, detailedHealthMetricsReply));
	addActor.send(queueTransactionStartRequests(&transactionQueue, proxy.getConsistentReadVersion.getFuture(), GRVTimer, &lastGRVTime, &GRVBatchTime, replyTimes.getFuture(), &commitData->stats));

	// Get a list of the other proxies that go together with us
	while (std::find(db->get().client.proxies.begin(), db->get().client.proxies.end(), proxy) == db->get().client.proxies.end())
		wait(db->onChange());
	for (MasterProxyInterface mp : db->get().client.proxies) {
		if (mp != proxy)
			otherProxies.push_back(mp);
	}

	ASSERT(db->get().recoveryState >= RecoveryState::ACCEPTING_COMMITS);  // else potentially we could return uncommitted read versions (since self->committedVersion is only a committed version if this recovery succeeds)

	TraceEvent("ProxyReadyForTxnStarts", proxy.id());

	loop{
		waitNext(GRVTimer.getFuture());
		// Select zero or more transactions to start
		double t = now();
		double elapsed = std::min<double>(now() - lastGRVTime, SERVER_KNOBS->START_TRANSACTION_BATCH_INTERVAL_MAX);
		lastGRVTime = t;

		if(elapsed == 0) elapsed = 1e-15; // resolve a possible indeterminant multiplication with infinite transaction rate

		normalRateInfo.reset(elapsed);
		batchRateInfo.reset(elapsed);

		int transactionsStarted[2] = {0,0};
		int systemTransactionsStarted[2] = {0,0};
		int defaultPriTransactionsStarted[2] = { 0, 0 };
		int batchPriTransactionsStarted[2] = { 0, 0 };

		vector<vector<GetReadVersionRequest>> start(2);  // start[0] is transactions starting with !(flags&CAUSAL_READ_RISKY), start[1] is transactions starting with flags&CAUSAL_READ_RISKY
		Optional<UID> debugID;

		int requestsToStart = 0;
		while (!transactionQueue.empty() && requestsToStart < SERVER_KNOBS->START_TRANSACTION_MAX_REQUESTS_TO_START) {
			auto& req = transactionQueue.top().first;
			int tc = req.transactionCount;

			if(req.priority() < GetReadVersionRequest::PRIORITY_DEFAULT && !batchRateInfo.canStart(transactionsStarted[0] + transactionsStarted[1])) {
				break;
			}
			else if(req.priority() < GetReadVersionRequest::PRIORITY_SYSTEM_IMMEDIATE && !normalRateInfo.canStart(transactionsStarted[0] + transactionsStarted[1])) {
				break;	
			}

			if (req.debugID.present()) {
				if (!debugID.present()) debugID = nondeterministicRandom()->randomUniqueID();
				g_traceBatch.addAttach("TransactionAttachID", req.debugID.get().first(), debugID.get().first());
			}

			transactionsStarted[req.flags&1] += tc;
			if (req.priority() >= GetReadVersionRequest::PRIORITY_SYSTEM_IMMEDIATE)
				systemTransactionsStarted[req.flags & 1] += tc;
			else if (req.priority() >= GetReadVersionRequest::PRIORITY_DEFAULT)
				defaultPriTransactionsStarted[req.flags & 1] += tc;
			else
				batchPriTransactionsStarted[req.flags & 1] += tc;

			start[req.flags & 1].push_back(std::move(req));  static_assert(GetReadVersionRequest::FLAG_CAUSAL_READ_RISKY == 1, "Implementation dependent on flag value");
			transactionQueue.pop();
			requestsToStart++;
		}

		if (!transactionQueue.empty())
			forwardPromise(GRVTimer, delayJittered(SERVER_KNOBS->START_TRANSACTION_BATCH_QUEUE_CHECK_INTERVAL, TaskPriority::ProxyGRVTimer));

		/*TraceEvent("GRVBatch", proxy.id())
		.detail("Elapsed", elapsed)
		.detail("NTransactionToStart", nTransactionsToStart)
		.detail("TransactionRate", transactionRate)
		.detail("TransactionQueueSize", transactionQueue.size())
		.detail("NumTransactionsStarted", transactionsStarted[0] + transactionsStarted[1]) 
		.detail("NumSystemTransactionsStarted", systemTransactionsStarted[0] + systemTransactionsStarted[1])
		.detail("NumNonSystemTransactionsStarted", transactionsStarted[0] + transactionsStarted[1] - systemTransactionsStarted[0] - systemTransactionsStarted[1])
		.detail("TransactionBudget", transactionBudget)
		.detail("BatchTransactionBudget", batchTransactionBudget);*/

		transactionCount += transactionsStarted[0] + transactionsStarted[1];
		batchTransactionCount += batchPriTransactionsStarted[0] + batchPriTransactionsStarted[1];

		normalRateInfo.updateBudget(transactionsStarted[0] + transactionsStarted[1]);
		batchRateInfo.updateBudget(transactionsStarted[0] + transactionsStarted[1]);

		if (debugID.present()) {
			g_traceBatch.addEvent("TransactionDebug", debugID.get().first(), "MasterProxyServer.masterProxyServerCore.Broadcast");
		}

		for (int i = 0; i < start.size(); i++) {
			if (start[i].size()) {
				Future<GetReadVersionReply> readVersionReply = getLiveCommittedVersion(commitData, i, &otherProxies, debugID, transactionsStarted[i], systemTransactionsStarted[i], defaultPriTransactionsStarted[i], batchPriTransactionsStarted[i]);
				addActor.send(sendGrvReplies(readVersionReply, start[i], &commitData->stats));

				// for now, base dynamic batching on the time for normal requests (not read_risky)
				if (i == 0) { 
					addActor.send(timeReply(readVersionReply, replyTimes));
				}
			}
		}
	}
}

ACTOR static Future<Void> readRequestServer( MasterProxyInterface proxy, ProxyCommitData* commitData ) {
	// Implement read-only parts of the proxy interface
	// We can't respond to these requests until we have valid txnStateStore
	wait(commitData->validState.getFuture());

	TraceEvent("ProxyReadyForReads", proxy.id());

	loop {
		GetKeyServerLocationsRequest req = waitNext(proxy.getKeyServersLocations.getFuture());
		++commitData->stats.keyServerLocationRequests;
		GetKeyServerLocationsReply rep;
		if(!req.end.present()) {
			auto r = req.reverse ? commitData->keyInfo.rangeContainingKeyBefore(req.begin) : commitData->keyInfo.rangeContaining(req.begin);
			vector<StorageServerInterface> ssis;
			ssis.reserve(r.value().src_info.size());
			for(auto& it : r.value().src_info) {
				ssis.push_back(it->interf);
			}
			rep.results.push_back(std::make_pair(r.range(), ssis));
		} else if(!req.reverse) {
			int count = 0;
			for(auto r = commitData->keyInfo.rangeContaining(req.begin); r != commitData->keyInfo.ranges().end() && count < req.limit && r.begin() < req.end.get(); ++r) {
				vector<StorageServerInterface> ssis;
				ssis.reserve(r.value().src_info.size());
				for(auto& it : r.value().src_info) {
					ssis.push_back(it->interf);
				}
				rep.results.push_back(std::make_pair(r.range(), ssis));
				count++;
			}
		} else {
			int count = 0;
			auto r = commitData->keyInfo.rangeContainingKeyBefore(req.end.get());
			while( count < req.limit && req.begin < r.end() ) {
				vector<StorageServerInterface> ssis;
				ssis.reserve(r.value().src_info.size());
				for(auto& it : r.value().src_info) {
					ssis.push_back(it->interf);
				}
				rep.results.push_back(std::make_pair(r.range(), ssis));
				if(r == commitData->keyInfo.ranges().begin()) {
					break;
				}
				count++;
				--r;
			}
		}
		req.reply.send(rep);
		wait(yield());
	}
}

ACTOR static Future<Void> rejoinServer( MasterProxyInterface proxy, ProxyCommitData* commitData ) {
	// We can't respond to these requests until we have valid txnStateStore
	wait(commitData->validState.getFuture());

	loop {
		GetStorageServerRejoinInfoRequest req = waitNext(proxy.getStorageServerRejoinInfo.getFuture());
		if (commitData->txnStateStore->readValue(serverListKeyFor(req.id)).get().present()) {
			GetStorageServerRejoinInfoReply rep;
			rep.version = commitData->version;
			rep.tag = decodeServerTagValue( commitData->txnStateStore->readValue(serverTagKeyFor(req.id)).get().get() );
			Standalone<VectorRef<KeyValueRef>> history = commitData->txnStateStore->readRange(serverTagHistoryRangeFor(req.id)).get();
			for(int i = history.size()-1; i >= 0; i-- ) {
				rep.history.push_back(std::make_pair(decodeServerTagHistoryKey(history[i].key), decodeServerTagValue(history[i].value)));
			}
			auto localityKey = commitData->txnStateStore->readValue(tagLocalityListKeyFor(req.dcId)).get();
			if( localityKey.present() ) {
				rep.newLocality = false;
				int8_t locality = decodeTagLocalityListValue(localityKey.get());
				if(locality != rep.tag.locality) {
					uint16_t tagId = 0;
					std::vector<uint16_t> usedTags;
					auto tagKeys = commitData->txnStateStore->readRange(serverTagKeys).get();
					for( auto& kv : tagKeys ) {
						Tag t = decodeServerTagValue( kv.value );
						if(t.locality == locality) {
							usedTags.push_back(t.id);
						}
					}
					auto historyKeys = commitData->txnStateStore->readRange(serverTagHistoryKeys).get();
					for( auto& kv : historyKeys ) {
						Tag t = decodeServerTagValue( kv.value );
						if(t.locality == locality) {
							usedTags.push_back(t.id);
						}
					}
					std::sort(usedTags.begin(), usedTags.end());

					int usedIdx = 0;
					for(; usedTags.size() > 0 && tagId <= usedTags.end()[-1]; tagId++) {
						if(tagId < usedTags[usedIdx]) {
							break;
						} else {
							usedIdx++;
						}
					}
					rep.newTag = Tag(locality, tagId);
				}
			} else {
				rep.newLocality = true;
				int8_t maxTagLocality = -1;
				auto localityKeys = commitData->txnStateStore->readRange(tagLocalityListKeys).get();
				for( auto& kv : localityKeys ) {
					maxTagLocality = std::max(maxTagLocality, decodeTagLocalityListValue( kv.value ));
				}
				rep.newTag = Tag(maxTagLocality+1,0);
			}
			req.reply.send(rep);
		} else {
			req.reply.sendError(worker_removed());
		}
	}
}

ACTOR Future<Void> healthMetricsRequestServer(MasterProxyInterface proxy, GetHealthMetricsReply* healthMetricsReply, GetHealthMetricsReply* detailedHealthMetricsReply)
{
	loop {
		choose {
			when(GetHealthMetricsRequest req =
				 waitNext(proxy.getHealthMetrics.getFuture()))
			{
				if (req.detailed)
					req.reply.send(*detailedHealthMetricsReply);
				else
					req.reply.send(*healthMetricsReply);
			}
		}
	}
}

ACTOR Future<Void> monitorRemoteCommitted(ProxyCommitData* self) {
	loop {
		wait(delay(0)); //allow this actor to be cancelled if we are removed after db changes.
		state Optional<std::vector<OptionalInterface<TLogInterface>>> remoteLogs;
		if(self->db->get().recoveryState >= RecoveryState::ALL_LOGS_RECRUITED) {
			for(auto& logSet : self->db->get().logSystemConfig.tLogs) {
				if(!logSet.isLocal) {
					remoteLogs = logSet.tLogs;
					for(auto& tLog : logSet.tLogs) {
						if(!tLog.present()) {
							remoteLogs = Optional<std::vector<OptionalInterface<TLogInterface>>>();
							break;
						}
					}
					break;
				}
			}
		}

		if(!remoteLogs.present()) {
			wait(self->db->onChange());
			continue;
		}
		self->popRemoteTxs = true;

		state Future<Void> onChange = self->db->onChange();
		loop {
			state std::vector<Future<TLogQueuingMetricsReply>> replies;
			for(auto &it : remoteLogs.get()) {
				replies.push_back(brokenPromiseToNever( it.interf().getQueuingMetrics.getReply( TLogQueuingMetricsRequest() ) ));
			}
			wait( waitForAll(replies) || onChange );

			if(onChange.isReady()) {
				break;
			}

			//FIXME: use the configuration to calculate a more precise minimum recovery version.
			Version minVersion = std::numeric_limits<Version>::max();
			for(auto& it : replies) {
				minVersion = std::min(minVersion, it.get().v);
			}

			while(self->txsPopVersions.size() && self->txsPopVersions.front().first <= minVersion) {
				self->lastTxsPop = self->txsPopVersions.front().second;
				self->logSystem->popTxs(self->txsPopVersions.front().second, tagLocalityRemoteLog);
				self->txsPopVersions.pop_front();
			}

			wait( delay(SERVER_KNOBS->UPDATE_REMOTE_LOG_VERSION_INTERVAL) || onChange );
			if(onChange.isReady()) {
				break;
			}
		}
	}
}


ACTOR Future<Void>
proxySnapCreate(ProxySnapRequest snapReq, ProxyCommitData* commitData)
{
	TraceEvent("SnapMasterProxy.SnapReqEnter")
		.detail("SnapPayload", snapReq.snapPayload)
		.detail("SnapUID", snapReq.snapUID);
	try {
		// whitelist check
		ExecCmdValueString execArg(snapReq.snapPayload);
		StringRef binPath = execArg.getBinaryPath();
		if (!isWhitelisted(commitData->whitelistedBinPathVec, binPath)) {
			TraceEvent("SnapMasterProxy.WhiteListCheckFailed")
				.detail("SnapPayload", snapReq.snapPayload)
				.detail("SnapUID", snapReq.snapUID);
			throw transaction_not_permitted();
		}
		// db fully recovered check
		if (commitData->db->get().recoveryState != RecoveryState::FULLY_RECOVERED)  {
			// Cluster is not fully recovered and needs TLogs
			// from previous generation for full recovery.
			// Currently, snapshot of old tlog generation is not
			// supported and hence failing the snapshot request until
			// cluster is fully_recovered.
			TraceEvent("SnapMasterProxy.ClusterNotFullyRecovered")
				.detail("SnapPayload", snapReq.snapPayload)
				.detail("SnapUID", snapReq.snapUID);
			throw cluster_not_fully_recovered();
		}

		auto result =
			commitData->txnStateStore->readValue(LiteralStringRef("log_anti_quorum").withPrefix(configKeysPrefix)).get();
		int logAntiQuorum = 0;
		if (result.present()) {
			logAntiQuorum = atoi(result.get().toString().c_str());
		}
		// FIXME: logAntiQuorum not supported, remove it later,
		// In version2, we probably don't need this limtiation, but this needs to be tested.
		if (logAntiQuorum > 0) {
			TraceEvent("SnapMasterProxy.LogAnitQuorumNotSupported")
				.detail("SnapPayload", snapReq.snapPayload)
				.detail("SnapUID", snapReq.snapUID);
			throw txn_exec_log_anti_quorum();
		}

		// send a snap request to DD
		if (!commitData->db->get().distributor.present()) {
			TraceEvent(SevWarnAlways, "DataDistributorNotPresent");
			throw operation_failed();
		}
		state Future<ErrorOr<Void>> ddSnapReq =
			commitData->db->get().distributor.get().distributorSnapReq.tryGetReply(DistributorSnapRequest(snapReq.snapPayload, snapReq.snapUID));
		try {
			wait(throwErrorOr(ddSnapReq));
		} catch (Error& e) {
			TraceEvent("SnapMasterProxy.DDSnapResponseError")
				.detail("SnapPayload", snapReq.snapPayload)
				.detail("SnapUID", snapReq.snapUID)
				.error(e, true /*includeCancelled*/ );
			throw e;
		}
		snapReq.reply.send(Void());
	} catch (Error& e) {
		TraceEvent("SnapMasterProxy.SnapReqError")
			.detail("SnapPayload", snapReq.snapPayload)
			.detail("SnapUID", snapReq.snapUID)
			.error(e, true /*includeCancelled*/);
		if (e.code() != error_code_operation_cancelled) {
			snapReq.reply.sendError(e);
		} else {
			throw e;
		}
	}
	TraceEvent("SnapMasterProxy.SnapReqExit")
		.detail("SnapPayload", snapReq.snapPayload)
		.detail("SnapUID", snapReq.snapUID);
	return Void();
}

ACTOR Future<Void> masterProxyServerCore(
	MasterProxyInterface proxy,
	MasterInterface master,
	Reference<AsyncVar<ServerDBInfo>> db,
	LogEpoch epoch,
	Version recoveryTransactionVersion,
	bool firstProxy,
	std::string whitelistBinPaths)
{
	state ProxyCommitData commitData(proxy.id(), master, proxy.getConsistentReadVersion, recoveryTransactionVersion, proxy.commit, db, firstProxy);

	state Future<Sequence> sequenceFuture = (Sequence)0;
	state PromiseStream< std::pair<vector<CommitTransactionRequest>, int> > batchedCommits;
	state Future<Void> commitBatcherActor;
	state Future<Void> lastCommitComplete = Void();

	state PromiseStream<Future<Void>> addActor;
	state Future<Void> onError = transformError( actorCollection(addActor.getFuture()), broken_promise(), master_tlog_failed() );
	state double lastCommit = 0;
	state std::set<Sequence> txnSequences;
	state Sequence maxSequence = std::numeric_limits<Sequence>::max();

	state GetHealthMetricsReply healthMetricsReply;
	state GetHealthMetricsReply detailedHealthMetricsReply;

	addActor.send( fetchVersions(&commitData) );
	addActor.send( waitFailureServer(proxy.waitFailure.getFuture()) );

	//TraceEvent("ProxyInit1", proxy.id());

	// Wait until we can load the "real" logsystem, since we don't support switching them currently
	while (!(commitData.db->get().master.id() == master.id() && commitData.db->get().recoveryState >= RecoveryState::RECOVERY_TRANSACTION)) {
		//TraceEvent("ProxyInit2", proxy.id()).detail("LSEpoch", db->get().logSystemConfig.epoch).detail("Need", epoch);
		wait(commitData.db->onChange());
	}
	state Future<Void> dbInfoChange = commitData.db->onChange();
	//TraceEvent("ProxyInit3", proxy.id());

	commitData.resolvers = commitData.db->get().resolvers;
	ASSERT(commitData.resolvers.size() != 0);

	auto rs = commitData.keyResolvers.modify(allKeys);
	for(auto r = rs.begin(); r != rs.end(); ++r)
		r->value().emplace_back(0,0);

	commitData.logSystem = ILogSystem::fromServerDBInfo(proxy.id(), commitData.db->get(), false, addActor);
	commitData.logAdapter = new LogSystemDiskQueueAdapter(commitData.logSystem, Reference<AsyncVar<PeekTxsInfo>>(), false);
	commitData.txnStateStore = keyValueStoreLogSystem(commitData.logAdapter, proxy.id(), 2e9, true, true, true);
	createWhitelistBinPathVec(whitelistBinPaths, commitData.whitelistedBinPathVec);

	// ((SERVER_MEM_LIMIT * COMMIT_BATCHES_MEM_FRACTION_OF_TOTAL) / COMMIT_BATCHES_MEM_TO_TOTAL_MEM_SCALE_FACTOR) is only a approximate formula for limiting the memory used.
	// COMMIT_BATCHES_MEM_TO_TOTAL_MEM_SCALE_FACTOR is an estimate based on experiments and not an accurate one.
	state int64_t commitBatchesMemoryLimit = std::min(SERVER_KNOBS->COMMIT_BATCHES_MEM_BYTES_HARD_LIMIT, static_cast<int64_t>((SERVER_KNOBS->SERVER_MEM_LIMIT * SERVER_KNOBS->COMMIT_BATCHES_MEM_FRACTION_OF_TOTAL) / SERVER_KNOBS->COMMIT_BATCHES_MEM_TO_TOTAL_MEM_SCALE_FACTOR));
	TraceEvent(SevInfo, "CommitBatchesMemoryLimit").detail("BytesLimit", commitBatchesMemoryLimit);

	addActor.send(monitorRemoteCommitted(&commitData));
	addActor.send(transactionStarter(proxy, commitData.db, addActor, &commitData, &healthMetricsReply, &detailedHealthMetricsReply));
	addActor.send(readRequestServer(proxy, &commitData));
	addActor.send(rejoinServer(proxy, &commitData));
	addActor.send(healthMetricsRequestServer(proxy, &healthMetricsReply, &detailedHealthMetricsReply));

	// wait for txnStateStore recovery
	wait(success(commitData.txnStateStore->readValue(StringRef())));

	int commitBatchByteLimit = 
		(int)std::min<double>(SERVER_KNOBS->COMMIT_TRANSACTION_BATCH_BYTES_MAX, 
			std::max<double>(SERVER_KNOBS->COMMIT_TRANSACTION_BATCH_BYTES_MIN, 
				SERVER_KNOBS->COMMIT_TRANSACTION_BATCH_BYTES_SCALE_BASE * pow(commitData.db->get().client.proxies.size(), SERVER_KNOBS->COMMIT_TRANSACTION_BATCH_BYTES_SCALE_POWER)));

	commitBatcherActor = commitBatcher(&commitData, batchedCommits, proxy.commit.getFuture(), commitBatchByteLimit, commitBatchesMemoryLimit);
	loop choose{
		when( wait( dbInfoChange ) ) {
			dbInfoChange = commitData.db->onChange();
			if(commitData.db->get().master.id() == master.id() && commitData.db->get().recoveryState >= RecoveryState::RECOVERY_TRANSACTION) {
				commitData.logSystem = ILogSystem::fromServerDBInfo(proxy.id(), commitData.db->get(), false, addActor);
				for(auto it : commitData.tag_popped) {
					commitData.logSystem->pop(it.second, it.first);
				}
				commitData.logSystem->popTxs(commitData.lastTxsPop, tagLocalityRemoteLog);
			}

			Optional<LatencyBandConfig> newLatencyBandConfig = commitData.db->get().latencyBandConfig;

			if(newLatencyBandConfig.present() != commitData.latencyBandConfig.present()
				|| (newLatencyBandConfig.present() && newLatencyBandConfig.get().grvConfig != commitData.latencyBandConfig.get().grvConfig))
			{
				TraceEvent("LatencyBandGrvUpdatingConfig").detail("Present", newLatencyBandConfig.present());
				commitData.stats.grvLatencyBands.clearBands();
				if(newLatencyBandConfig.present()) {
					for(auto band : newLatencyBandConfig.get().grvConfig.bands) {
						commitData.stats.grvLatencyBands.addThreshold(band);
					}
				}
			}

			if(newLatencyBandConfig.present() != commitData.latencyBandConfig.present()
				|| (newLatencyBandConfig.present() && newLatencyBandConfig.get().commitConfig != commitData.latencyBandConfig.get().commitConfig))
			{
				TraceEvent("LatencyBandCommitUpdatingConfig").detail("Present", newLatencyBandConfig.present());
				commitData.stats.commitLatencyBands.clearBands();
				if(newLatencyBandConfig.present()) {
					for(auto band : newLatencyBandConfig.get().commitConfig.bands) {
						commitData.stats.commitLatencyBands.addThreshold(band);
					}
				}
			}

			commitData.latencyBandConfig = newLatencyBandConfig;
		}
		when(wait(onError)) {}
		when(std::pair<vector<CommitTransactionRequest>, int> batchedRequests = waitNext(batchedCommits.getFuture())) {
			const vector<CommitTransactionRequest> &trs = batchedRequests.first;
			int batchBytes = batchedRequests.second;
			//TraceEvent("MasterProxyCTR", proxy.id()).detail("CommitTransactions", trs.size()).detail("TransactionRate", transactionRate).detail("TransactionQueue", transactionQueue.size()).detail("ReleasedTransactionCount", transactionCount);
			if (trs.size() || (commitData.db->get().recoveryState >= RecoveryState::ACCEPTING_COMMITS && now() - lastCommit >= SERVER_KNOBS->MAX_COMMIT_BATCH_INTERVAL)) {
				lastCommit = now();

				if (trs.size() || lastCommitComplete.isReady()) {
					lastCommitComplete = commitBatch(&commitData, trs, batchBytes);
					addActor.send(lastCommitComplete);
				}
			}
		}
		when(GetRawCommittedVersionRequest req = waitNext(proxy.getRawCommittedVersion.getFuture())) {
			//TraceEvent("ProxyGetRCV", proxy.id());
			if (req.debugID.present())
				g_traceBatch.addEvent("TransactionDebug", req.debugID.get().first(), "MasterProxyServer.masterProxyServerCore.GetRawCommittedVersion");
			GetReadVersionReply rep;
			rep.locked = commitData.locked;
			rep.metadataVersion = commitData.metadataVersion;
			rep.version = commitData.committedVersion.get();
			req.reply.send(rep);
		}
		when(ProxySnapRequest snapReq = waitNext(proxy.proxySnapReq.getFuture())) {
			addActor.send(proxySnapCreate(snapReq, &commitData));
		}
		when(TxnStateRequest req = waitNext(proxy.txnState.getFuture())) {
			state ReplyPromise<Void> reply = req.reply;
			if(req.last) maxSequence = req.sequence + 1;
			if (!txnSequences.count(req.sequence)) {
				txnSequences.insert(req.sequence);
				
				ASSERT(!commitData.validState.isSet()); // Although we may receive the CommitTransactionRequest for the recovery transaction before all of the TxnStateRequest, we will not get a resolution result from any resolver until the master has submitted its initial (sequence 0) resolution request, which it doesn't do until we have acknowledged all TxnStateRequests

				for(auto& kv : req.data)
					commitData.txnStateStore->set(kv, &req.arena);
				commitData.txnStateStore->commit(true);

				if(txnSequences.size() == maxSequence) {
					state KeyRange txnKeys = allKeys;
					loop {
						wait(yield());
						Standalone<VectorRef<KeyValueRef>> data = commitData.txnStateStore->readRange(txnKeys, SERVER_KNOBS->BUGGIFIED_ROW_LIMIT, SERVER_KNOBS->APPLY_MUTATION_BYTES).get();
						if(!data.size()) break;
						((KeyRangeRef&)txnKeys) = KeyRangeRef( keyAfter(data.back().key, txnKeys.arena()), txnKeys.end );

						Standalone<VectorRef<MutationRef>> mutations;
						std::vector<std::pair<MapPair<Key,ServerCacheInfo>,int>> keyInfoData;
						vector<UID> src, dest;
						ServerCacheInfo info;
						for(auto &kv : data) {
							if( kv.key.startsWith(keyServersPrefix) ) {
								KeyRef k = kv.key.removePrefix(keyServersPrefix);
								if(k != allKeys.end) {
									decodeKeyServersValue(kv.value, src, dest);
									info.tags.clear();
									info.src_info.clear();
									info.dest_info.clear();
									for (const auto& id : src) {
										auto storageInfo = getStorageInfo(id, &commitData.storageCache, commitData.txnStateStore);
										ASSERT(storageInfo->tag != invalidTag);
										info.tags.push_back( storageInfo->tag );
										info.src_info.push_back( storageInfo );
									}
									for (const auto& id : dest) {
										auto storageInfo = getStorageInfo(id, &commitData.storageCache, commitData.txnStateStore);
										ASSERT(storageInfo->tag != invalidTag);
										info.tags.push_back( storageInfo->tag );
										info.dest_info.push_back( storageInfo );
									}
									uniquify(info.tags);
									keyInfoData.emplace_back(MapPair<Key,ServerCacheInfo>(k, info), 1);
								}
							} else {
								mutations.push_back(mutations.arena(), MutationRef(MutationRef::SetValue, kv.key, kv.value));
							}
						}
						
						//insert keyTag data separately from metadata mutations so that we can do one bulk insert which avoids a lot of map lookups.
						commitData.keyInfo.rawInsert(keyInfoData); 

						Arena arena;
						bool confChanges;
						applyMetadataMutations(commitData.dbgid, arena, mutations, commitData.txnStateStore, NULL, &confChanges, Reference<ILogSystem>(), 0, &commitData.vecBackupKeys, &commitData.keyInfo, commitData.firstProxy ? &commitData.uid_applyMutationsData : NULL, commitData.commit, commitData.cx, &commitData.committedVersion, &commitData.storageCache, &commitData.tag_popped, true );
					}

					auto lockedKey = commitData.txnStateStore->readValue(databaseLockedKey).get();
					commitData.locked = lockedKey.present() && lockedKey.get().size();
					commitData.metadataVersion = commitData.txnStateStore->readValue(metadataVersionKey).get();

					commitData.txnStateStore->enableSnapshot();
				}
			}
			reply.send(Void());
			wait(yield());
		}
	}
}

ACTOR Future<Void> checkRemoved(Reference<AsyncVar<ServerDBInfo>> db, uint64_t recoveryCount, MasterProxyInterface myInterface) {
	loop{
		if (db->get().recoveryCount >= recoveryCount && !std::count(db->get().client.proxies.begin(), db->get().client.proxies.end(), myInterface)) {
			throw worker_removed();
		}
		wait(db->onChange());
	}
}

ACTOR template <class X> Future<Void> stripRequests( RequestStream<X> in, PromiseStream<ReplyPromise<REPLY_TYPE(X)>> out, int* count) {
	loop {
		X req = waitNext(in.getFuture());
		out.send(req.reply);
		if((*count) >= 0 && ++(*count) >= SERVER_KNOBS->MAX_FORWARD_MESSAGES) {
			TraceEvent(SevWarnAlways, "TooManyProxyForwardRequests");
			return Void();
		}
	}
}

ACTOR Future<Void> forwardProxy(ClientDBInfo info, PromiseStream<ReplyPromise<CommitID>> commitReplies, PromiseStream<ReplyPromise<GetReadVersionReply>> grvReplies, PromiseStream<ReplyPromise<GetKeyServerLocationsReply>> locationReplies) {
	loop {
		choose {
			when(ReplyPromise<CommitID> req = waitNext(commitReplies.getFuture())) {
				CommitID rep;
				rep.newClientInfo = info;
				req.send(rep);
			}
			when(ReplyPromise<GetReadVersionReply> req = waitNext(grvReplies.getFuture())) {
				GetReadVersionReply rep;
				rep.newClientInfo = info;
				req.send(rep);
			}
			when(ReplyPromise<GetKeyServerLocationsReply> req = waitNext(locationReplies.getFuture())) {
				GetKeyServerLocationsReply rep;
				rep.newClientInfo = info;
				req.send(rep);
			}
		}
		wait(yield());
	}
}

ACTOR Future<Void> masterProxyServer(
	MasterProxyInterface proxy,
	InitializeMasterProxyRequest req,
	Reference<AsyncVar<ServerDBInfo>> db,
	std::string whitelistBinPaths)
{
	state Future<Void> core;
	try {
		core = masterProxyServerCore(proxy, req.master, db, req.recoveryCount, req.recoveryTransactionVersion, req.firstProxy, whitelistBinPaths);
		wait(core || checkRemoved(db, req.recoveryCount, proxy));
	}
	catch (Error& e) {
		TraceEvent("MasterProxyTerminated", proxy.id()).error(e, true);

		if (e.code() != error_code_worker_removed && e.code() != error_code_tlog_stopped &&
			e.code() != error_code_master_tlog_failed && e.code() != error_code_coordinators_changed &&
			e.code() != error_code_coordinated_state_conflict && e.code() != error_code_new_coordinators_timed_out) {
			throw;
		}
	}
	core.cancel();
	state PromiseStream<ReplyPromise<CommitID>> commitReplies;
	state PromiseStream<ReplyPromise<GetReadVersionReply>> grvReplies;
	state PromiseStream<ReplyPromise<GetKeyServerLocationsReply>> locationReplies;
	state int replyCount = 0;
	state Future<Void> finishForward = delay(SERVER_KNOBS->PROXY_FORWARD_DELAY) || stripRequests(proxy.commit, commitReplies, &replyCount) || stripRequests(proxy.getConsistentReadVersion, grvReplies, &replyCount) || stripRequests(proxy.getKeyServersLocations, locationReplies, &replyCount);
	proxy = MasterProxyInterface();
	loop {
		if(finishForward.isReady()) {
			return Void();
		}
		if(db->get().client.proxies.size() > 0 && !db->get().client.proxies[0].provisional && db->get().recoveryCount >= req.recoveryCount
			&& !std::count(db->get().client.proxies.begin(), db->get().client.proxies.end(), proxy)) {
			replyCount = -1;
			core = forwardProxy(db->get().client, commitReplies, grvReplies, locationReplies);
			wait(finishForward);
			return Void();
		}
		wait(db->onChange() || finishForward);
	}
}
