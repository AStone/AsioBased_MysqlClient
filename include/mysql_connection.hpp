#pragma once

#include <mariadb/mysql.h>
#include <mariadb/mysqld_error.h>

#include <asio.hpp>
#include <iostream>
#include <memory>
#include <string>

#include "mysql_result.hpp"
namespace db {
class Channel;  // tcp connection used for read and write
enum class ConnectStatus { None = 0,
                           Connecting,
                           SettingCharacterSet,
                           Ok,
                           Bad };

struct ConnectionInfo {
    std::string user;
    std::string host;
    std::string port;
    std::string password;
    std::string database;
    std::string character_set;
    ConnectionInfo(const std::string_view& u,
                   const std::string_view& h,
                   const std::string_view& po,
                   const std::string_view& pw,
                   const std::string_view& db,
                   const std::string_view& cs)
        : user(u), host(h), port(po), password(pw), database(db), character_set(cs) {}
};
class MysqlConnection;
using MysqlConnectionPtr = std::shared_ptr<MysqlConnection>;
using ResultPtrCallback = std::function<void(const MysqlResultPtr&)>;
using ExceptPtrCallback = std::function<void(std::exception_ptr)>;
using ConnectionCallback = std::function<void(const MysqlConnectionPtr&)>;
struct SqlCmd {
    std::string_view sql_;
    ResultPtrCallback result_callback_;
    ExceptPtrCallback exception_callback_;
    SqlCmd(std::string_view&& sql,
           ResultPtrCallback&& cb,
           ExceptPtrCallback&& exceptCb)
        : sql_(std::move(sql)),
          result_callback_(std::move(cb)),
          exception_callback_(std::move(exceptCb)) {
    }
};
class MysqlConnection : public std::enable_shared_from_this<MysqlConnection> {
   private:
    enum class ExecStatus { None = 0,
                            RealQuery,
                            StoreResult,
                            NextResult };
    bool is_working_ = false;
    std::shared_ptr<MYSQL> mysql_ptr_;
    asio::io_context& io_context_;
    asio::ip::tcp::socket socket_;
    ConnectionInfo conn_info_;
    ConnectStatus conn_status_{ConnectStatus::None};
    ExecStatus exec_status_{ExecStatus::None};

    std::string sql_;
    ResultPtrCallback result_callback_;
    ExceptPtrCallback ec_callback_;
    ConnectionCallback connected_callback_{[](const MysqlConnectionPtr&) {}};
    ConnectionCallback closed_callback_{[](const MysqlConnectionPtr&) {}};
    std::function<void()> complete_callback_;

    unsigned int reconnect_{1};
    std::thread::id thread_id_;

   public:
    MysqlConnection(asio::io_context& io_context, const ConnectionInfo& conn_info)
        : mysql_ptr_(std::shared_ptr<MYSQL>(new MYSQL,
                                            [](MYSQL* p) {
                                                mysql_close(p);
                                                delete p;
                                            })),
          io_context_(io_context),
          socket_(io_context_),
          conn_info_(conn_info) {
        mysql_init(mysql_ptr_.get());
        mysql_options(mysql_ptr_.get(), MYSQL_OPT_NONBLOCK, nullptr);
    }
    ~MysqlConnection() { std::cout << "connection disconnected\n"; }

    void execute_sql(std::string_view sql, ResultPtrCallback&& result_callback, ExceptPtrCallback&& ec_callback) {
        result_callback_ = std::move(result_callback);
        ec_callback_ = std::move(ec_callback);
        sql_ = std::string(sql);
        is_working_ = true;
        asio::post(io_context_, [weak_this = std::weak_ptr(shared_from_this())]() {
            auto this_ptr = weak_this.lock();
            if (!this_ptr) return;
            asio::co_spawn(this_ptr->io_context_, this_ptr->async_execute(), asio::detached);
        });
    }
    void set_connected_callback(ConnectionCallback&& callback) { connected_callback_ = callback; }
    void set_closed_callback(ConnectionCallback&& callback) { closed_callback_ = callback; }
    void set_complete_callback(std::function<void()>&& callback) { complete_callback_ = callback; }
    bool is_working() { return is_working_; }
    ConnectStatus status() { return conn_status_; }
    asio::io_context& io_context() { return io_context_; }

    void handle_connect() {
        asio::co_spawn(io_context_, async_connect(), asio::detached);
    }
    void handle_close() {
        if (closed_callback_) {
            closed_callback_(shared_from_this());
        }
    }

