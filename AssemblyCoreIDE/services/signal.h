#pragma once
// A minimal observer list. Not thread-safe on purpose: signals are emitted and connected on the UI thread (IDispatcher brings results back).
#include <functional>
#include <map>
#include <vector>
#include <utility>

namespace acide {

template <class... Args>
class Signal {
public:
    using Slot = std::function<void(Args...)>;
    int connect(Slot s) { int id = ++next_; slots_[id] = std::move(s); return id; }
    void disconnect(int id) { slots_.erase(id); }
    void fire(Args... args) const {
        auto copy = slots_;                         // a slot may disconnect itself
        for (auto& kv : copy) kv.second(args...);
    }
    size_t count() const { return slots_.size(); }

private:
    std::map<int, Slot> slots_;
    int next_ = 0;
};

}  // namespace acide

namespace acide {

// Disconnects everything connected through it when it is destroyed (a panel's lifetime is shorter than the signals it listens to).
class ConnectionBag {
public:
    ConnectionBag() = default;
    ConnectionBag(const ConnectionBag&) = delete;
    ConnectionBag& operator=(const ConnectionBag&) = delete;
    ~ConnectionBag() { for (auto& u : undo_) u(); }
    template <class Sig, class F>
    void add(Sig& sig, F&& f) {
        int id = sig.connect(std::forward<F>(f));
        Sig* p = &sig;
        undo_.push_back([p, id] { p->disconnect(id); });
    }

private:
    std::vector<std::function<void()>> undo_;
};

}  // namespace acide
