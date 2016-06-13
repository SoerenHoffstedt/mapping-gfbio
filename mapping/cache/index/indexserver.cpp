/*
 * indexserver.cpp
 *
 *  Created on: 26.05.2015
 *      Author: mika
 */

#include "cache/index/indexserver.h"
#include "cache/index/reorg_strategy.h"
#include "util/make_unique.h"
#include "util/concat.h"

#include <memory>

#include <stdlib.h>
#include <stdio.h>

// socket() etc
#include <sys/types.h>
#include <sys/socket.h>

#include <unistd.h>
#include <errno.h>

////////////////////////////////////////////////////////////
//
// INDEX SERVER
//
////////////////////////////////////////////////////////////

IndexServer::IndexServer(int port, time_t update_interval, const std::string &reorg_strategy, const std::string &relevance_function, const std::string &scheduler) :
	caches(reorg_strategy,relevance_function), port(port), shutdown(false), next_node_id(1),
	query_manager(QueryManager::by_name(this->caches,this->nodes,scheduler)), last_reorg(CacheCommon::time_millis()), update_interval(update_interval), wakeup_pipe(BinaryStream::makePipe()) {
}

void IndexServer::stop() {
	Log::info("Shutting down.");
	shutdown = true;
	wakeup();
}

void IndexServer::wakeup() {
	BinaryWriteBuffer buffer;
	buffer.write('w');
	wakeup_pipe.write(buffer);
}

void IndexServer::run() {
	int listen_socket = CacheCommon::get_listening_socket(port,true,SOMAXCONN);
	Log::info("index-server: listening on node-port: %d", port);


	struct pollfd fds[0xffff];
	fds[0].fd = listen_socket;
	fds[0].events = POLLIN;
	fds[1].fd = wakeup_pipe.getReadFD();
	fds[1].events = POLLIN;


	std::vector<std::unique_ptr<NewNBConnection>> new_cons;
	size_t num_fds;

	while (!shutdown) {
		// Prepare listen socket
		fds[0].revents = 0;

		// Prepare wakeup
		fds[1].revents = 0;

		num_fds = 2;

		// setup new connections
		auto nc_iter = new_cons.begin();
		while ( nc_iter != new_cons.end() ){
			if ( (*nc_iter)->is_faulty() )
				nc_iter = new_cons.erase(nc_iter);
			else {
				(*nc_iter)->prepare(&fds[num_fds++]);
				nc_iter++;
			}
		}


		setup_fdset(fds,num_fds);

		int poll_ret = poll(fds, num_fds, 1000 );
		if (poll_ret < 0 && errno != EINTR) {
			Log::error("Poll returned error: %s", strerror(errno));
			exit(1);
		}
		else if (poll_ret > 0) {
			if ( fds[1].revents & POLLIN ) {
				// we have been woken, now we need to read any outstanding data or the pipe will remain readable
				char buf[1024];
				read(wakeup_pipe.getReadFD(), buf, 1024);
			}

			process_worker_connections();
			process_control_connections();
			process_client_connections();
			process_handshake(new_cons);

			// Accept new connections
			if ( fds[0].revents & POLLIN ) {
				struct sockaddr_storage remote_addr;
				socklen_t sin_size = sizeof(remote_addr);
				int new_fd = accept(listen_socket, (struct sockaddr *) &remote_addr, &sin_size);
				if (new_fd == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
					Log::error("Accept failed: %d", strerror(errno));
				}
				else if (new_fd > 0) {
					Log::debug("New connection established, fd: %d", new_fd);
					new_cons.push_back( make_unique<NewNBConnection>(&remote_addr,new_fd) );
				}
			}
		}
		// Schedule Jobs
		query_manager->schedule_pending_jobs(worker_connections);

		if ( update_interval == 0 )
			continue;


		time_t now = CacheCommon::time_millis();
		time_t oldest_stats = now;
		bool all_idle = true;
		bool requires_reorg = false;

		// Check timestamps
		for (auto &kv : nodes) {
			Node& node = *kv.second;
			ControlConnection &cc = *control_connections.at(node.control_connection);
			oldest_stats = std::min(oldest_stats, node.last_stat_update);
			// Remeber if all connections are idle -> Allows reorg
			all_idle &= (cc.get_state() == ControlState::IDLE);
		}


		// Reorganize
		if ( oldest_stats > last_reorg ) {
			requires_reorg = caches.require_reorg( nodes );
			if ( requires_reorg && all_idle )
				reorganize();
		}

		// Update stats if applicable
		if ( !requires_reorg ) {
			for (auto &kv : nodes) {
				Node& node = *kv.second;
				ControlConnection &cc = *control_connections.at(node.control_connection);
				if (cc.get_state() == ControlState::IDLE && (now - node.last_stat_update) > update_interval) {
					cc.send_get_stats();
				}
			}
		}
	}

	close(listen_socket);
	Log::info("Index-Server done.");
}

