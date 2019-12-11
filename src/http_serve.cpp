#include <memory>
#include <thread>
#include <iostream>
#include <mutex>
#include <queue>

#include "threads/locks.h"
#include "server_http.hpp"

#include "logger.h"
#include "http_serve.h"
#include "http_cli.h"
#include "rpc.h"

using namespace std::string_literals;

namespace openset
{
    namespace http = SimpleWeb;
}

namespace openset
{
    namespace globals
    {
        std::shared_ptr<asio::io_service> global_io_service;
    }
}

namespace openset::web
{

    /* MakeMessage - magic
     *
     * This anonomizes the request objects (which might be HTTP or HTTPS objects)
     *
     * by making a message and attaching a callback (cb) with closures to the correct objects.
     */
    template<typename TRes, typename TReq>
    std::shared_ptr<Message> MakeMessage(TRes response, TReq request)
    {
        auto queryParts = request->parse_query_string();
        auto length = request->content.size();
        auto data = static_cast<char*>(PoolMem::getPool().getPtr(length));
        request->content.read(data, length);
        request->content.clear();

        auto reply = [request, response](http::StatusCode status, const char* data, size_t length)
        {
            http::CaseInsensitiveMultimap header;
            header.emplace("Content-Length", to_string(length));
            header.emplace("Content-Type", "application/json");
            header.emplace("Access-Control-Allow-Origin", "*");
            response->write(status, header);

            if (data)
                response->write(data, length);
        };

        return make_shared<Message>(request->header, queryParts, request->method, request->path, request->query_string, data, length, reply);
    }

    void webWorker::runner()
    {
        while (true)
        {
            // wait on accept handler

            std::shared_ptr<Message> message;

            if (queryWorker)
            {
                // wait on a job to appear, verify it's there, and run it.
                {
                    unique_lock<std::mutex> waiter(server->queryReadyLock);
                    if (server->queryMessagesQueued == 0 || server->runningQueries >= 3)
                        server->queryMessageReady.wait(waiter,
                            [&]()
                    {
                        return static_cast<int32_t>(server->queryMessagesQueued) != 0 && server->runningQueries < 3;
                    });

                    message = server->getQueuedQueryMessage();
                    if (!message)
                        continue;
                }

                ++server->jobsRun;
                ++server->runningQueries;
                openset::comms::Dispatch(message);
                --server->runningQueries;

            }
            else
            {
                {
                    // wait on a job to appear, verify it's there, and run it.
                    unique_lock<std::mutex> waiter(server->otherReadyLock);
                    if (server->otherMessagesQueued == 0)
                        server->otherMessageReady.wait(waiter,
                            [&]()
                    {
                        return static_cast<int32_t>(server->otherMessagesQueued) != 0;
                    });

                    message = server->getQueuedOtherMessage();
                    if (!message)
                        continue;
                }

                ++server->jobsRun;
                openset::comms::Dispatch(message);

            } // unlock out of scope

        }
    }

    void HttpServe::queueQueryMessage(std::shared_ptr<Message> message)
    {
        csLock lock(messagesLock);
        ++queryMessagesQueued;
        queryMessages.emplace(message);
        queryMessageReady.notify_one();
    }

    void HttpServe::queueOtherMessage(std::shared_ptr<Message> message)
    {
        csLock lock(messagesLock);
        ++otherMessagesQueued;
        otherMessages.emplace(message);
        otherMessageReady.notify_one();
    }

    std::shared_ptr<Message> HttpServe::getQueuedOtherMessage()
    {
        csLock lock(messagesLock);

        if (otherMessages.empty())
            return nullptr;

        --otherMessagesQueued;

        auto result = otherMessages.front();
        otherMessages.pop();
        return result;
    }

    std::shared_ptr<Message> HttpServe::getQueuedQueryMessage()
    {
        csLock lock(messagesLock);

        if (queryMessages.empty())
            return nullptr;

        --queryMessagesQueued;

        auto result = queryMessages.front();
        queryMessages.pop();
        return result;
    }

    template<typename T>
    void HttpServe::mapEndpoints(T& server)
    {

        using SharedResponseT = std::shared_ptr<typename T::Response>;
        using SharedRequestT = std::shared_ptr<typename T::Request>;

        server.resource["^/v1/.*$"]["GET"] = [&](SharedResponseT response, SharedRequestT request) {
            if (request->path.find("/v1/query/") == 0 && request->query_string.find("fork=true") == -1)
                queueQueryMessage(std::move(MakeMessage(response, request)));
            else
                queueQueryMessage(std::move(MakeMessage(response, request)));
        };

        server.resource["^/v1/.*$"]["POST"] = [&](SharedResponseT response, SharedRequestT request) {
            if (request->path.find("/v1/query/") == 0 && request->query_string.find("fork=true") == -1)
                queueQueryMessage(std::move(MakeMessage(response, request)));
            else
                queueOtherMessage(std::move(MakeMessage(response, request)));
        };

        server.resource["^/v1/.*$"]["PUT"] = [&](SharedResponseT response, SharedRequestT request) {
            queueOtherMessage(std::move(MakeMessage(response, request)));
        };

        server.resource["^/v1/.*$"]["DELETE"] = [&](SharedResponseT response, SharedRequestT request) {
            queueOtherMessage(std::move(MakeMessage(response, request)));
        };

        server.resource["^/ping$"]["GET"] = [&](SharedResponseT response, SharedRequestT request) {
            http::CaseInsensitiveMultimap header;
            header.emplace("Content-Type", "application/json");
            header.emplace("Access-Control-Allow-Origin", "*");

            response->write("{\"pong\":true}", header);
        };
    }

    void HttpServe::makeWorkers()
    {
        otherWorkers.reserve(32);
        queryWorkers.reserve(8);
        threads.reserve(40);

        for (auto i = 0; i < 32; i++)
        {
            otherWorkers.emplace_back(std::make_shared<webWorker>(this, i, false));
            threads.emplace_back(thread(&webWorker::runner, otherWorkers[i]));
        }

        for (auto i = 0; i < 8; i++)
        {
            queryWorkers.emplace_back(std::make_shared<webWorker>(this, i, true));
            threads.emplace_back(thread(&webWorker::runner, queryWorkers[i]));
        }

        Logger::get().info(" HTTP REST server created.");

        // detach these threads, let them do their thing in the background
        for (auto& thread : threads)
            thread.detach();
    }

    void HttpServe::serve(const std::string& ip, const int port)
    {
        using HttpServer = SimpleWeb::Server<SimpleWeb::HTTP>;

        HttpServer server;

        server.config.port = port;
        server.config.address = ip;
        server.config.reuse_address = false; // we want an error if this is already going

        mapEndpoints(server);
        makeWorkers();

        server.default_resource["GET"] = [](std::shared_ptr<HttpServer::Response> response, std::shared_ptr<HttpServer::Request> request) {
            response->write("{\"error\":\"unknown request\""s);
        };

        server.on_error = [](shared_ptr<HttpServer::Request> /*request*/, const SimpleWeb::error_code & /*ec*/) {
            // Handle errors here
            // Note that connection timeouts will also call this handle with ec set to SimpleWeb::errc::operation_canceled
          };


        // Start server
        thread server_thread([&server]() {
            // Start server
            server.start();
        });

        Logger::get().info("HTTP REST server listening on "s + ip + ":"s + to_string(port) + "."s);

        ThreadSleep(250);

        // wait here forever
        server_thread.join();
    }
}