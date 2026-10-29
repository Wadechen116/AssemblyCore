// Debugging panels. Each one listens to the Debugger of the AppContext, shows one aspect of the simulator state and, where it makes
// sense, lets the user change it (poke memory, set a signal, write a CSR, edit a breakpoint condition).
#include <set>

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QSpinBox>
#include <QTimer>
#include <QCoreApplication>
#include <QAbstractTableModel>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QTableView>
#include <QTableWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "asmcore/diagnostics.h"
#include "panel_registry.h"
#include "text_util.h"
#include "theme.h"

namespace acide {

namespace {

using asmcore::Domain;

QString hex(uint64_t v, int digits) { return QString("%1").arg((qulonglong)v, digits, 16, QChar('0')).toUpper(); }
QString shortFile(const std::string& f) { return QFileInfo(fromStd(f)).fileName(); }
bool parseNumber(const QString& text, int64_t& out) {
    bool ok = false;
    QString t = text.trimmed();
    qlonglong v = t.startsWith("0x", Qt::CaseInsensitive) ? t.mid(2).toLongLong(&ok, 16) : t.toLongLong(&ok, 0);
    out = v;
    return ok;
}
QTableWidgetItem* ro(const QString& s) {
    auto* it = new QTableWidgetItem(s);
    it->setFlags(it->flags() & ~Qt::ItemIsEditable);
    return it;
}

// ---- state ------------------------------------------------------------------------------------------------------------------------
class StatePanel : public QWidget {
    Q_DECLARE_TR_FUNCTIONS(StatePanel)
public:
    explicit StatePanel(AppContext& ctx) : ctx_(ctx) {
        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(2, 2, 2, 2);
        tree_ = new QTreeWidget;
        tree_->setObjectName("stateTree");
        tree_->setHeaderLabels({tr("Item"), tr("Value")});
        tree_->setFont(editorFont(ctx.settings));
        lay->addWidget(tree_);
        auto* row = new QHBoxLayout;
        clearMain_ = new QPushButton(tr("Clear Main fault"));
        clearPeri_ = new QPushButton(tr("Clear Peri fault"));
        clearMain_->setObjectName("clearMainFault");
        clearPeri_->setObjectName("clearPeriFault");
        row->addWidget(clearMain_);
        row->addWidget(clearPeri_);
        lay->addLayout(row);
        connect(clearMain_, &QPushButton::clicked, this, [this] { ctx_.debugger.session().command("fault_clear main"); });
        connect(clearPeri_, &QPushButton::clicked, this, [this] { ctx_.debugger.session().command("fault_clear peri"); });
        conns_.add(ctx.debugger.updated, [this](const SimUpdate& u) { fill(u.view); });
        fill(ctx.debugger.session().view());
    }

private:
    void add(QTreeWidgetItem* parent, const QString& k, const QString& v) { new QTreeWidgetItem(parent, {k, v}); }
    void fill(const ViewPtr& v) {
        tree_->clear();
        clearMain_->setVisible(v && v->hasFault[0]);
        clearPeri_->setVisible(v && v->hasFault[1]);
        if (!v) return;
        auto* t = new QTreeWidgetItem(tree_, {tr("Time"), QString()});
        add(t, tr("Cycle"), QString::number((qulonglong)v->cycle));
        for (int i = 0; i < 2; i++) {
            Domain d = v->domainOf(i);
            auto* g = new QTreeWidgetItem(tree_, {i == 0 ? tr("Main") : tr("Peri"), QString()});
            DebugLocation loc = ctx_.debugger.location(d);
            QString where = loc.valid ? QString("%1:%2").arg(shortFile(loc.file)).arg(loc.line) : QString();
            add(g, tr("PC"), QString("0x%1   %2").arg(hex(v->pc[i], 2), where));
            add(g, tr("State"), v->faulted[i] ? tr("FAULT") : v->halted[i] ? tr("halted (spinning on a jump to itself)") : tr("running"));
            add(g, tr("Instructions"), QString::number((qulonglong)v->instrs[i]));
            if (v->hasFault[i]) {
                add(g, tr("Fault"), fromStd(v->fault[i].code));
                add(g, tr("Message"), fromStd(v->fault[i].msg));
                add(g, tr("At cycle"), QString::number((qulonglong)v->fault[i].at));
            }
        }
        auto* alu = new QTreeWidgetItem(tree_, {tr("ALU"), QString()});
        auto bytes = [&](const char* r) { auto it = v->regions.find(r); return it == v->regions.end() ? std::vector<uint8_t>() : it->second; };
        auto le = [](const std::vector<uint8_t>& b) { uint64_t x = 0; for (size_t i = 0; i < b.size(); i++) x |= (uint64_t)b[i] << (8 * i); return x; };
        add(alu, "ALU_OPA", QString("0x%1  (%2)").arg(hex(le(bytes("opa")), 4)).arg((qulonglong)le(bytes("opa"))));
        add(alu, "ALU_OPB", QString("0x%1  (%2)").arg(hex(le(bytes("opb")), 4)).arg((qulonglong)le(bytes("opb"))));
        add(alu, "ALU_OUT", QString("0x%1  (%2)").arg(hex(le(bytes("out")), 8)).arg((qulonglong)le(bytes("out"))));
        add(alu, "ALU_STATUS", QString("0x%1").arg(hex(le(bytes("status")) & 0xFF, 2)));
        auto meta = bytes("meta");
        auto* slot = new QTreeWidgetItem(tree_, {tr("Result slot"), QString()});
        if (meta.size() >= 6) {
            add(slot, tr("Valid"), meta[4] ? tr("yes") : tr("no"));
            add(slot, tr("Service id"), QString::number(meta[0]));
            add(slot, tr("Overrun"), meta[5] ? tr("yes") : tr("no"));
        }
        auto payload = bytes("slot");
        QString p;
        for (size_t i = 0; i < payload.size() && i < 16; i++) p += hex(payload[i], 2) + " ";
        add(slot, tr("Payload[0..15]"), p.trimmed());
        tree_->expandAll();
        tree_->resizeColumnToContents(0);
    }