void IndexServer::setup_fdset(struct pollfd *fds, size_t &pos) {

	auto wit = worker_connections.begin();
	while (wit != worker_connections.end()) {
		WorkerConnection &wc = *wit->second;
		if (wc.is_faulty()) {
			query_manager->worker_failed(wc.id);
			worker_connections.erase(wit++);
		}
		else {
			wc.prepare(&fds[pos++]);
			wit++;
		}
	}

	auto ccit = control_connections.begin();
	while (ccit != control_connections.end()) {
		ControlConnection &cc = *ccit->second;
		if (cc.is_faulty()) {
			caches.remove_all_by_node(cc.node_id);
			nodes.erase(cc.node_id);
			query_manager->node_failed(cc.node_id);
			control_connections.erase(ccit++);
		}
		else {
			cc.prepare(&fds[pos++]);
			ccit++;
		}
	}

	auto clit = client_connections.begin();
	while (clit != client_connections.end()) {
		ClientConnection &cc = *clit->second;
		if (cc.is_faulty()) {
			if ( cc.get_state() != ClientState::IDLE ) {
				Log::debug("Client connection cancelled: %ld", cc.id);
				query_manager->handle_client_abort(cc.id);
			}
			client_connections.erase(clit++);
		}
		else {
			cc.prepare(&fds[pos++]);
			clit++;
		}
	}
}

void IndexServer::process_handshake(std::vector<std::unique_ptr<NewNBConnection>> &new_fds) {
	auto it = new_fds.begin();
	while (it != new_fds.end()) {
		try {
			auto &nc = **it;
			if ( nc.process() ) {
				auto &data = nc.get_data();
				uint32_t magic = data.read<uint32_t>();
				switch (magic) {
					case ClientConnection::MAGIC_NUMBER: {
						std::unique_ptr<ClientConnection> cc = make_unique<ClientConnection>(nc.release_socket());
						Log::trace("New client connections established");
						if ( !client_connections.emplace(cc->id, std::move(cc)).second )
							throw MustNotHappenException("Emplaced same connection-id twice!");
						break;
					}
					case WorkerConnection::MAGIC_NUMBER: {
						uint32_t node_id = data.read<uint32_t>();
						std::unique_ptr<WorkerConnection> wc = make_unique<WorkerConnection>(nc.release_socket(),node_id);
						Log::info("New worker registered for node: %d, id: %d", node_id, wc->id);
						if ( !worker_connections.emplace(wc->id, std::move(wc)).second )
							throw MustNotHappenException("Emplaced same connection-id twice!");
						break;
					}
					case ControlConnection::MAGIC_NUMBER: {
						NodeHandshake hs(data);
						auto node = std::make_shared<Node>(next_node_id++, nc.hostname, hs);
						auto con  = make_unique<ControlConnection>(nc.release_socket(), node->id, node->host);
						node->control_connection = con->id;
						nodes.emplace(node->id, node);
						caches.process_handshake(node->id,hs);
						Log::info("New node registered. ID: %d, hostname: %s, control-connection-id: %d", node->id, nc.hostname.c_str(), con->id);
						if ( !control_connections.emplace(con->id, std::move(con)).second )
							throw MustNotHappenException("Emplaced same connection-id twice!");
						break;
					}
					default:
						Log::warn("Received unknown magic-number: %d. Dropping connection.", magic);
				}
				it = new_fds.erase(it);
			}
			else
				it++;
		} catch (const std::exception &e) {
			Log::error("Error on new connection: %s. Dropping.", e.what());
			it = new_fds.erase(it);
		}
	}
}

void IndexServer::process_control_connections() {
	for (auto &e : control_connections) {
		ControlConnection &cc = *e.second;
		// Check if node is waiting for a confirmation
		if ( cc.get_state() == ControlState::MOVE_RESULT_READ ) {
			auto res = cc.get_move_result();
			IndexCacheKey old(res.semantic_id, res.from_node_id, res.entry_id);
			if ( !query_manager->is_locked(res.type, old) )
				cc.confirm_move();
		}
		else if ( cc.get_state() == ControlState::REMOVE_REQUEST_READ ) {
			auto &node_key = cc.get_remove_request();
			IndexCacheKey key(node_key.semantic_id, cc.node_id, node_key.entry_id );
			if ( !query_manager->is_locked( node_key.type, key ) )
				cc.confirm_remove();
		}
		// Default handling
		else if ( cc.process() ) {
			switch (cc.get_state()) {
				case ControlState::MOVE_RESULT_READ: {
					Log::trace("Node %d migrated one cache-entry.", cc.node_id);
					auto res = cc.get_move_result();
					handle_reorg_result(res);
					IndexCacheKey old(res.semantic_id, res.from_node_id, res.entry_id);
					if ( !query_manager->is_locked(res.type, old) )
						cc.confirm_move();
					break;
				}
				case ControlState::REMOVE_REQUEST_READ: {
					Log::trace("Node %d requested removal of entry: %s", cc.node_id, cc.get_remove_request().to_string().c_str() );
					auto &node_key = cc.get_remove_request();
					IndexCacheKey key(node_key.semantic_id, cc.node_id, node_key.entry_id );
					if ( !query_manager->is_locked( node_key.type, key ) )
						cc.confirm_remove();
					break;
				}
				case ControlState::REORG_FINISHED:
					Log::trace("Node %d finished reorganization.", cc.node_id);
					cc.release();
					break;
				case ControlState::STATS_RECEIVED: {
					auto &stats = cc.get_stats();
					Log::trace("Node %d delivered fresh statistics", cc.node_id);
					auto &node = nodes.at(cc.node_id);
					node->update_stats(stats);
					node->last_stat_update = CacheCommon::time_millis();
					caches.update_stats(cc.node_id, stats);
					cc.release();
					break;
				}
				default:
					throw IllegalStateException(
						concat("Illegal control-connection state after read: ", (int) cc.get_state()));
			}
		}
	}
}

