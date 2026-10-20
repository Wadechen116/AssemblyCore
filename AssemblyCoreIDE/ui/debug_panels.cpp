// Debugging panels. Each one listens to the Debugger of the AppContext, shows one aspect of the simulator state and, where it makes
// sense, lets the user change it (poke memory, set a signal, write a CSR, edit a breakpoint condition).
#include <QAbstractItemView>
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

void registerDebugPanels(PanelRegistry& r) {
    r.add({"state", QT_TRANSLATE_NOOP("QObject", "State"), Qt::RightDockWidgetArea, false, [](AppContext& c) -> QWidget* { return new StatePanel(c); }});
    r.add({"memory", QT_TRANSLATE_NOOP("QObject", "Memory"), Qt::RightDockWidgetArea, false, [](AppContext& c) -> QWidget* { return new MemoryPanel(c); }});
    r.add({"signals", QT_TRANSLATE_NOOP("QObject", "Signals"), Qt::RightDockWidgetArea, false, [](AppContext& c) -> QWidget* { return new SignalsPanel(c); }});
    r.add({"csrs", QT_TRANSLATE_NOOP("QObject", "CSRs"), Qt::RightDockWidgetArea, false, [](AppContext& c) -> QWidget* { return new CsrPanel(c); }});
    r.add({"watch", QT_TRANSLATE_NOOP("QObject", "Watch"), Qt::RightDockWidgetArea, false, [](AppContext& c) -> QWidget* { return new WatchPanel(c); }});
    r.add({"callstack", QT_TRANSLATE_NOOP("QObject", "Call Stack"), Qt::BottomDockWidgetArea, false, [](AppContext& c) -> QWidget* { return new CallStackPanel(c); }});
    r.add({"breakpoints", QT_TRANSLATE_NOOP("QObject", "Breakpoints"), Qt::BottomDockWidgetArea, false, [](AppContext& c) -> QWidget* { return new BreakpointsPanel(c); }});
    r.add({"events", QT_TRANSLATE_NOOP("QObject", "Events"), Qt::BottomDockWidgetArea, false, [](AppContext& c) -> QWidget* { return new EventsPanel(c); }});
}

}  // namespace acide
