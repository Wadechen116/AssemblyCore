#include "qt_dispatcher.h"

#include <QMetaObject>

namespace acide {

void QtDispatcher::post(std::function<void()> fn) {
    QMetaObject::invokeMethod(this, [fn = std::move(fn)]() { fn(); }, Qt::QueuedConnection);
}

}  // namespace acide