void IndexServer::handle_reorg_result(const ReorgMoveResult& res) {
	IndexCacheKey old(res.semantic_id, res.from_node_id, res.entry_id);
	IndexCacheKey new_key(res.semantic_id, res.to_node_id, res.to_cache_id);
	caches.get_cache(res.type).move(old,new_key);
}

void IndexServer::process_client_connections() {
	for (auto &e : client_connections) {
		ClientConnection &cc = *e.second;
		if ( cc.process() ) {
			// Handle state-changes
			switch (cc.get_state()) {
				case ClientState::AWAIT_RESPONSE:
					Log::debug("Client-request read: %s", cc.get_request().to_string().c_str() );

					query_manager->add_request(cc.id, cc.get_request());
					break;
				case ClientState::AWAIT_STATS: {
					SystemStats cumulated( query_manager->get_stats() );
					for ( auto &p : nodes )
						cumulated += p.second->get_query_stats();
					cc.send_stats(cumulated);
					break;
				}
				case ClientState::AWAIT_RESET:
					query_manager->reset_stats();
					for ( auto &p : nodes )
						p.second->reset_query_stats();
					cc.confirm_reset();
					break;
				default:
					throw IllegalStateException(
						concat("Illegal client-connection state after read: ", (int) cc.get_state()));
			}
		}
	}
}

void IndexServer::process_worker_connections() {
	for (auto &e : worker_connections) {
		WorkerConnection &wc = *e.second;
		if (wc.process()) {
			// Handle state-changes
			switch (wc.get_state()) {
				case WorkerState::ERROR: {
					Log::warn("Worker returned error: %s. Forwarding to client.",
						wc.get_error_message().c_str());
					query_manager->close_worker(wc.id);
					auto clients = query_manager->release_worker(wc.id);
					for (auto &cid : clients) {
						try {
							client_connections.at(cid)->send_error(wc.get_error_message());
						} catch (const std::out_of_range &oor) {
							Log::warn("Client %d does not exist.", cid);
						}
					}
					wc.release();
					break;
				}
				case WorkerState::DONE: {
					Log::debug("Worker returned result. Determinig delivery qty.");
					size_t qty = query_manager->close_worker(wc.id);
					wc.send_delivery_qty(qty);
					break;
				}
				case WorkerState::DELIVERY_READY: {
					auto &node = nodes.at(wc.node_id);
					DeliveryResponse response(node->host,node->port, wc.get_delivery_id());
					Log::debug("Worker returned delivery: %s", response.to_string().c_str());
					auto clients = query_manager->release_worker(wc.id);
					for (auto &cid : clients) {
						try {
							client_connections.at(cid)->send_response(response);
						} catch (const std::out_of_range &oor) {
							Log::warn("Client %d does not exist.", cid);
						}
					}
					wc.release();
					break;
				}
				case WorkerState::NEW_ENTRY: {
					Log::debug("Worker added new raster-entry");
					auto &mce = wc.get_new_entry();
					caches.get_cache( mce.type ).put(mce.semantic_id,wc.node_id,mce.entry_id,mce);
					wc.entry_cached();
					break;
				}
				case WorkerState::QUERY_REQUESTED: {
					Log::debug("Worker issued cache-query: %s", wc.get_query().to_string().c_str());
					query_manager->process_worker_query(wc);
					break;
				}
				default: {
					throw IllegalStateException(
						concat("Illegal worker-connection state after read: ", (int) wc.get_state()));
				}
			}
		}
	}
}

void IndexServer::reorganize(bool force) {
	// Remember time of this reorg
	last_reorg = CacheCommon::time_millis();
	auto reorgs = caches.reorganize(nodes,force);
	for (auto &d : reorgs) {
		for (auto &rm : d.second.get_removals()) {
			caches.get_cache(rm.type).remove(IndexCacheKey(rm.semantic_id, d.first, rm.entry_id));
		}

		if (!d.second.is_empty())
			control_connections.at(d.second.node->control_connection)->send_reorg(d.second);
	}
}

std::string IndexServer::stats_string() const {
	std::ostringstream out;
	out << "============ STATISTICS ============" << std::endl;
	out << query_manager->get_stats().to_string() << std::endl;
	for ( auto &p : nodes )
		out << p.second->to_string() << std::endl;
	out << "====================================" << std::endl;
	return out.str();
}
