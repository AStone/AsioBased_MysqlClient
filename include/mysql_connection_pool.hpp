#pragma once

#include <asio.hpp>
#include <chrono>
#include <deque>
#include <iostream>
#include <list>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>

#include "mysql_connection.hpp"
#include "mysql_transaction.hpp"
namespace db {
constexpr int max_sql_buffer = 200000;
class MysqlConnectionPool;
using MysqlPoolPtr = std::shared_ptr<MysqlConnectionPool>;
class MysqlConnectionPool : public std::enable_shared_from_this<MysqlConnectionPool> {
    asio::io_context& io_context_;
    std::size_t min_size_;
    std::size_t max_size_;
    ConnectionInfo conn_info_;

    mutable std::mutex conn_mutex_;
    std::unordered_set<MysqlConnectionPtr> connections_;
    std::unordered_set<MysqlConnectionPtr> ready_connections_;
    std::unordered_set<MysqlConnectionPtr> busy_connections_;

    std::deque<std::shared_ptr<SqlCmd>> sqlCmd_buffer_;
    using TransactionPtrCallback = std::function<void(const MysqlTransactionPtr&)>;
    std::list<std::shared_ptr<TransactionPtrCallback>> trans_callbacks_;
    bool is_busy_ = false;
    std::thread::id thread_id_;

   public:
    MysqlConnectionPool(asio::io_context& io, std::size_t min_size, std::size_t max_size, const ConnectionInfo& conn_info)
        : io_context_(io), min_size_(min_size), max_size_(max_size), conn_info_(conn_info) {}
    void init() {
        for (size_t i = 0; i < min_size_; ++i) {
            connections_.insert(create_connection());
        }
    }
    void close_all() {
        for (auto& conn : connections_) {
            conn->handle_close();
        }
        connections_.clear();
        busy_connections_.clear();
        ready_connections_.clear();
    }
    void execute_sql(
        const char* sql,
        ResultPtrCallback&& result_callback = nullptr,
        ExceptPtrCallback&& except_callback = nullptr) {
        MysqlConnectionPtr conn = nullptr;
        {
            std::lock_guard<std::mutex> locker(conn_mutex_);
            if (ready_connections_.empty()) {
                if (sqlCmd_buffer_.size() > max_sql_buffer) {
                    is_busy_ = true;
                } else {
                    auto cmd_ptr =
                        std::make_shared<SqlCmd>(std::string_view(sql), std::move(result_callback), std::move(except_callback));
                    sqlCmd_buffer_.push_back((cmd_ptr));
                    if (connections_.size() < max_size_) {
                        connections_.insert(create_connection());
                    }
                }
            } else {
                auto iter = ready_connections_.begin();
                busy_connections_.insert(*iter);
                conn = *iter;
                ready_connections_.erase(iter);
            }
        }
        if (is_busy_) {
        }
        if (conn) {
            execute_sql(conn, sql, std::move(result_callback), std::move(except_callback));
        } else {
        }
    }
    void new_transaction_async(TransactionPtrCallback&& callback) {
        MysqlConnectionPtr conn_ptr = nullptr;
        {
            std::lock_guard<std::mutex> locker(conn_mutex_);
            if (!ready_connections_.empty()) {
                auto iter = ready_connections_.begin();
                busy_connections_.insert(*iter);
                conn_ptr = *iter;
                ready_connections_.erase(iter);
            } else {
                auto callback_ptr = std::make_shared<TransactionPtrCallback>(callback);
                trans_callbacks_.push_back(callback_ptr);
            }
        }
        if (conn_ptr) {
            begin_trans(conn_ptr, std::move(callback));
        }
    }

   private:
    void execute_sql(const MysqlConnectionPtr& conn, std::string_view sql, ResultPtrCallback&& result_callback, ExceptPtrCallback&& except_callback) {
        conn->execute_sql(std::move(sql), std::move(result_callback), std::move(except_callback));
    };
    MysqlConnectionPtr create_connection();

    void handle_new_task(const MysqlConnectionPtr& conn);