    AppContext& ctx_;
    QTreeWidget* tree_;
    QPushButton *clearMain_, *clearPeri_;
    ConnectionBag conns_;
};

// ---- memory -----------------------------------------------------------------------------------------------------------------------
class MemoryPanel : public QWidget {
    Q_DECLARE_TR_FUNCTIONS(MemoryPanel)
public:
    explicit MemoryPanel(AppContext& ctx) : ctx_(ctx) {
        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(2, 2, 2, 2);
        region_ = new QComboBox;
        region_->setObjectName("memoryRegion");
        for (const char* r : {"scratch", "csrbuf", "slot", "meta", "rx", "const", "opa", "opb", "out", "status"}) region_->addItem(r);
        lay->addWidget(region_);
        table_ = new QTableWidget(0, 10);
        table_->setObjectName("memoryTable");
        table_->setHorizontalHeaderLabels({tr("Offset"), "+0", "+1", "+2", "+3", "+4", "+5", "+6", "+7", tr("ASCII")});
        table_->verticalHeader()->hide();
        table_->setFont(editorFont(ctx.settings));
        table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        lay->addWidget(table_);
        connect(region_, &QComboBox::currentTextChanged, this, [this] { prev_.clear(); fill(ctx_.debugger.session().view()); });
        connect(table_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* it) {
            if (filling_ || it->column() < 1 || it->column() > 8) return;
            bool ok = false;
            int v = it->text().trimmed().toInt(&ok, 16);
            if (!ok || v < 0 || v > 255) { fill(ctx_.debugger.session().view()); return; }
            ctx_.debugger.session().writeRegion(toStd(region_->currentText()), it->row() * 8 + it->column() - 1, {(uint8_t)v});
        });
        conns_.add(ctx.debugger.updated, [this](const SimUpdate& u) { fill(u.view); });
        fill(ctx.debugger.session().view());
    }

private:
    void fill(const ViewPtr& v) {
        filling_ = true;
        std::string name = toStd(region_->currentText());
        std::vector<uint8_t> cur;
        if (v) { auto it = v->regions.find(name); if (it != v->regions.end()) cur = it->second; }
        const int rows = ((int)cur.size() + 7) / 8;
        table_->setRowCount(rows);
        const bool writable = name == "scratch" || name == "csrbuf" || name == "rx" || name == "slot";
        for (int r = 0; r < rows; r++) {
            table_->setItem(r, 0, ro(hex((uint64_t)r * 8, 2)));
            QString ascii;
            for (int c = 0; c < 8; c++) {
                size_t i = (size_t)r * 8 + (size_t)c;
                QTableWidgetItem* it = new QTableWidgetItem(i < cur.size() ? hex(cur[i], 2) : QString());
                if (!writable || i >= cur.size()) it->setFlags(it->flags() & ~Qt::ItemIsEditable);
                if (i < cur.size() && i < prev_.size() && prev_[i] != cur[i]) it->setBackground(QColor("#c0392b"));      // changed since the last update
                table_->setItem(r, 1 + c, it);
                if (i < cur.size()) ascii += (cur[i] >= 32 && cur[i] < 127) ? QChar(cur[i]) : QChar('.');
            }
            table_->setItem(r, 9, ro(ascii));
        }
        prev_ = cur;
        filling_ = false;
    }

    AppContext& ctx_;
    QComboBox* region_;
    QTableWidget* table_;
    std::vector<uint8_t> prev_;
    bool filling_ = false;
    ConnectionBag conns_;
};

// ---- signals ----------------------------------------------------------------------------------------------------------------------
class SignalsPanel : public QWidget {
    Q_DECLARE_TR_FUNCTIONS(SignalsPanel)
public:
    explicit SignalsPanel(AppContext& ctx) : ctx_(ctx) {
        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(2, 2, 2, 2);
        filter_ = new QLineEdit;
        filter_->setPlaceholderText(tr("filter"));
        filter_->setClearButtonEnabled(true);
        lay->addWidget(filter_);
        table_ = new QTableWidget(0, 2);
        table_->setObjectName("signalsTable");
        table_->setHorizontalHeaderLabels({tr("Signal"), tr("Value")});
        table_->verticalHeader()->hide();
        table_->horizontalHeader()->setStretchLastSection(true);
        lay->addWidget(table_);
        connect(filter_, &QLineEdit::textChanged, this, [this] { fill(ctx_.debugger.session().view()); });
        connect(table_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* it) {
            if (filling_ || it->column() != 1) return;
            int64_t v;
            if (!parseNumber(it->text(), v)) { fill(ctx_.debugger.session().view()); return; }
            ctx_.debugger.session().setSignal(toStd(table_->item(it->row(), 0)->text()), v);
        });
        conns_.add(ctx.debugger.updated, [this](const SimUpdate& u) { fill(u.view); });
        fill(ctx.debugger.session().view());
    }

private:
    void fill(const ViewPtr& v) {
        filling_ = true;
        table_->setRowCount(0);
        if (v) {
            QString f = filter_->text();
            for (auto& kv : v->signalValues) {
                QString name = fromStd(kv.first);
                if (!f.isEmpty() && !name.contains(f, Qt::CaseInsensitive)) continue;
                int r = table_->rowCount();
                table_->insertRow(r);
                table_->setItem(r, 0, ro(name));
                auto* it = new QTableWidgetItem(QString::number(kv.second));
                auto p = prev_.find(kv.first);
                if (p != prev_.end() && p->second != kv.second) it->setBackground(QColor("#c0392b"));
                table_->setItem(r, 1, it);
            }
            prev_.clear();
            for (auto& kv : v->signalValues) prev_[kv.first] = kv.second;
        }
        filling_ = false;
    }

    AppContext& ctx_;
    QLineEdit* filter_;
    QTableWidget* table_;
    std::map<std::string, int64_t> prev_;
    bool filling_ = false;
    ConnectionBag conns_;
};

// ---- CSRs -------------------------------------------------------------------------------------------------------------------------
class CsrPanel : public QWidget {
    Q_DECLARE_TR_FUNCTIONS(CsrPanel)
public:
    explicit CsrPanel(AppContext& ctx) : ctx_(ctx) {
        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(2, 2, 2, 2);
        table_ = new QTableWidget(0, 3);
        table_->setObjectName("csrTable");
        table_->setHorizontalHeaderLabels({tr("CSR"), tr("Address"), tr("Value")});
        table_->verticalHeader()->hide();
        table_->horizontalHeader()->setStretchLastSection(true);
        lay->addWidget(table_);
        auto* hint = new QLabel(tr("Values are the stored bytes; editing writes the CSR (the write may trigger its action)."));
        hint->setWordWrap(true);
        lay->addWidget(hint);
        connect(table_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* it) {
            if (filling_ || it->column() != 2) return;
            int64_t v;
            if (!parseNumber(it->text(), v) || v < 0 || v > 255) { fill(ctx_.debugger.session().view()); return; }
            ctx_.debugger.session().command(QString("csr 0x%1=%2").arg(table_->item(it->row(), 1)->text().mid(2)).arg((int)v).toStdString());
        });
        conns_.add(ctx.debugger.updated, [this](const SimUpdate& u) { fill(u.view); });
        fill(ctx.debugger.session().view());
    }

