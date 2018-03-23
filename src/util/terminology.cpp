
#include "terminology.h"
#include <vector>
#include <iostream>
#include <future>
#include "util/make_unique.h"
#include "util/configuration.h"

#include <boost/shared_ptr.hpp>
#include <boost/make_shared.hpp>
#include <boost/thread.hpp>
#include <boost/asio.hpp>
#include <boost/move/move.hpp>

typedef boost::packaged_task<std::string> task_t;
typedef boost::shared_ptr<task_t> ptask_t;

void Terminology::resolveMultiple(const std::vector<std::string> &names_in,
                                  std::vector<std::string> &names_out,
                                  const std::string &terminology,
                                  const std::string &key,
                                  const HandleNotResolvable on_not_resolvable){

    if(names_in.empty())
        return;

    //init thread pool
    int threads_num = Configuration::get<int>("terminology.threads",16);
    if(threads_num > names_in.size())
        threads_num = names_in.size();

    boost::asio::io_service io_service;
    boost::thread_group threads;
    boost::asio::io_service::work work(io_service);

    for(int i = 0; i < threads_num; i++){
        threads.create_thread(boost::bind(&boost::asio::io_service::run, &io_service));
    }

    std::vector<boost::shared_future<std::string>> pending_results;

    // create context, enable session cache. First name has to be resolved seperately and not in thread to
    // set the session ptr. Because the Session::Ptr can not be assigned a new value the local session_ptr
    // variable is passed as a raw pointer to the method.
    Poco::Net::Context::Ptr context = new Poco::Net::Context(
            Poco::Net::Context::CLIENT_USE,
            "",
            Poco::Net::Context::VERIFY_RELAXED,
            9,
            true
    );
    context->enableSessionCache(true);
    Poco::Net::Session::Ptr session_ptr;
    std::string first_resolved = resolveSingleNameSetSessionPtr(context, &session_ptr, names_in[0], terminology, key,
                                                                on_not_resolvable);
    names_out.push_back(first_resolved);

    //push tasks to resolve all names into the thread pool
    for(int i = 1; i < names_in.size(); i++) {
        const std::string &name = names_in[i];
        ptask_t task = boost::make_shared<task_t>(
                boost::bind(resolveSingleNameInternal, context, session_ptr, name, terminology, key, on_not_resolvable));
        boost::shared_future<std::string> future(task->get_future());
        pending_results.push_back(future);
        io_service.post(boost::bind(&task_t::operator(), task));
    }

    //get the results from the futures
    for(auto &future : pending_results){
        names_out.push_back(future.get());
    }}


std::string Terminology::resolveSingleNameInternal(Poco::Net::Context::Ptr &context,
                                                   Poco::Net::Session::Ptr &session_ptr,
                                                   const std::string &name,
                                                   const std::string &terminology,
                                                   const std::string &key,
                                                   HandleNotResolvable &on_not_resolvable){
    Poco::URI uri("https://terminologies.gfbio.org/api/terminologies/search");
    uri.addQueryParameter("query", name);
    uri.addQueryParameter("terminologies", terminology);

    Poco::Net::HTTPSClientSession session(uri.getHost(), uri.getPort(), context, session_ptr);
    Poco::Net::HTTPRequest request(Poco::Net::HTTPRequest::HTTP_GET, uri.getPathAndQuery(), Poco::Net::HTTPRequest::HTTP_1_1);
    Poco::Net::HTTPResponse response;

    session.sendRequest(request);
    std::istream& respStream = session.receiveResponse(response);
    Json::Value response_json;

    if (response.getStatus() == Poco::Net::HTTPResponse::HTTP_OK)
    {
        respStream >> response_json;
    } else {
        response_json = Json::Value::null;
    }

    std::string not_resolved = (on_not_resolvable == EMPTY) ? "" : name;

    if (response_json.isNull())
    {
        return not_resolved;
    } else
    {
        Json::Value results = response_json["results"];

        Json::Value val = results[0].get(key, not_resolved);
        if(val.isArray())
            return val[0].asString();
        else
            return val.asString();
    }
}

std::string Terminology::resolveSingleNameSetSessionPtr(Poco::Net::Context::Ptr &context,
                                                        Poco::Net::Session::Ptr *session_ptr,
                                                        const std::string &name,
                                                        const std::string &terminology,
                                                        const std::string &key,
                                                        HandleNotResolvable on_not_resolvable) {

    Poco::URI uri("https://terminologies.gfbio.org/api/terminologies/search");
    uri.addQueryParameter("query", name);
    uri.addQueryParameter("terminologies", terminology);

    Poco::Net::HTTPSClientSession session(uri.getHost(), uri.getPort(), context);
    Poco::Net::HTTPRequest request(Poco::Net::HTTPRequest::HTTP_GET, uri.getPathAndQuery(), Poco::Net::HTTPRequest::HTTP_1_1);
    Poco::Net::HTTPResponse response;

    session.sendRequest(request);

    *session_ptr = session.sslSession();

    std::istream& respStream = session.receiveResponse(response);
    Json::Value response_json;

    if (response.getStatus() == Poco::Net::HTTPResponse::HTTP_OK)
    {
        respStream >> response_json;
    } else {
        response_json = Json::Value::null;
    }

    std::string not_resolved = (on_not_resolvable == EMPTY) ? "" : name;

    if (response_json.isNull())
    {
        return not_resolved;
    } else
    {
        Json::Value results = response_json["results"];

        Json::Value val = results[0].get(key, not_resolved);
        if(val.isArray())
            return val[0].asString();
        else
            return val.asString();
    }
}

std::string Terminology::resolveSingle(const std::string &name,
                                       const std::string &terminology,
                                       HandleNotResolvable onNotResolvable)
{
    Poco::URI uri("https://terminologies.gfbio.org/api/terminologies/search");
    uri.addQueryParameter("query", name);
    uri.addQueryParameter("terminologies", terminology);

    Poco::Net::HTTPSClientSession session(uri.getHost(), uri.getPort());
    Poco::Net::HTTPRequest request(Poco::Net::HTTPRequest::HTTP_GET, uri.getPathAndQuery(), Poco::Net::HTTPRequest::HTTP_1_1);
    Poco::Net::HTTPResponse response;

    session.sendRequest(request);
    std::istream& respStream = session.receiveResponse(response);

    Json::Value response_json;
    if (response.getStatus() == Poco::Net::HTTPResponse::HTTP_OK)
    {
        respStream >> response_json;
    } else {
        response_json = Json::Value::null;
    }

    std::string not_resolved = (onNotResolvable == EMPTY) ? "" : name;

    if (response_json.isNull())
    {
        return not_resolved;
    } else
    {
        Json::Value results = response_json["results"];
        Json::Value first = results[0];
        return first.get("label", not_resolved).asString();
    }
}