    void begin_trans(const MysqlConnectionPtr& conn, TransactionPtrCallback&& callback);
};
inline MysqlConnectionPtr MysqlConnectionPool::create_connection() {
    auto conn_ptr = std::make_shared<MysqlConnection>(io_context_, conn_info_);
    std::weak_ptr<MysqlConnectionPool> weakPtr = shared_from_this();
    conn_ptr->set_closed_callback([weakPtr](const MysqlConnectionPtr& close_ptr) {
        auto this_ptr = weakPtr.lock();
        if (this_ptr == nullptr)
            return;
        {
            std::lock_guard<std::mutex> locker(this_ptr->conn_mutex_);
            this_ptr->ready_connections_.erase(close_ptr);
            this_ptr->busy_connections_.erase(close_ptr);
            this_ptr->connections_.erase(close_ptr);
        }
    });
    conn_ptr->set_connected_callback([weakPtr](const MysqlConnectionPtr& create_ptr) {
        auto this_ptr = weakPtr.lock();
        if (this_ptr == nullptr)
            return;
        {
            std::lock_guard<std::mutex> locker(this_ptr->conn_mutex_);
            this_ptr->busy_connections_.insert(create_ptr);
        }
        this_ptr->handle_new_task(create_ptr);
    });

    std::weak_ptr<MysqlConnection> weakConn = conn_ptr;
    conn_ptr->set_complete_callback([weakPtr, weakConn]() {
        auto this_ptr = weakPtr.lock();
        if (this_ptr == nullptr)
            return;
        auto conn_ptr = weakConn.lock();
        if (conn_ptr == nullptr)
            return;
        this_ptr->handle_new_task(conn_ptr);
    });
    conn_ptr->handle_connect();
    return conn_ptr;
}
inline void MysqlConnectionPool::handle_new_task(const MysqlConnectionPtr& conn) {
    std::shared_ptr<SqlCmd> sql_cmd = nullptr;
    TransactionPtrCallback trans_callback = nullptr;
    bool is_extra = 0;
    {
        std::lock_guard<std::mutex> locker(conn_mutex_);
        if (!sqlCmd_buffer_.empty()) {
            sql_cmd = sqlCmd_buffer_.front();
            sqlCmd_buffer_.pop_front();
        } else if (!trans_callbacks_.empty()) {
            trans_callback = std::move(*(trans_callbacks_.front()));
            trans_callbacks_.pop_front();
        } else {
            if (connections_.size() > min_size_) {
                is_extra = true;
            } else {
                ready_connections_.insert(conn);
                busy_connections_.erase(conn);
            }
        }
    }
    if (sql_cmd) {
        execute_sql(conn, std::move(sql_cmd->sql_), std::move(sql_cmd->result_callback_), std::move(sql_cmd->exception_callback_));
    }
    if (trans_callback) {
        begin_trans(conn, std::move(trans_callback));
    } else {
        if (is_extra) {
            conn->handle_close();
        }
    }
}
inline void MysqlConnectionPool::begin_trans(const MysqlConnectionPtr& conn, TransactionPtrCallback&& callback) {
    std::weak_ptr<MysqlConnectionPool> weakThis = shared_from_this();
    auto trans = std::make_shared<MysqlTransaction>(conn,
                                                     std::function<void(bool)>(),
                                                     [weakThis, conn]() {
                                                         auto thisPtr = weakThis.lock();
                                                         if (!thisPtr)
                                                             return;
                                                         if (conn->status() == ConnectStatus::Bad) {
                                                             return;
                                                         }
                                                         {
                                                             std::lock_guard<std::mutex> guard(thisPtr->conn_mutex_);
                                                             if (thisPtr->connections_.find(conn) ==
                                                                 thisPtr->connections_.end()) {
                                                                 // connection is broken and removed
                                                                 //  assert(thisPtr->busyConnections_.find(conn) ==
                                                                 //             thisPtr->busyConnections_.end() &&
                                                                 //         thisPtr->readyConnections_.find(conn) ==
                                                                 //             thisPtr->readyConnections_.end());

                                                                 return;
                                                             }
                                                         }
                                                         asio::post(conn->io_context(), [weakThis, conn]() {
                                                             auto thisPtr = weakThis.lock();
                                                             if (!thisPtr)
                                                                 return;
                                                             std::weak_ptr<MysqlConnection> weakConn(conn);
                                                             conn->set_complete_callback([weakThis, weakConn]() {
                                                                 auto thisPtr = weakThis.lock();
                                                                 if (!thisPtr)
                                                                     return;
                                                                 auto connPtr = weakConn.lock();
                                                                 if (!connPtr)
                                                                     return;
                                                                 thisPtr->handle_new_task(connPtr);
                                                             });
                                                             thisPtr->handle_new_task(conn);
                                                         });
                                                     });
    trans->do_begin();
    asio::post(conn->io_context(),
               [callback = std::move(callback), trans]() { callback(trans); });
}
}  // namespace db

// namespace test