private:
    void fill(const ViewPtr& v) {
        filling_ = true;
        table_->setRowCount(0);
        SnapshotPtr prog = ctx_.debugger.session().program();
        if (v && prog) {
            for (auto& kv : v->csrs) {
                int64_t addr = 0;
                for (const asmcore::SymbolInfo& s : prog->result.symbols) if (s.kind == "csr" && s.name == kv.first) addr = s.value;
                int r = table_->rowCount();
                table_->insertRow(r);
                table_->setItem(r, 0, ro(fromStd(kv.first)));
                table_->setItem(r, 1, ro(QString("0x%1").arg(hex((uint64_t)addr, 3))));
                auto* it = new QTableWidgetItem(QString("0x%1").arg(hex(kv.second, 2)));
                auto p = prev_.find(kv.first);
                if (p != prev_.end() && p->second != kv.second) it->setBackground(QColor("#c0392b"));
                table_->setItem(r, 2, it);
            }
            prev_.clear();
            for (auto& kv : v->csrs) prev_[kv.first] = kv.second;
        }
        filling_ = false;
    }

    AppContext& ctx_;
    QTableWidget* table_;
    std::map<std::string, uint8_t> prev_;
    bool filling_ = false;
    ConnectionBag conns_;
};

// ---- watch ------------------------------------------------------------------------------------------------------------------------
class WatchPanel : public QWidget {
    Q_DECLARE_TR_FUNCTIONS(WatchPanel)
public:
    explicit WatchPanel(AppContext& ctx) : ctx_(ctx) {
        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(2, 2, 2, 2);
        table_ = new QTableWidget(0, 3);
        table_->setObjectName("watchTable");
        table_->setHorizontalHeaderLabels({tr("Expression"), tr("Value"), tr("Hex")});
        table_->verticalHeader()->hide();
        table_->horizontalHeader()->setStretchLastSection(true);
        lay->addWidget(table_);
        auto* hint = new QLabel(tr("e.g.  scratch[0..1]   csr[0x120]   pc(main)   slot.valid   (empty a row to delete it)"));
        hint->setWordWrap(true);
        lay->addWidget(hint);
        connect(table_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* it) {
            if (filling_ || it->column() != 0) return;
            std::vector<std::string> ex;
            for (int r = 0; r < table_->rowCount(); r++) {
                QString t = table_->item(r, 0) ? table_->item(r, 0)->text().trimmed() : QString();
                if (!t.isEmpty()) ex.push_back(toStd(t));
            }
            ctx_.debugger.setWatchExprs(ex);
            ctx_.debugger.saveBreakpoints();
            fill(ctx_.debugger.session().view());
        });
        conns_.add(ctx.debugger.updated, [this](const SimUpdate& u) { fill(u.view); });
        conns_.add(ctx.workspace.projectChanged, [this] { fill(ctx_.debugger.session().view()); });
        fill(ctx.debugger.session().view());
    }

private:
    void fill(const ViewPtr& v) {
        filling_ = true;
        table_->setRowCount(0);
        const auto& exprs = ctx_.debugger.watchExprs();
        for (size_t i = 0; i < exprs.size(); i++) {
            table_->insertRow((int)i);
            table_->setItem((int)i, 0, new QTableWidgetItem(fromStd(exprs[i])));
            const WatchValue* w = v && i < v->watches.size() ? &v->watches[i] : nullptr;
            table_->setItem((int)i, 1, ro(w ? (w->ok ? QString::number(w->value) : "<" + fromStd(w->error) + ">") : QString()));
            table_->setItem((int)i, 2, ro(w && w->ok ? QString("0x%1").arg((qulonglong)w->value, 0, 16) : QString()));
        }
        int last = table_->rowCount();
        table_->insertRow(last);                                   // an empty row to type a new expression into
        table_->setItem(last, 0, new QTableWidgetItem(QString()));
        table_->setItem(last, 1, ro(QString()));
        table_->setItem(last, 2, ro(QString()));
        filling_ = false;
    }

    AppContext& ctx_;
    QTableWidget* table_;
    bool filling_ = false;
    ConnectionBag conns_;
};

// ---- call stack -------------------------------------------------------------------------------------------------------------------
class CallStackPanel : public QWidget {
    Q_DECLARE_TR_FUNCTIONS(CallStackPanel)
public:
    explicit CallStackPanel(AppContext& ctx) : ctx_(ctx) {
        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(2, 2, 2, 2);
        tree_ = new QTreeWidget;
        tree_->setObjectName("callStackTree");
        tree_->setHeaderLabels({tr("Frame"), tr("Where")});
        lay->addWidget(tree_);
        connect(tree_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* it, int) {
            int line = it->data(0, Qt::UserRole).toInt();
            if (line <= 0) return;
            Location l;
            l.file = toStd(it->data(0, Qt::UserRole + 1).toString());
            l.line = line;
            ctx_.goTo.fire(l);
        });
        conns_.add(ctx.debugger.updated, [this](const SimUpdate& u) { fill(u.view); });
        fill(ctx.debugger.session().view());
    }

private:
    void fill(const ViewPtr& v) {
        tree_->clear();
        if (!v) return;
        for (int i = 0; i < 2; i++) {
            Domain d = v->domainOf(i);
            auto* g = new QTreeWidgetItem(tree_, {i == 0 ? tr("Main") : tr("Peri"), QString()});
            g->setExpanded(true);
            DebugLocation loc = ctx_.debugger.location(d);
            auto place = [&](QTreeWidgetItem* it, const std::string& file, int line) {
                it->setData(0, Qt::UserRole, line);
                it->setData(0, Qt::UserRole + 1, fromStd(file));
            };
            if (loc.valid) {
                auto* cur = new QTreeWidgetItem(g, {QString::fromUtf8("\xE2\x96\xB6 ") + tr("current"), QString("%1:%2").arg(shortFile(loc.file)).arg(loc.line)});
                place(cur, loc.file, loc.line);
                // inline expansion: the chain of CALLs the instruction came from, innermost first
                for (auto it = loc.frames.rbegin(); it != loc.frames.rend(); ++it) {
                    auto* f = new QTreeWidgetItem(g, {fromStd(it->proc), tr("called from %1:%2").arg(shortFile(it->callSite.file)).arg(it->callSite.line)});
                    place(f, it->callSite.file, it->callSite.line);
                }
            }
            for (const asmcore::sim::CallFrame& fr : v->stack[i]) {         // softret: frames that exist at run time
                if (fr.inlined) continue;
                auto* f = new QTreeWidgetItem(g, {fromStd(fr.proc), tr("called from %1:%2").arg(shortFile(fr.callSite.file)).arg(fr.callSite.line)});
                place(f, fr.callSite.file, fr.callSite.line);
            }
        }
        tree_->resizeColumnToContents(0);
    }

    AppContext& ctx_;
    QTreeWidget* tree_;
    ConnectionBag conns_;
};

