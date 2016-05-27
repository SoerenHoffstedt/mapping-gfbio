/*
 * querymanager.cpp
 *
 *  Created on: 10.07.2015
 *      Author: mika
 */

#include "cache/index/indexserver.h"
#include "cache/index/query_manager/default_query_manager.h"
#include "cache/common.h"
#include "util/make_unique.h"

#include <algorithm>

DefaultQueryManager::DefaultQueryManager(IndexCacheManager &caches, const std::map<uint32_t, std::shared_ptr<Node>> &nodes) :
	QueryManager(nodes), caches(caches) {
}

void DefaultQueryManager::add_request(uint64_t client_id, const BaseRequest &req ) {
	stats.queries_issued++;
	TIME_EXEC("QueryManager.add_request");
	// Check if running jobs satisfy the given query
	for (auto &qi : queries) {
		if (qi.second->satisfies(req)) {
			qi.second->add_client(client_id);
			return;
		}
	}

	// Check if pending jobs satisfy the given query
	for (auto &j : pending_jobs) {
		if (j->satisfies(req)) {
			j->add_client(client_id);
			return;
		}
	}

	// Perform a cache-query
	auto &cache = caches.get_cache(req.type);
	auto res = cache.query(req.semantic_id, req.query);
	Log::debug("QueryResult: %s", res.to_string().c_str());

	//  No result --> Check if a pending query may be extended by the given query
	if ( res.keys.empty() ) {
		for (auto &j : pending_jobs) {
			if (j->extend(req)) {
				j->add_client(client_id);
				return;
			}
		}
	}
	// Create a new job
	auto job = create_job(req,res);
	job->add_client(client_id);
	pending_jobs.push_back(std::move(job));
}

void DefaultQueryManager::process_worker_query(WorkerConnection& con) {
	auto &req = con.get_query();
	auto &query = *queries.at(con.id);
	auto &cache = caches.get_cache(req.type);
	auto res = cache.query(req.semantic_id, req.query);
	Log::debug("QueryResult: %s", res.to_string().c_str());

	// Full single hit
	if (res.keys.size() == 1 && !res.has_remainder()) {
		Log::debug("Full HIT. Sending reference.");
		IndexCacheKey key(req.semantic_id, res.keys.at(0));
		auto node = nodes.at(key.get_node_id());
		CacheRef cr(node->host, node->port, key.get_entry_id());
		// Apply lock
		query.add_lock( CacheLocks::Lock(req.type,key) );
		con.send_hit(cr);
	}
	// Puzzle
	else if (res.has_hit() ) {
		Log::debug("Partial HIT. Sending puzzle-request, coverage: %f");
		std::vector<CacheRef> entries;
		for (auto &id : res.keys) {
			auto &node = nodes.at(id.first);
			query.add_lock(CacheLocks::Lock(req.type,IndexCacheKey(req.semantic_id, id)));
			entries.push_back(CacheRef(node->host, node->port, id.second));
		}
		PuzzleRequest pr( req.type, req.semantic_id, req.query, std::move(res.remainder), std::move(entries) );
		con.send_partial_hit(pr);
	}
	// Full miss
	else {
		Log::debug("Full MISS.");
		con.send_miss();
	}
}



//
// PRIVATE
//

std::unique_ptr<PendingQuery> DefaultQueryManager::create_job( const BaseRequest &req, const CacheQueryResult<std::pair<uint32_t,uint64_t>>& res) {
	TIME_EXEC("DefaultQueryManager.create_job");

	// Full single hit
	if (res.keys.size() == 1 && !res.has_remainder()) {
		stats.single_hits++;
		Log::debug("Full HIT. Sending reference.");
		IndexCacheKey key(req.semantic_id, res.keys.at(0));
		DeliveryRequest dr(
				req.type,
				req.semantic_id,
				res.covered,
				key.get_entry_id());
		return make_unique<DeliverJob>(std::move(dr), key);
	}
	// Puzzle
	else if (res.has_hit()) {
		Log::debug("Partial HIT. Sending puzzle-request.");
		std::set<uint32_t> node_ids;
		std::vector<IndexCacheKey> keys;
		std::vector<CacheRef> entries;
		for (auto id : res.keys) {
			IndexCacheKey key(req.semantic_id, id);
			auto &node = nodes.at(key.get_node_id());
			keys.push_back(key);
			node_ids.insert(key.get_node_id());
			entries.push_back(CacheRef(node->host, node->port, key.get_entry_id()));
		}
		PuzzleRequest pr(
				req.type,
				req.semantic_id,
				res.covered,
				std::move(res.remainder), std::move(entries));

		// STATS ONLY
		if ( pr.has_remainders() && node_ids.size() == 1 ) {
			stats.partial_single_node++;
		}
		else if ( pr.has_remainders() ) {
			stats.partial_multi_node++;
		}
		if ( !pr.has_remainders() && node_ids.size() == 1 ) {
			stats.multi_hits_single_node++;
		}
		else if ( pr.has_remainders() ) {
			stats.multi_hits_multi_node++;
		}
		// END STATS ONLY

		return make_unique<PuzzleJob>(std::move(pr), std::move(keys));
	}
	// Full miss
	else {
		stats.misses++;
		Log::debug("Full MISS.");
		return make_unique<CreateJob>(BaseRequest(req), *this);
	}
}

std::unique_ptr<PendingQuery> DefaultQueryManager::recreate_job(const RunningQuery& query) {
	auto &req = query.get_request();
	auto &cache = caches.get_cache(req.type);
	auto res = cache.query(req.semantic_id, req.query);
	auto job = create_job( req, res );
	job->add_clients( query.get_clients() );
	return job;
}

