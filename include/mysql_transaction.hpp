#include <list>

#include "mysql_connection.hpp"
namespace db {
class MysqlTransaction;
using MysqlTransactionPtr = std::shared_ptr<MysqlTransaction>;
class MysqlTransaction : public std::enable_shared_from_this<MysqlTransaction> {
   private:
    MysqlConnectionPtr conn_ptr_;
    asio::io_context& io_context_;
    std::function<void(bool)> commit_callback_;
    std::function<void()> usedup_callback_;

    struct SqlCmd {
        std::string_view sql_;
        ResultPtrCallback result_callback_;
        ExceptPtrCallback ec_callback_;
        bool is_rollback_cmd_ = false;
        std::shared_ptr<MysqlTransaction> this_ptr_;
    };
    using SqlCmdPtr = std::shared_ptr<SqlCmd>;
    std::list<SqlCmdPtr> sqlCmdBuffer_;

    bool is_commited_rollback = false;
    bool is_working_ = false;

   public:
    MysqlTransaction(const MysqlConnectionPtr& conn_ptr, std::function<void(bool)>&& commit_callback,
                     std::function<void()>&& usedup_callback)
        : conn_ptr_(conn_ptr), io_context_(conn_ptr_->io_context()), commit_callback_(commit_callback), usedup_callback_(usedup_callback) {
    }
    ~MysqlTransaction();
    void set_commit_callback(const std::function<void(bool)>& commitCallback) { commit_callback_ = commitCallback; }
    bool is_connection_available() { return conn_ptr_->status() == ConnectStatus::Ok; }
    void execute_sql(std::string_view&& sql, ResultPtrCallback&& rcb, ExceptPtrCallback&& ecb);
    void do_begin();

   private:
    void execute_new_task();
    void roll_back();
};
MysqlTransaction::~MysqlTransaction() {
    assert(sqlCmdBuffer_.empty());
    if (!is_commited_rollback) {
        asio::post(io_context_, [conn = conn_ptr_,
                                 ucb = std::move(usedup_callback_),
                                 commitCb = std::move(commit_callback_)]() {
            conn->set_complete_callback([ucb = std::move(ucb)]() {
                if (ucb)
                    ucb();
            });
            conn->execute_sql(
                "commit",
                [commitCb](const MysqlResultPtr&) {
                    if (commitCb) {
                        commitCb(true);
                    }
                },
                [commitCb](const std::exception_ptr& ePtr) {
                    if (commitCb) {
                        try {
                            std::rethrow_exception(ePtr);
                        } catch (const std::exception& e) {
                            commitCb(false);
                        }
                    }
                });
        });
    } else {
        if (usedup_callback_) {
            usedup_callback_();
        }
    }
}
inline void MysqlTransaction::do_begin() {
    asio::post(io_context_,
               [this_ptr = shared_from_this()]() {
                   std::weak_ptr<MysqlTransaction> weak_this(this_ptr);
                   this_ptr->conn_ptr_->set_complete_callback([weak_this]() {
                       auto this_ptr = weak_this.lock();
                       if (!this_ptr)
                           return;
                       this_ptr->execute_new_task();
                   });
                   this_ptr->is_working_ = true;
                   this_ptr->conn_ptr_->execute_sql(
                       "begin", [](const MysqlResultPtr& ptr) {}, [this_ptr](const std::exception_ptr& e) { this_ptr->is_commited_rollback = true; });
               });
}
inline void MysqlTransaction::roll_back() {
    auto thisPtr = shared_from_this();

    asio::post(io_context_, [thisPtr]() {
        if (thisPtr->is_commited_rollback)
            return;
        if (thisPtr->is_working_) {
            // push sql cmd to buffer;
            auto cmdPtr = std::make_shared<SqlCmd>();
            cmdPtr->sql_ = "rollback";
            cmdPtr->result_callback_ = [thisPtr](const MysqlResultPtr&) {
                thisPtr->is_commited_rollback = true;
            };
            cmdPtr->ec_callback_ = [thisPtr](const std::exception_ptr&) {
                // clearupCb();
                thisPtr->is_commited_rollback = true;
            };
            cmdPtr->is_rollback_cmd_ = true;
            // Rollback cmd should be executed firstly, so we push it in front
            // of the list
            thisPtr->sqlCmdBuffer_.push_front(std::move(cmdPtr));
            return;
        }
        thisPtr->is_working_ = true;
        thisPtr->conn_ptr_->execute_sql(
            "rollback",
            [thisPtr](const MysqlResultPtr&) {
                thisPtr->is_commited_rollback = true;
                // clearupCb();
            },
            [thisPtr](const std::exception_ptr&) {
                thisPtr->is_commited_rollback = true;
            });
    });
}
inline void MysqlTransaction::execute_new_task() {
    assert(is_working_);
    if (!is_commited_rollback) {
        auto this_ptr = shared_from_this();
        if (!sqlCmdBuffer_.empty()) {
            auto cmd = std::move(sqlCmdBuffer_.front());
            sqlCmdBuffer_.pop_front();
            conn_ptr_->execute_sql(
                std::move(cmd->sql_),
                [rcb = std::move(cmd->result_callback_), cmd, this_ptr](
                    const MysqlResultPtr& result_ptr) {
                    if (cmd->is_rollback_cmd_) {
                        this_ptr->is_commited_rollback = true;
                    }
                    if (rcb) {
                        rcb(result_ptr);
                    }
                },
                [cmd, this_ptr](const std::exception_ptr& ec_ptr) {
                    if (!cmd->is_rollback_cmd_) {
                        this_ptr->roll_back();
                    } else {
                        this_ptr->is_commited_rollback = true;
                    }
                    if (cmd->ec_callback_) {
                        cmd->ec_callback_(ec_ptr);
                    }
                });
        } else {
            is_working_ = false;
        }
    } else {
        is_working_ = false;
        if (!sqlCmdBuffer_.empty()) {
            auto ec_ptr = std::make_exception_ptr(std::exception(std::runtime_error("transaction been rollback")));
            for (auto& cmd : sqlCmdBuffer_) {
                if (cmd->ec_callback_) {
                    cmd->ec_callback_(ec_ptr);
                }
            }
            sqlCmdBuffer_.clear();
        } else {
            if (usedup_callback_) {
                usedup_callback_();
                usedup_callback_ = std::function<void()>();
            }
        }
    }
}
inline void MysqlTransaction::execute_sql(std::string_view&& sql, ResultPtrCallback&& rcb, ExceptPtrCallback&& ecb) {
    auto thisPtr = shared_from_this();
    if (!is_commited_rollback) {
        auto thisPtr = shared_from_this();
        if (!is_working_) {
            is_working_ = true;
            conn_ptr_->execute_sql(std::move(sql),
                                   std::move(rcb),
                                   [ecb,
                                    thisPtr](const std::exception_ptr& ePtr) {
                                       thisPtr->roll_back();
                                       if (ecb)
                                           ecb(ePtr);
                                   });
        } else {
            // push sql cmd to buffer;
            auto cmdPtr = std::make_shared<SqlCmd>();
            cmdPtr->sql_ = std::move(sql);
            cmdPtr->result_callback_ = std::move(rcb);
            cmdPtr->ec_callback_ = std::move(ecb);
            cmdPtr->this_ptr_ = thisPtr;
            thisPtr->sqlCmdBuffer_.push_back(std::move(cmdPtr));
        }
    } else {
        // The transaction has been rolled back;
        auto ec_ptr = std::make_exception_ptr(std::exception(std::runtime_error("transaction been rollback")));
        ecb(ec_ptr);
    }
}

}  // namespace db