// ---- breakpoints ------------------------------------------------------------------------------------------------------------------
class BreakpointsPanel : public QWidget {
    Q_DECLARE_TR_FUNCTIONS(BreakpointsPanel)
public:
    explicit BreakpointsPanel(AppContext& ctx) : ctx_(ctx) {
        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(2, 2, 2, 2);
        table_ = new QTableWidget(0, 4);
        table_->setObjectName("breakpointsTable");
        table_->setHorizontalHeaderLabels({tr("On"), tr("Line"), tr("Condition"), tr("Hits")});
        table_->verticalHeader()->hide();
        table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
        lay->addWidget(table_);
        auto* row = new QHBoxLayout;
        auto* rm = new QPushButton(tr("Remove"));
        auto* rmAll = new QPushButton(tr("Remove all"));
        row->addWidget(rm);
        row->addWidget(rmAll);
        row->addStretch(1);
        lay->addLayout(row);
        lay->addWidget(new QLabel(tr("Data watches (stop when the value changes):")));
        watches_ = new QListWidget;
        watches_->setObjectName("dataWatches");
        watches_->setMaximumHeight(90);
        lay->addWidget(watches_);
        auto* add = new QHBoxLayout;
        watchEdit_ = new QLineEdit;
        watchEdit_->setPlaceholderText(tr("scratch[8..9]   csr 0x120   signal slot.valid   or an expression"));
        watchEdit_->setObjectName("dataWatchEdit");
        auto* addBtn = new QPushButton(tr("Add"));
        add->addWidget(watchEdit_, 1);
        add->addWidget(addBtn);
        lay->addLayout(add);
        connect(table_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* it) {
            if (filling_) return;
            auto* key = table_->item(it->row(), 1);
            if (!key) return;
            std::string file = toStd(key->data(Qt::UserRole + 1).toString());
            int line = key->data(Qt::UserRole).toInt();
            if (it->column() == 0) ctx_.debugger.setEnabled(file, line, it->checkState() == Qt::Checked);
            else if (it->column() == 2) ctx_.debugger.setCondition(file, line, toStd(it->text().trimmed()));
        });
        connect(table_, &QTableWidget::cellActivated, this, [this](int row, int) { open(row); });
        connect(table_, &QTableWidget::cellDoubleClicked, this, [this](int row, int col) { if (col == 1) open(row); });
        connect(rm, &QPushButton::clicked, this, [this] {
            for (QModelIndex ix : table_->selectionModel()->selectedRows()) {
                auto* key = table_->item(ix.row(), 1);
                if (key) ctx_.debugger.removeLineBreakpoint(toStd(key->data(Qt::UserRole + 1).toString()), key->data(Qt::UserRole).toInt());
            }
        });
        connect(rmAll, &QPushButton::clicked, this, [this] { ctx_.debugger.clearLineBreakpoints(); });
        connect(addBtn, &QPushButton::clicked, this, [this] { addWatch(); });
        connect(watchEdit_, &QLineEdit::returnPressed, this, [this] { addWatch(); });
        connect(watches_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* it) {      // double click removes
            auto w = ctx_.debugger.dataWatches();
            w.erase(std::remove(w.begin(), w.end(), toStd(it->text())), w.end());
            ctx_.debugger.setDataWatches(w);
            ctx_.debugger.saveBreakpoints();
            fillWatches();
        });
        conns_.add(ctx.debugger.breakpointsChanged, [this] { fill(); });
        conns_.add(ctx.debugger.updated, [this](const SimUpdate&) { fill(); });
        conns_.add(ctx.workspace.projectChanged, [this] { fill(); fillWatches(); });
        fill();
        fillWatches();
    }

private:
    void open(int row) {
        auto* key = table_->item(row, 1);
        if (!key) return;
        Location l;
        l.file = toStd(key->data(Qt::UserRole + 1).toString());
        l.line = key->data(Qt::UserRole).toInt();
        ctx_.goTo.fire(l);
    }
    void addWatch() {
        std::string t = toStd(watchEdit_->text().trimmed());
        if (t.empty()) return;
        auto w = ctx_.debugger.dataWatches();
        if (std::find(w.begin(), w.end(), t) == w.end()) w.push_back(t);
        ctx_.debugger.setDataWatches(w);
        ctx_.debugger.saveBreakpoints();
        watchEdit_->clear();
        fillWatches();
    }
    void fillWatches() {
        watches_->clear();
        for (const std::string& w : ctx_.debugger.dataWatches()) watches_->addItem(fromStd(w));
    }
    void fill() {
        filling_ = true;
        table_->setRowCount(0);
        for (const LineBreakpoint& b : ctx_.debugger.lineBreakpoints()) {
            int r = table_->rowCount();
            table_->insertRow(r);
            auto* on = new QTableWidgetItem;
            on->setFlags((on->flags() | Qt::ItemIsUserCheckable) & ~Qt::ItemIsEditable);
            on->setCheckState(b.enabled ? Qt::Checked : Qt::Unchecked);
            table_->setItem(r, 0, on);
            auto* where = ro(QString("%1:%2%3").arg(shortFile(b.file)).arg(b.line).arg(b.resolved ? QString() : tr("  (no code)")));
            where->setData(Qt::UserRole, b.line);
            where->setData(Qt::UserRole + 1, fromStd(b.file));
            table_->setItem(r, 1, where);
            table_->setItem(r, 2, new QTableWidgetItem(fromStd(b.condition)));
            table_->setItem(r, 3, ro(QString::number((qulonglong)b.hits)));
        }
        filling_ = false;
    }

    AppContext& ctx_;
    QTableWidget* table_;
    QListWidget* watches_;
    QLineEdit* watchEdit_;
    bool filling_ = false;
    ConnectionBag conns_;
};

