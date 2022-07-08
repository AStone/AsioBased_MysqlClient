#ifndef __IO_CONTEXT_POOL_HPP__
#define __IO_CONTEXT_POOL_HPP__
#include <asio/io_context.hpp>
#include <asio/signal_set.hpp>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

class SigleIOThread {
   private:
    std::shared_ptr<std::thread> thread_;
    std::shared_ptr<asio::io_context> io_context_;
    std::shared_ptr<asio::executor_work_guard<asio::io_context::executor_type>> work_guard_;
    std::shared_ptr<asio::signal_set> signal_;
    std::thread::id thread_id_;

   public:
    SigleIOThread() = default;
    ~SigleIOThread() {
        if (thread_->joinable()) {
            thread_->join();
        }
    }
    void init() {
        io_context_ = std::make_shared<asio::io_context>(1);
        work_guard_ = std::make_shared<asio::executor_work_guard<asio::io_context::executor_type>>(asio::make_work_guard(*io_context_));
        signal_ = std::make_shared<asio::signal_set>(*io_context_, SIGINT, SIGTERM);
    }
    void run() {
        signal_->async_wait([this](auto, auto) { this->stop(); });
        thread_ = std::make_shared<std::thread>([this]() {
            this->thread_id_ = std::this_thread::get_id();
            this->io_context_->run();
        });
    }
    void join() { thread_->join(); }
    void stop() { io_context_->stop(); }
    asio::io_context& get_io_context() { return *io_context_; }
};
class MultiIOThreads {
   private:
    std::vector<std::unique_ptr<SigleIOThread>> io_workers_;
    std::size_t current_io_index_ = {0};

   public:
    void init(std::size_t num) {
        for (std::size_t i = 0; i < num; ++i) {
            auto io = std::make_unique<SigleIOThread>();
            io->init();
            io_workers_.emplace_back(std::move(io));
        }
    }
    void run() {
        for (auto& io : io_workers_) {
            io->run();
        }
    }
    void join() {
        for (auto& io : io_workers_) {
            io->join();
        }
    }
    void stop() {
        for (auto& io : io_workers_) {
            io->stop();
        }
    }
    asio::io_context& get_io_context() {
        auto& io = io_workers_.at(current_io_index_);
        ++current_io_index_;
        if (current_io_index_ == io_workers_.size()) {
            current_io_index_ = 0;
        }
        return io->get_io_context();
    }
};
template <class PoolPolicy>
class IOContextPoolBase : public PoolPolicy {
   public:
    IOContextPoolBase(std::size_t size) { PoolPolicy::init(size); }
};
using IOContextPool = IOContextPoolBase<MultiIOThreads>;
using IoContextPool = IOContextPoolBase<MultiIOThreads>;
#endif  //__IO_CONTEXT_POOL_HPP__