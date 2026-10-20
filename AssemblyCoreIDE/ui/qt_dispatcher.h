#pragma once
// IDispatcher on top of the Qt event loop.
#include <QObject>

#include "dispatcher.h"

namespace acide {

class QtDispatcher : public QObject, public IDispatcher {
public:
    explicit QtDispatcher(QObject* parent = nullptr) : QObject(parent) {}
    void post(std::function<void()> fn) override;
};

}  // namespace acide