// ---- events -----------------------------------------------------------------------------------------------------------------------
const char* typeName(asmcore::sim::EventType t) {
    using asmcore::sim::EventType;
    switch (t) {
        case EventType::Issue: return "issue";
        case EventType::Retire: return "retire";
        case EventType::RegionWrite: return "write";
        case EventType::CsrAccess: return "csr";
        case EventType::Signal: return "signal";
        case EventType::Bus: return "bus";
        case EventType::Publish: return "publish";
        case EventType::Consume: return "consume";
        case EventType::Overrun: return "overrun";
        case EventType::Fault: return "FAULT";
        case EventType::Inject: return "inject";
        case EventType::Note: return "note";
        case EventType::HwEvent: return "event";
    }
    return "?";
}

// Holds up to kCap events. A filter shows a subset; rows are numbered by the running index of the event so trimming the old ones does not
// disturb the list of what is shown.
class EventModel : public QAbstractTableModel {
    Q_DECLARE_TR_FUNCTIONS(EventModel)
public:
    static const size_t kCap = 50000;
    int rowCount(const QModelIndex& p = QModelIndex()) const override { return p.isValid() ? 0 : (int)shown_.size(); }
    int columnCount(const QModelIndex& p = QModelIndex()) const override { return p.isValid() ? 0 : 4; }
    QVariant headerData(int s, Qt::Orientation o, int role) const override {
        if (o != Qt::Horizontal || role != Qt::DisplayRole) return QVariant();
        static const char* h[] = {QT_TR_NOOP("Cycle"), QT_TR_NOOP("Dom"), QT_TR_NOOP("Type"), QT_TR_NOOP("Detail")};
        return tr(h[s]);
    }
    QVariant data(const QModelIndex& ix, int role) const override {
        if (!ix.isValid() || ix.row() >= (int)shown_.size()) return QVariant();
        const asmcore::sim::Event& e = at(shown_[(size_t)ix.row()]);
        if (role == Qt::ForegroundRole && e.type == asmcore::sim::EventType::Fault) return QColor("#f14c4c");
        if (role != Qt::DisplayRole) return QVariant();
        switch (ix.column()) {
            case 0: return (qulonglong)e.t;
            case 1: return e.domain == 0 ? "M" : e.domain == 1 ? "P" : "-";
            case 2: return typeName(e.type);
            default: return describe(e);
        }
    }
    const asmcore::sim::Event* eventAt(int row) const { return row >= 0 && row < (int)shown_.size() ? &at(shown_[(size_t)row]) : nullptr; }
    int total() const { return (int)all_.size(); }

    void setFilter(int kind, const QString& text) {
        kind_ = kind;
        text_ = text;
        beginResetModel();
        shown_.clear();
        for (size_t i = 0; i < all_.size(); i++) if (match(all_[i])) shown_.push_back(base_ + i);
        endResetModel();
    }
    void clear() {
        beginResetModel();
        all_.clear();
        shown_.clear();
        base_ = 0;
        endResetModel();
    }
    void append(const std::vector<asmcore::sim::Event>& ev) {
        std::vector<size_t> add;
        for (const auto& e : ev) {
            all_.push_back(e);
            if (match(e)) add.push_back(base_ + all_.size() - 1);
        }
        if (!add.empty()) {
            beginInsertRows(QModelIndex(), (int)shown_.size(), (int)(shown_.size() + add.size()) - 1);
            shown_.insert(shown_.end(), add.begin(), add.end());
            endInsertRows();
        }
        if (all_.size() > kCap) {
            size_t drop = all_.size() - kCap;
            all_.erase(all_.begin(), all_.begin() + (long)drop);
            base_ += drop;
            size_t k = 0;
            while (k < shown_.size() && shown_[k] < base_) k++;
            if (k) { beginRemoveRows(QModelIndex(), 0, (int)k - 1); shown_.erase(shown_.begin(), shown_.begin() + (long)k); endRemoveRows(); }
        }
    }

private:
    const asmcore::sim::Event& at(size_t idx) const { return all_[idx - base_]; }
    static QString describe(const asmcore::sim::Event& e) {
        using asmcore::sim::EventType;
        QString s = fromStd(e.what);
        switch (e.type) {
            case EventType::Issue: return QString("pc=0x%1  %2").arg(hex(e.pc, 2), fromStd(e.text));
            case EventType::RegionWrite: return QString("%1[%2..%3]").arg(s).arg((int)e.a).arg((int)(e.a + e.b - 1));
            case EventType::CsrAccess: return QString("%1 0x%2 = 0x%3").arg(s, hex((uint64_t)e.a, 3), hex((uint64_t)e.b, 2));
            case EventType::Signal: return QString("%1: %2 -> %3").arg(s).arg((qlonglong)e.a).arg((qlonglong)e.b);
            case EventType::Bus: return QString("%1 dev 0x%2 %3, %4 byte(s)").arg(s, hex((uint64_t)e.a, 2), e.b ? "ack" : "NACK").arg((int)e.c);
            case EventType::Publish: return QString("service %1, %2 byte(s)").arg((int)e.a).arg((int)e.b);
            case EventType::Consume: return QString("service %1").arg((int)e.a);
            default: return (s + " " + fromStd(e.text)).trimmed();
        }
    }
    bool match(const asmcore::sim::Event& e) const {
        using asmcore::sim::EventType;
        switch (kind_) {
            case 1: if (e.type != EventType::Issue && e.type != EventType::Retire) return false; break;
            case 2: if (e.type != EventType::Bus && e.type != EventType::CsrAccess) return false; break;
            case 3: if (e.type != EventType::Publish && e.type != EventType::Consume && e.type != EventType::Overrun) return false; break;
            case 4: if (e.type != EventType::Fault && e.type != EventType::Note && e.type != EventType::Inject && e.type != EventType::Signal) return false; break;
            default: break;
        }
        if (text_.isEmpty()) return true;
        return describe(e).contains(text_, Qt::CaseInsensitive) || QString(typeName(e.type)).contains(text_, Qt::CaseInsensitive);
    }

    std::deque<asmcore::sim::Event> all_;
    std::vector<size_t> shown_;
    size_t base_ = 0;
    int kind_ = 0;
    QString text_;
};

