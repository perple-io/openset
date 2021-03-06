#pragma once

#include "shuttle.h"
#include "database.h"
#include "http_serve.h"
#include "rpc_global.h"

using namespace openset::async;
using namespace openset::db;

namespace openset::comms
{
    class RpcInsert
    {
        static void insertRetry(const openset::web::MessagePtr& message, const RpcMapping& matches, const int retryCount);
    public:
        // POST /v1/insert/{table}
        static void insert(const openset::web::MessagePtr& message, const RpcMapping& matches);
    };
}