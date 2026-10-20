#pragma once
// How a background thread hands work back to the thread that owns the UI. The services never know which toolkit is behind it.
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>

namespace acide {

class IDispatcher {
public:
    virtual ~IDispatcher() {}
    virtual void post(std::function<void()> fn) = 0;          // callable from any thread; fn runs on the owner thread
};

// Runs the function right away on the calling thread (single-threaded tools and tests).
class ImmediateDispatcher : public IDispatcher {
public:
    void post(std::function<void()> fn) override { fn(); }
};

// Collects the functions; the owner pumps them. What a test (or a console tool) uses to stand in for an event loop.
class QueueDispatcher : public IDispatcher {
public:
    void post(std::function<void()> fn) override {
        { std::lock_guard<std::mutex> l(m_); q_.push_back(std::move(fn)); }
        cv_.notify_all();
    }
    // Runs what is queued; returns how many ran.
    int runPending() {
        std::deque<std::function<void()>> take;
        { std::lock_guard<std::mutex> l(m_); take.swap(q_); }
        for (auto& f : take) f();
        return (int)take.size();
    }
    // Pumps until done() is true or the time is up.
    bool pumpUntil(const std::function<bool()>& done, int timeoutMs) {
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;) {
            runPending();
            if (done()) return true;
            std::unique_lock<std::mutex> l(m_);
            if (std::chrono::steady_clock::now() >= end) return false;
            cv_.wait_until(l, std::min(end, std::chrono::steady_clock::now() + std::chrono::milliseconds(20)));
        }
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> q_;
};

}  // namespace acide