class EventsPanel : public QWidget {
    Q_DECLARE_TR_FUNCTIONS(EventsPanel)
public:
    explicit EventsPanel(AppContext& ctx) : ctx_(ctx) {
        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(2, 2, 2, 2);
        auto* bar = new QHBoxLayout;
        kind_ = new QComboBox;
        kind_->setObjectName("eventKind");
        kind_->addItems({tr("All events"), tr("Instructions"), tr("Bus and CSR"), tr("Service publish / consume"), tr("Faults, signals, notes")});
        filter_ = new QLineEdit;
        filter_->setPlaceholderText(tr("filter text"));
        filter_->setClearButtonEnabled(true);
        auto* save = new QPushButton(tr("Save trace..."));
        auto* clear = new QPushButton(tr("Clear"));
        count_ = new QLabel;
        bar->addWidget(kind_);
        bar->addWidget(filter_, 1);
        bar->addWidget(count_);
        bar->addWidget(clear);
        bar->addWidget(save);
        lay->addLayout(bar);
        view_ = new QTableView;
        view_->setObjectName("eventsView");
        view_->setModel(&model_);
        view_->verticalHeader()->hide();
        view_->setSelectionBehavior(QAbstractItemView::SelectRows);
        view_->horizontalHeader()->setStretchLastSection(true);
        view_->setFont(editorFont(ctx.settings));
        view_->setColumnWidth(0, 80);
        view_->setColumnWidth(1, 36);
        view_->setColumnWidth(2, 80);
        lay->addWidget(view_);
        auto refilter = [this] { model_.setFilter(kind_->currentIndex(), filter_->text()); updateCount(); };
        connect(kind_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, refilter);
        connect(filter_, &QLineEdit::textChanged, this, refilter);
        connect(clear, &QPushButton::clicked, this, [this] { model_.clear(); updateCount(); });
        connect(view_, &QTableView::activated, this, [this](const QModelIndex& ix) { open(ix.row()); });
        connect(view_, &QTableView::doubleClicked, this, [this](const QModelIndex& ix) { open(ix.row()); });
        connect(save, &QPushButton::clicked, this, [this] {
            QString f = QFileDialog::getSaveFileName(this, tr("Save trace"), QString(), tr("Text (*.txt);;CSV (*.csv);;Value change dump (*.vcd)"));
            if (f.isEmpty()) return;
            std::string fmt = f.endsWith(".csv") ? "csv" : f.endsWith(".vcd") ? "vcd" : "text";
            ctx_.debugger.session().exportTrace(fmt, [this, f](const std::string& text) {
                if (!ctx_.workspace.fileSystem()->write(toStd(f), text)) ctx_.status.fire(tr("cannot write %1").arg(f));
                else ctx_.status.fire(tr("trace saved to %1").arg(f));
            });
        });
        conns_.add(ctx.debugger.updated, [this](const SimUpdate& u) {
            if (u.eventsReset) model_.clear();
            if (u.droppedEvents) dropped_ += u.droppedEvents;
            model_.append(u.events);
            updateCount();
            if (follow_ && !u.events.empty()) view_->scrollToBottom();
        });
    }

private:
    void updateCount() { count_->setText(tr("%1 / %2").arg(model_.rowCount()).arg(model_.total())); }
    void open(int row) {
        const asmcore::sim::Event* e = model_.eventAt(row);
        if (!e || e->domain < 0) return;
        DebugLocation l = ctx_.debugger.locationOf(e->domain == 0 ? Domain::Main : Domain::Peri, e->pc);
        if (!l.valid) return;
        Location loc;
        loc.file = l.file;
        loc.line = l.line;
        ctx_.goTo.fire(loc);
    }

    AppContext& ctx_;
    EventModel model_;
    QComboBox* kind_;
    QLineEdit* filter_;
    QLabel* count_;
    QTableView* view_;
    size_t dropped_ = 0;
    bool follow_ = true;
    ConnectionBag conns_;
};

}  // namespace

// ---- auto debug -------------------------------------------------------------------------------------------------------------------
// One click finds what is wrong and mends what has one sure repair (services/auto_fix.h): misspelt words, numbers out of range, a missing closing jump ...
// in the text or in the blocks, as ONE undo step; then the repaired program is run at full speed in a simulator of its own and searched for what is still wrong
// (services/auto_debug.h). One row per thing: what was fixed, what is left for you, what the run found. A click shows where (on the blocks of a block
// program); "Debug to here" runs the debug session to the cycle a finding is about.
class AutoDebugPanel : public QWidget {
    Q_DECLARE_TR_FUNCTIONS(AutoDebugPanel)
public:
    explicit AutoDebugPanel(AppContext& ctx) : ctx_(ctx) {
        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(2, 2, 2, 2);
        auto* top = new QHBoxLayout;
        fix_ = new QPushButton(tr("Find and fix"));
        fix_->setObjectName("autoDebugFix");
        QFont bold = fix_->font();
        bold.setBold(true);
        fix_->setFont(bold);
        fix_->setToolTip(tr("Finds what is wrong and mends what has one sure repair (a misspelt word, a number out of range, a missing closing jump ...) in the text or the blocks - as one step that Ctrl+Z takes back - then runs the program at full speed in a simulator of its own and looks for what is still wrong."));
        top->addWidget(fix_);
        run_ = new QPushButton(tr("Look only"));
        run_->setObjectName("autoDebugRun");
        run_->setToolTip(tr("The same, but nothing is changed: it says what it would repair and what the run finds."));
        top->addWidget(run_);
        top->addWidget(new QLabel(tr("run for")));
        cycles_ = new QSpinBox;
        cycles_->setObjectName("autoDebugCycles");
        cycles_->setRange(1000, 200000000);
        cycles_->setSingleStep(10000);
        cycles_->setSuffix(tr(" cycles"));
        cycles_->setValue(ctx.settings.getInt("autodebug.cycles", 200000));
        top->addWidget(cycles_);
        auto_ = new QCheckBox(tr("look after every successful build"));
        auto_->setObjectName("autoDebugAuto");
        auto_->setChecked(ctx.settings.getInt("autodebug.auto", 0) != 0);
        top->addWidget(auto_);
        top->addStretch(1);
        lay->addLayout(top);
        summary_ = new QLabel(tr("Not run yet."));
        summary_->setObjectName("autoDebugSummary");
        summary_->setWordWrap(true);
        lay->addWidget(summary_);
        list_ = new QTreeWidget;
        list_->setObjectName("autoDebugList");
        list_->setRootIsDecorated(false);
        list_->setUniformRowHeights(true);
        list_->setHeaderLabels({tr("Kind"), tr("Cycle"), tr("Where"), tr("Finding")});
        list_->setSelectionBehavior(QAbstractItemView::SelectRows);
        list_->header()->setStretchLastSection(true);
        lay->addWidget(list_, 1);
        detail_ = new QLabel;
        detail_->setObjectName("autoDebugDetail");
        detail_->setWordWrap(true);
        detail_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        detail_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        detail_->setMinimumHeight(48);
        lay->addWidget(detail_);
        auto* bottom = new QHBoxLayout;
        show_ = new QPushButton(tr("Show in code"));
        show_->setObjectName("autoDebugShow");
        replay_ = new QPushButton(tr("Debug to here"));
        replay_->setObjectName("autoDebugReplay");
        replay_->setToolTip(tr("Starts the simulator from cycle 0 at full speed and stops it at the cycle of the finding, so that you can look at the state"));
        bottom->addWidget(show_);
        bottom->addWidget(replay_);
        bottom->addStretch(1);
        lay->addLayout(bottom);
        timer_ = new QTimer(this);
        timer_->setSingleShot(true);

        connect(fix_, &QPushButton::clicked, this, [this] { run(true); });
        connect(run_, &QPushButton::clicked, this, [this] { run(false); });
        connect(cycles_, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v) { ctx_.settings.setInt("autodebug.cycles", v); });
        connect(auto_, &QCheckBox::toggled, this, [this](bool on) { ctx_.settings.setInt("autodebug.auto", on ? 1 : 0); });
        connect(list_, &QTreeWidget::currentItemChanged, this, [this] { select(); });
        connect(list_, &QTreeWidget::itemActivated, this, [this] { showCurrent(); });
        connect(show_, &QPushButton::clicked, this, [this] { showCurrent(); });
        connect(replay_, &QPushButton::clicked, this, [this] {
            const Row* r = current();
            if (!r) return;
            if (!ctx_.debugger.active()) { ctx_.status.fire(tr("Start debugging first (F5), then use this")); return; }
            ctx_.debugger.runToCycle(r->at);
        });
        connect(timer_, &QTimer::timeout, this, [this] { if (isVisible()) run(false); });
        conns_.add(ctx.snapshotChanged, [this](SnapshotPtr s) { if (auto_->isChecked() && s && s->result.ok && !busy_) timer_->start(500); });
        conns_.add(ctx.debugger.updated, [this](const SimUpdate&) { buttons(); });
        buttons();
    }