   private:
    asio::awaitable<bool> async_connect();
    asio::awaitable<void> async_execute();
    void handle_error();
};

inline asio::awaitable<void> MysqlConnection::async_execute() {
    int err = 0;
    int wait_status = 0;
    wait_status = mysql_real_query_start(&err, mysql_ptr_.get(), sql_.data(), sql_.length());
    exec_status_ = ExecStatus::RealQuery;
    while (wait_status) {
        co_await socket_.async_wait(asio::ip::tcp::socket::wait_read, asio::use_awaitable);
        wait_status = mysql_real_query_cont(&err, mysql_ptr_.get(), MYSQL_WAIT_READ);
    }
    if (err) {
        handle_error();
        co_return;
    }
    for (;;) {
        MYSQL_RES* result;
        exec_status_ = ExecStatus::StoreResult;
        wait_status = mysql_store_result_start(&result, mysql_ptr_.get());
        while (wait_status) {
            co_await socket_.async_wait(asio::ip::tcp::socket::wait_read, asio::use_awaitable);
            wait_status = mysql_store_result_cont(&result, mysql_ptr_.get(), MYSQL_WAIT_READ);
        }
        if (!result && mysql_errno(mysql_ptr_.get())) {
            handle_error();
            co_return;
        }
        auto result_ptr = std::shared_ptr<MYSQL_RES>(result, [](MYSQL_RES* r) { mysql_free_result(r); });
        auto query_result_ptr = std::make_shared<MysqlResult>(result_ptr, mysql_affected_rows(mysql_ptr_.get()), mysql_insert_id(mysql_ptr_.get()));
        if (result_callback_) {
            result_callback_(query_result_ptr);
        }

        if (!mysql_more_results(mysql_ptr_.get())) {
            {
                ec_callback_ = nullptr;
                result_callback_ = nullptr;
                is_working_ = false;
                if (complete_callback_) {
                    complete_callback_();
                }
                co_return;
            }
        } else {
            exec_status_ = ExecStatus::NextResult;
            wait_status = mysql_next_result_start(&err, mysql_ptr_.get());
            while (wait_status) {
                co_await socket_.async_wait(asio::ip::tcp::socket::wait_read, asio::use_awaitable);
                wait_status = mysql_next_result_cont(&err, mysql_ptr_.get(), MYSQL_WAIT_READ);
            }
            if (wait_status == 0) {
                if (err) {
                    handle_error();
                    co_return;
                }
            }
        }
    }
}
inline asio::awaitable<bool> MysqlConnection::async_connect() {
    int wait_status = 0;
    MYSQL* ret;
    conn_status_ = ConnectStatus::Connecting;
    wait_status = mysql_real_connect_start(&ret, mysql_ptr_.get(), conn_info_.host.c_str(), conn_info_.user.c_str(), conn_info_.password.c_str(),
                                           conn_info_.database.c_str(), atol(conn_info_.port.c_str()), nullptr, 0);
    auto fd = mysql_get_socket(mysql_ptr_.get());
    if (fd < 0) {
        exit(1);
    }
    socket_.assign(asio::ip::tcp::v4(), fd);
    while (wait_status) {
        co_await socket_.async_wait(asio::ip::tcp::socket::wait_read, asio::use_awaitable);
        wait_status = mysql_real_connect_cont(&ret, mysql_ptr_.get(), MYSQL_WAIT_READ);
    }
    auto errorNo = mysql_errno(mysql_ptr_.get());
    if (!ret && errno) {
        handle_error();
        co_return false;
    }
    if (!conn_info_.character_set.empty()) {
        int err = 0;
        wait_status = mysql_set_character_set_start(&err, mysql_ptr_.get(), conn_info_.character_set.c_str());
        while (wait_status) {
            co_await socket_.async_wait(asio::ip::tcp::socket::wait_read, asio::use_awaitable);
            wait_status = mysql_set_character_set_cont(&err, mysql_ptr_.get(), MYSQL_WAIT_READ);
        }
        if (err) {
            handle_error();
            co_return false;
        }
    }
    conn_status_ = ConnectStatus::Ok;
    if (connected_callback_) {
        connected_callback_(shared_from_this());
    }
    co_return true;
}
inline void MysqlConnection::handle_error() {
    exec_status_ = ExecStatus::None;
    auto errorNo = mysql_errno(mysql_ptr_.get());
    if (is_working_) {
        auto ec_ptr = std::make_exception_ptr(std::exception(std::runtime_error(mysql_error(mysql_ptr_.get()))));
        if (ec_callback_) {
            ec_callback_(ec_ptr);
        }
        ec_callback_ = nullptr;
        result_callback_ = nullptr;
        is_working_ = false;
        handle_close();
    }
}
}  // namespace db
