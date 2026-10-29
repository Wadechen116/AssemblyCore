#include "cursor_bar.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>

#include "text_util.h"

namespace acide {

namespace {

const char* kColour[2] = {"#ffb300", "#19c8e0"};                              // Main, Peri
const char* kName[2] = {"MAIN", "PERI"};
const char* kObject[2] = {"Main", "Peri"};

// What the bar keeps of one sequencer's chip.
struct Chip {
    QFrame* frame = nullptr;
    QLabel* badge = nullptr;
    QPushButton* go = nullptr;
    QPushButton* step = nullptr;
    QPushButton* over = nullptr;
};

}  // namespace

QToolBar* createCursorBar(AppContext& ctx, QWidget* parent) {
    auto* bar = new QToolBar(QCoreApplication::translate("CursorBar", "Cursors"), parent);
    bar->setObjectName("cursorBar");
    bar->setMovable(false);
    auto chips = std::make_shared<std::vector<Chip>>(2);
    auto conns = std::make_shared<ConnectionBag>();
    for (int d = 0; d < 2; d++) {
        Chip& c = (*chips)[(size_t)d];
        c.frame = new QFrame;
        c.frame->setObjectName(QString("cursor%1").arg(kObject[d]));
        auto* lay = new QHBoxLayout(c.frame);
        lay->setContentsMargins(4, 2, 4, 2);
        lay->setSpacing(6);
        c.badge = new QLabel(kName[d]);
        c.badge->setObjectName(QString("cursorBadge%1").arg(kObject[d]));
        lay->addWidget(c.badge);
        c.go = new QPushButton;
        c.go->setObjectName(QString("cursorGo%1").arg(kObject[d]));
        c.go->setFlat(true);
        c.go->setMinimumWidth(300);
        c.go->setStyleSheet("text-align:left; font-family:monospace;");
        c.go->setToolTip(QCoreApplication::translate("CursorBar", "Where this sequencer is about to execute. Click: Step acts on it, and its line is shown"));
        lay->addWidget(c.go, 1);
        c.step = new QPushButton(QCoreApplication::translate("CursorBar", "Step"));
        c.step->setObjectName(QString("cursorStep%1").arg(kObject[d]));
        c.step->setToolTip(QCoreApplication::translate("CursorBar", "Execute one instruction of this sequencer (what falls due before it, in the other one, runs first)"));
        c.over = new QPushButton(QCoreApplication::translate("CursorBar", "Over"));
        c.over->setObjectName(QString("cursorOver%1").arg(kObject[d]));
        c.over->setToolTip(QCoreApplication::translate("CursorBar", "Like Step, but a CALL is run to its return"));
        lay->addWidget(c.step);
        lay->addWidget(c.over);
        bar->addWidget(c.frame);
        if (d == 0) bar->addSeparator();
        const asmcore::Domain dom = d == 0 ? asmcore::Domain::Main : asmcore::Domain::Peri;
        QObject::connect(c.go, &QPushButton::clicked, bar, [&ctx, dom] {
            ctx.debugger.setFocus(dom);
            const DebugLocation loc = ctx.debugger.location(dom);
            if (loc.valid) { Location l; l.file = loc.file; l.line = loc.line; ctx.showExecution.fire(l); }
        });
        QObject::connect(c.step, &QPushButton::clicked, bar, [&ctx, dom] { ctx.debugger.setFocus(dom); ctx.debugger.stepInto(); });
        QObject::connect(c.over, &QPushButton::clicked, bar, [&ctx, dom] { ctx.debugger.setFocus(dom); ctx.debugger.stepOver(); });
    }
    auto refresh = [&ctx, bar, chips] {
        const bool active = ctx.debugger.active();
        const bool running = ctx.debugger.running();
        for (int d = 0; d < 2; d++) {
            Chip& c = (*chips)[(size_t)d];
            const asmcore::Domain dom = d == 0 ? asmcore::Domain::Main : asmcore::Domain::Peri;
            const bool focus = active && ctx.debugger.focus() == dom;
            const DebugLocation loc = active ? ctx.debugger.location(dom) : DebugLocation();
            QString text = QString::fromUtf8("\xE2\x80\x94");
            if (loc.valid) {
                text = QFileInfo(fromStd(loc.file)).fileName() + ":" + QString::number(loc.line);
                const std::string ins = ctx.debugger.instructionText(dom);
                if (!ins.empty()) text += "   " + fromStd(ins);
            }
            c.go->setText(QFontMetrics(c.go->font()).elidedText(text, Qt::ElideRight, 420));
            c.badge->setText(QString(kName[d]) + (focus ? QString::fromUtf8("  \xE2\x96\xB6 STEP") : QString()));
            const QString colour = kColour[d];
            c.badge->setStyleSheet(QString("background:%1; color:#141820; font-weight:bold; padding:2px 8px; border-radius:4px;%2").arg(colour, focus ? " border:2px solid white;" : ""));
            c.frame->setStyleSheet(QString("QFrame#%1 { border:%2px solid %3; border-radius:6px; }").arg(c.frame->objectName()).arg(focus ? 3 : 1).arg(focus ? colour : QString("#808080")));
            c.frame->setProperty("focused", focus);
            const bool canStep = active && !running && loc.valid;
            c.step->setEnabled(canStep);
            c.over->setEnabled(canStep);
            c.go->setEnabled(loc.valid);
        }
        bar->setVisible(active);
    };
    conns->add(ctx.debugger.updated, [refresh](const SimUpdate&) { refresh(); });
    conns->add(ctx.debugger.focusChanged, [refresh](asmcore::Domain) { refresh(); });
    refresh();
    struct Hold : QObject { std::shared_ptr<ConnectionBag> c; std::shared_ptr<std::vector<Chip>> v; };           // the connections and the chips live as long as the bar
    auto* hold = new Hold;
    hold->c = conns;
    hold->v = chips;
    hold->setParent(bar);
    return bar;
}

}  // namespace acide