private:
    struct Row {
        enum Kind { Fixed, CanFix, Error, Warning, Note, Guess } kind = Note;          // Guess: mended, but with a value that had to be chosen
        asmcore::sim::Cycle at = 0;
        QString where, title, detail, hint;
        std::string file;
        int line = 0;
        int count = 1;
    };
    const Row* current() const {
        QTreeWidgetItem* it = list_->currentItem();
        if (!it) return nullptr;
        const size_t i = (size_t)it->data(0, Qt::UserRole).toInt();
        return i < rows_.size() ? &rows_[i] : nullptr;
    }
    void buttons() {
        const Row* r = current();
        show_->setEnabled(r && r->line > 0 && !r->file.empty());
        replay_->setEnabled(r && r->at > 0 && ctx_.debugger.active());
    }
    void select() {
        const Row* r = current();
        if (!r) { detail_->clear(); buttons(); return; }
        detail_->setText("<b>" + r->title.toHtmlEscaped() + "</b><br>" + r->detail.toHtmlEscaped().replace("\n", "<br>") + (r->hint.isEmpty() ? QString() : "<br><i>" + r->hint.toHtmlEscaped() + "</i>"));
        buttons();
    }
    void showCurrent() {
        const Row* r = current();
        if (!r || r->line <= 0 || r->file.empty()) return;
        Location l;
        l.file = r->file;
        l.line = r->line;
        ctx_.showExecution.fire(l);
    }
    static QString lineOf(const std::string& s) { QString t = fromStd(s); t.replace('\n', QString::fromUtf8(" \xE2\x8F\x8E ")); return t.trimmed(); }
    void run(bool fix) {
        busy_ = true;
        rows_.clear();
        list_->clear();
        detail_->clear();
        AutoFixOutcome fx;
        QApplication::setOverrideCursor(Qt::WaitCursor);
        if (ctx_.autoFix) fx = ctx_.autoFix(fix);
        else fx.error = "no window";
        if (!fx.error.empty()) { QApplication::restoreOverrideCursor(); busy_ = false; summary_->setText(fromStd(fx.error)); buttons(); return; }
        const bool applied = fix && fx.applied;
        // what was (or can be) mended
        std::set<std::pair<std::string, int>> left;                                       // steps that could not be put into the blocks
        for (const FixStep& s : fx.notApplied) left.insert({s.file, s.line});
        int mended = 0;
        for (const FixStep& s : fx.result.steps) {
            Row r;
            const bool notDone = applied && left.count({s.file, s.line});
            r.kind = notDone ? Row::Warning : applied ? (s.needsCheck ? Row::Guess : Row::Fixed) : Row::CanFix;
            if (!notDone) mended++;
            r.file = s.file;
            r.line = s.line;
            r.where = shortFile(s.file) + ":" + QString::number(s.line);
            r.title = (notDone ? tr("Could not mend in the blocks: ") : QString()) + fromStd(s.what);
            r.detail = tr("%1 line %2 (%3)\n  before: %4\n  after:  %5").arg(shortFile(s.file)).arg(s.line).arg(fromStd(s.code)).arg(lineOf(s.before).trimmed()).arg(lineOf(s.after).trimmed());
            r.hint = notDone ? tr("The word is not a field of any block: change it in the block by hand.")
                   : s.needsCheck ? tr("A good guess, not a certainty: check that this is what you meant. Ctrl+Z in the editor takes the repairs back (all of them in one step).")
                   : applied ? tr("Ctrl+Z in the editor takes the repairs back (all of them in one step).") : QString();
            rows_.push_back(r);
        }
        // what is left of the assembler's complaints
        int errors = 0, warnings = 0;
        for (const asmcore::Diagnostic& d : fx.result.remaining) {
            Row r;
            r.kind = d.sev == asmcore::Diagnostic::Error ? Row::Error : d.sev == asmcore::Diagnostic::Warning ? Row::Warning : Row::Note;
            (r.kind == Row::Error ? errors : warnings)++;
            r.file = d.loc.file;
            r.line = d.loc.line;
            r.where = shortFile(d.loc.file) + ":" + QString::number(d.loc.line);
            r.title = fromStd(d.code) + ": " + fromStd(d.msg);
            r.detail = fromStd(d.msg);
            const std::string why = asmcore::explainDiagnostic(d.code);
            const size_t f = why.find("fix:");
            r.hint = f == std::string::npos ? fromStd(why) : fromStd(why.substr(f));
            rows_.push_back(r);
        }
        // the repaired program, run
        QString ran;
        bool didRun = false;
        report_ = AutoDebugReport();
        if (fx.result.clean() && fx.notApplied.empty()) {
            AutoDebugOptions o;
            o.maxCycles = (asmcore::sim::Cycle)cycles_->value();
            std::string why;
            if (!ctx_.debugger.autoDebug(o, report_, why, fx.result.build)) ran = fromStd(why);
            else if (!report_.ok()) ran = tr("The simulator could not be made: %1").arg(fromStd(report_.error));
            else {
                didRun = true;
                ran = tr("Ran %1 cycles").arg((qulonglong)report_.endAt);
                if (!report_.stopDetail.empty() && report_.reason != asmcore::sim::StopReason::CycleLimit) ran += " (" + fromStd(report_.stopDetail) + ")";
                for (const AutoFinding& f : report_.findings) {
                    Row r;
                    r.kind = f.sev == AutoFinding::Error ? Row::Error : f.sev == AutoFinding::Warning ? Row::Warning : Row::Note;
                    if (f.sev == AutoFinding::Error) errors++; else if (f.sev == AutoFinding::Warning) warnings++;
                    r.at = f.at;
                    r.file = f.file;
                    r.line = f.line;
                    r.count = f.count;
                    r.where = (f.domain == 0 ? tr("Main") : f.domain == 1 ? tr("Peri") : QString());
                    if (f.line > 0) r.where += (r.where.isEmpty() ? "" : " ") + shortFile(f.file) + ":" + QString::number(f.line);
                    r.title = fromStd(f.title) + (f.count > 1 ? QString(" (x%1)").arg(f.count) : QString());
                    r.detail = fromStd(f.detail);
                    r.hint = fromStd(f.hint);
                    rows_.push_back(r);
                }
            }
        } else if (!fx.result.clean()) {
            ran = tr("The program still has errors: it was not run.");
        }
        QApplication::restoreOverrideCursor();
        QStringList parts;
        if (!fx.result.steps.empty())
            parts << (applied ? tr("Fixed %1 problem(s) in %2 file(s) - Ctrl+Z in the editor takes them back.").arg(mended).arg((int)fx.changedFiles.size())
                              : tr("%1 problem(s) can be fixed automatically (nothing was changed).").arg(mended));
        if (!ran.isEmpty()) parts << ran;
        if (errors + warnings == 0) parts << (fx.result.clean() && didRun ? tr("nothing wrong found.") : QString());
        else parts << tr("%1 error(s), %2 warning(s) left.").arg(errors).arg(warnings);
        parts.removeAll(QString());
        summary_->setText(parts.isEmpty() ? tr("Nothing wrong found.") : parts.join("  "));
        const QColor colour[6] = {QColor("#2e8b57"), QColor("#0b6bcb"), QColor("#d13438"), QColor("#c77700"), QColor("#767676"), QColor("#9a7d0a")};
        const QString kind[6] = {tr("Fixed"), tr("Can fix"), tr("Error"), tr("Warning"), tr("Note"), tr("Fixed (check)")};
        for (size_t i = 0; i < rows_.size(); i++) {
            const Row& r = rows_[i];
            auto* it = new QTreeWidgetItem({kind[r.kind], r.at ? QString::number((qulonglong)r.at) : QString("-"), r.where, r.title});
            it->setData(0, Qt::UserRole, (int)i);
            it->setForeground(0, colour[r.kind]);
            QFont b = it->font(0);
            b.setBold(r.kind != Row::Note);
            it->setFont(0, b);
            it->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
            list_->addTopLevelItem(it);
        }
        for (int c = 0; c < 3; c++) list_->resizeColumnToContents(c);
        if (list_->topLevelItemCount()) list_->setCurrentItem(list_->topLevelItem(0));
        busy_ = false;
        buttons();
    }

    AppContext& ctx_;
    AutoDebugReport report_;
    std::vector<Row> rows_;
    bool busy_ = false;
    QPushButton *fix_, *run_, *show_, *replay_;
    QSpinBox* cycles_;
    QCheckBox* auto_;
    QLabel *summary_, *detail_;
    QTreeWidget* list_;
    QTimer* timer_;
    ConnectionBag conns_;
};