//
// Jobs
//

CreateJob::CreateJob( BaseRequest&& request, const DefaultQueryManager &mgr ) :
	PendingQuery(), request(request),
	orig_query(this->request.query),
	orig_area( (this->request.query.x2 - this->request.query.x1) * (this->request.query.y2 - this->request.query.y1)),
	mgr(mgr) {
}

bool CreateJob::extend(const BaseRequest& req) {
	if ( req.type == request.type &&
		 req.semantic_id == request.semantic_id &&
		 orig_query.TemporalReference::contains(req.query) &&
		 orig_query.restype == req.query.restype) {

		double nx1, nx2, ny1, ny2, narea;

		nx1 = std::min(request.query.x1, req.query.x1);
		ny1 = std::min(request.query.y1, req.query.y1);
		nx2 = std::max(request.query.x2, req.query.x2);
		ny2 = std::max(request.query.y2, req.query.y2);

		narea = (nx2 - nx1) * (ny2 - ny1);

		if (orig_query.restype == QueryResolution::Type::NONE && narea / orig_area <= 4.01) {
			SpatialReference sref(orig_query.epsg, nx1, ny1, nx2, ny2);
			request.query = QueryRectangle(sref, orig_query, orig_query);
			return true;
		}
		else if (orig_query.restype == QueryResolution::Type::PIXELS && narea / orig_area <= 4.01) {
			// Check resolution
			double my_xres = (orig_query.x2 - orig_query.x1) / orig_query.xres;
			double my_yres = (orig_query.y2 - orig_query.y1) / orig_query.yres;

			double q_xres = (req.query.x2 - req.query.x1) / req.query.xres;
			double q_yres = (req.query.y2 - req.query.y1) / req.query.yres;

			if (std::abs(1.0 - my_xres / q_xres) < 0.01 && std::abs(1.0 - my_yres / q_yres) < 0.01) {

				uint32_t nxres = std::ceil(
					orig_query.xres * ((nx2 - nx1) / (orig_query.x2 - orig_query.x1)));
				uint32_t nyres = std::ceil(
					orig_query.yres * ((ny2 - ny1) / (orig_query.y2 - orig_query.y1)));

				SpatialReference sref(orig_query.epsg, nx1, ny1, nx2, ny2);
				request.query = QueryRectangle(sref, orig_query, QueryResolution::pixels(nxres, nyres));
				return true;
			}
		}
	}
	return false;
}

uint64_t CreateJob::schedule(const std::map<uint64_t, std::unique_ptr<WorkerConnection> >& connections) {
	// Do not schedule if we have no nodes
	if ( mgr.nodes.empty() )
		return 0;

	uint32_t node_id = mgr.caches.find_node_for_job(request,mgr.nodes);
	for (auto &e : connections) {
		auto &con = *e.second;
		if (!con.is_faulty() && con.node_id == node_id && con.get_state() == WorkerState::IDLE) {
			con.process_request(WorkerConnection::CMD_CREATE, request);
			return con.id;
		}
	}
	// Fallback
	for (auto &e : connections) {
		auto &con = *e.second;
		if (!con.is_faulty() && con.get_state() == WorkerState::IDLE) {
			con.process_request(WorkerConnection::CMD_CREATE, request);
			return con.id;
		}
	}
	return 0;
}

bool CreateJob::is_affected_by_node(uint32_t node_id) {
	(void) node_id;
	return false;
}

const BaseRequest& CreateJob::get_request() const {
	return request;
}

//
// DELIVCER JOB
//

DeliverJob::DeliverJob(DeliveryRequest&& request, const IndexCacheKey &key) :
	PendingQuery(std::vector<CacheLocks::Lock>{CacheLocks::Lock(request.type,key)}),
	request(request),
	node(key.get_node_id()) {
}

uint64_t DeliverJob::schedule(const std::map<uint64_t, std::unique_ptr<WorkerConnection> >& connections) {
	for (auto &e : connections) {
		auto &con = *e.second;
		if (!con.is_faulty() && con.node_id == node && con.get_state() == WorkerState::IDLE) {
			con.process_request(WorkerConnection::CMD_DELIVER, request);
			return con.id;
		}
	}
	return 0;
}

bool DeliverJob::is_affected_by_node(uint32_t node_id) {
	return node_id == node;
}

bool DeliverJob::extend(const BaseRequest&) {
	return false;
}

const BaseRequest& DeliverJob::get_request() const {
	return request;
}

//
// PUZZLE JOB
//

PuzzleJob::PuzzleJob(PuzzleRequest&& request, const std::vector<IndexCacheKey> &keys) :
	PendingQuery(),
	request(std::move(request)) {
	for ( auto &k : keys ) {
		nodes.insert(k.get_node_id());
		add_lock( CacheLocks::Lock(request.type,k) );
	}
}

uint64_t PuzzleJob::schedule(const std::map<uint64_t, std::unique_ptr<WorkerConnection> >& connections) {
	for (auto &node : nodes) {
		for (auto &e : connections) {
			auto &con = *e.second;
			if (!con.is_faulty() && con.node_id == node
				&& con.get_state() == WorkerState::IDLE) {
				con.process_request(WorkerConnection::CMD_PUZZLE, request);
				return con.id;
			}
		}
	}
	return 0;
}

bool PuzzleJob::is_affected_by_node(uint32_t node_id) {
	return nodes.find(node_id) != nodes.end();
}


bool PuzzleJob::extend(const BaseRequest&) {
	return false;
}

const BaseRequest& PuzzleJob::get_request() const {
	return request;
}