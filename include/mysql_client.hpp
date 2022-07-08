#pragma once

#include <deque>
#include <list>
#include <memory>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>

#include "io_context_pool.hpp"
#include "mysql_connection_pool.hpp"

using namespace std::chrono_literals;
namespace db {
class MysqlClient : public std::enable_shared_from_this<MysqlClient> {
   private:
    IOContextPool io_context_;
    ConnectionInfo conn_info_;
    MysqlPoolPtr mysql_pool_ptr_;

   public:
    MysqlClient(const ConnectionInfo& conn_info, const std::size_t min_conn_num, const std::size_t max_conn_num)
        : io_context_(1),
          conn_info_(conn_info),
          mysql_pool_ptr_(std::make_shared<MysqlConnectionPool>(io_context_.get_io_context(), min_conn_num, max_conn_num, conn_info_)) {}
    void init() {
        io_context_.run();
        mysql_pool_ptr_->init();
        std::this_thread::sleep_for(1s);
    }
    void join() { io_context_.join(); }
    void close_all();
    void execute(const char* sql) { mysql_pool_ptr_->execute_sql(sql); }
    void query(const char* sql, ResultPtrCallback&& result_callback, ExceptPtrCallback ec_callback = nullptr) {
        mysql_pool_ptr_->execute_sql(sql, std::move(result_callback), std::move(ec_callback));
    }
    MysqlTransactionPtr new_transaction(std::function<void(bool)>&& commit_callback) {
        std::promise<MysqlTransactionPtr> pro;
        auto f = pro.get_future();
        mysql_pool_ptr_->new_transaction_async([&pro](const MysqlTransactionPtr& trans) {
            pro.set_value(trans);
        });
        auto trans = f.get();
        if (!trans) {
            // throw error......
            return nullptr;
        }
        trans->set_commit_callback(commit_callback);
        return trans;
    }
};
}  // namespace db