void registerDebugPanels(PanelRegistry& r) {
    r.add({"state", QT_TRANSLATE_NOOP("QObject", "State"), Qt::RightDockWidgetArea, false, [](AppContext& c) -> QWidget* { return new StatePanel(c); }});
    r.add({"memory", QT_TRANSLATE_NOOP("QObject", "Memory"), Qt::RightDockWidgetArea, false, [](AppContext& c) -> QWidget* { return new MemoryPanel(c); }});
    r.add({"signals", QT_TRANSLATE_NOOP("QObject", "Signals"), Qt::RightDockWidgetArea, false, [](AppContext& c) -> QWidget* { return new SignalsPanel(c); }});
    r.add({"csrs", QT_TRANSLATE_NOOP("QObject", "CSRs"), Qt::RightDockWidgetArea, false, [](AppContext& c) -> QWidget* { return new CsrPanel(c); }});
    r.add({"watch", QT_TRANSLATE_NOOP("QObject", "Watch"), Qt::RightDockWidgetArea, false, [](AppContext& c) -> QWidget* { return new WatchPanel(c); }});
    r.add({"callstack", QT_TRANSLATE_NOOP("QObject", "Call Stack"), Qt::BottomDockWidgetArea, false, [](AppContext& c) -> QWidget* { return new CallStackPanel(c); }});
    r.add({"breakpoints", QT_TRANSLATE_NOOP("QObject", "Breakpoints"), Qt::BottomDockWidgetArea, false, [](AppContext& c) -> QWidget* { return new BreakpointsPanel(c); }});
    r.add({"events", QT_TRANSLATE_NOOP("QObject", "Events"), Qt::BottomDockWidgetArea, false, [](AppContext& c) -> QWidget* { return new EventsPanel(c); }});
    r.add({"autodebug", QT_TRANSLATE_NOOP("QObject", "Auto Debug"), Qt::BottomDockWidgetArea, false, [](AppContext& c) -> QWidget* { return new AutoDebugPanel(c); }});
}

}  // namespace acide
