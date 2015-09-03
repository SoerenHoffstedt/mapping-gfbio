/*
 * server_main.cpp
 *
 *  Created on: 18.05.2015
 *      Author: mika
 */

#include "cache/node/nodeserver.h"
#include "cache/manager.h"
#include "cache/common.h"
#include "cache/node/puzzletracer.h"
#include "util/configuration.h"
#include <signal.h>

NodeServer *instance = nullptr;

void termination_handler(int signum) {
	if ( signum == SIGSEGV ) {
		printf("Segmentation fault. Stacktrace:\n%s", CacheCommon::get_stacktrace().c_str());
		exit(1);
	}
	else {
		instance->stop();
	}
}

void set_signal_handler() {
	struct sigaction new_action, old_action;

	/* Set up the structure to specify the new action. */
	new_action.sa_handler = termination_handler;
	sigemptyset(&new_action.sa_mask);
	new_action.sa_flags = 0;

	sigaction(SIGINT, NULL, &old_action);
	if (old_action.sa_handler != SIG_IGN)
		sigaction(SIGINT, &new_action, NULL);
	sigaction(SIGHUP, NULL, &old_action);
	if (old_action.sa_handler != SIG_IGN)
		sigaction(SIGHUP, &new_action, NULL);
	sigaction(SIGTERM, NULL, &old_action);
	if (old_action.sa_handler != SIG_IGN)
		sigaction(SIGTERM, &new_action, NULL);
	sigaction(SIGSEGV, NULL, &old_action);
		sigaction(SIGSEGV, &new_action, NULL);
}

int main(void) {
	CacheCommon::set_uncaught_exception_handler();
	set_signal_handler();
	Configuration::loadFromDefaultPaths();
	auto portstr = Configuration::get("nodeserver.port");
	auto hoststr = Configuration::get("nodeserver.host");
	auto portnr = atoi(portstr.c_str());

	auto iportstr = Configuration::get("indexserver.port");
	auto ihoststr = Configuration::get("indexserver.host");
	auto iportnr = atoi(iportstr.c_str());

	auto numThreadsstr = Configuration::get("nodeserver.threads", "4");
	auto num_threads = atoi(numThreadsstr.c_str());

//	PuzzleTracer::init();

	std::unique_ptr<CacheManager> cache_impl;
	// Inititalize cache
	if (Configuration::getBool("cache.enabled", false)) {
		size_t raster_size = atoi(Configuration::get("cache.raster.size", "5242880").c_str());
		cache_impl.reset(new RemoteCacheManager(raster_size,hoststr,portnr));

	} else {
		cache_impl.reset(new NopCacheManager());
	}
	CacheManager::init(std::move(cache_impl), make_unique<CacheAll>());


	instance = new NodeServer(hoststr,portnr,ihoststr,iportnr,num_threads);
	instance->run();
	return 0;
}

