// The built-in panels. Each is a small widget that listens to the AppContext and shows one aspect of the newest build.
#include <map>
#include <QAbstractItemView>
#include <QCoreApplication>
#include <QCheckBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QTreeWidgetItemIterator>
#include <QHeaderView>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPlainTextEdit>
#include <QTableWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "block_link.h"
#include "panel_registry.h"
#include "text_util.h"
#include "theme.h"

namespace acide {

namespace {

QString shortName(const std::string& path) { return QFileInfo(fromStd(path)).fileName(); }

std::string lineOf(AppContext& ctx, const std::string& file, int line) {
    Document* d = ctx.workspace.find(file);
    std::string text;
    if (d) text = d->text;
    else if (!ctx.workspace.fileSystem()->read(file, text)) return "";
    size_t at = 0;
    for (int n = 1; n < line; n++) { at = text.find('\n', at); if (at == std::string::npos) return ""; at++; }
    size_t e = text.find('\n', at);
    std::string l = text.substr(at, e == std::string::npos ? std::string::npos : e - at);
    if (!l.empty() && l.back() == '\r') l.pop_back();
    size_t b = l.find_first_not_of(" \t");
    return b == std::string::npos ? "" : l.substr(b);
}

// ---- problems -------------------------------------------------------------------------------------------------------------------------
class ProblemsPanel : public QWidget {
    Q_DECLARE_TR_FUNCTIONS(ProblemsPanel)
public:
    explicit ProblemsPanel(AppContext& ctx) : ctx_(ctx) {
        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(2, 2, 2, 2);
        auto* bar = new QHBoxLayout;
        errors_ = new QCheckBox(tr("Errors"));
        warnings_ = new QCheckBox(tr("Warnings"));
        errors_->setChecked(true);
        warnings_->setChecked(true);
        filter_ = new QLineEdit;
        filter_->setPlaceholderText(tr("filter (code, text, file)"));
        filter_->setClearButtonEnabled(true);
        summary_ = new QLabel;
        bar->addWidget(errors_);
        bar->addWidget(warnings_);
        bar->addWidget(filter_, 1);
        bar->addWidget(summary_);
        lay->addLayout(bar);
        // Kind: the build's message, or a problem found while debugging -- by the decoder (A / B, before anything runs) or when it ran (a fault A / B / C)
        table_ = new QTableWidget(0, 5);
        table_->setHorizontalHeaderLabels({tr("Kind"), tr("Code"), tr("Message"), tr("File"), tr("Line")});
        table_->horizontalHeader()->setStretchLastSection(false);
        table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
        table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        table_->verticalHeader()->hide();
        table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table_->setObjectName("problemsTable");
        lay->addWidget(table_);
        connect(errors_, &QCheckBox::toggled, this, [this] { refill(); });
        connect(warnings_, &QCheckBox::toggled, this, [this] { refill(); });
        connect(filter_, &QLineEdit::textChanged, this, [this] { refill(); });
        connect(table_, &QTableWidget::cellActivated, this, [this](int row, int) { open(row); });
        connect(table_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) { open(row); });
        conns_.add(ctx.snapshotChanged, [this](SnapshotPtr s) { snap_ = s; refill(); });
        conns_.add(ctx.debugger.runtimeProblemsChanged, [this] { refill(); });
        snap_ = ctx.snapshot;
        refill();
    }

private:
    void open(int row) {
        auto* it = table_->item(row, 0);
        if (!it) return;
        Location l;
        l.file = toStd(it->data(Qt::UserRole + 1).toString());
        l.line = it->data(Qt::UserRole + 2).toInt();
        l.col = it->data(Qt::UserRole + 3).toInt();
        l.endCol = it->data(Qt::UserRole + 4).toInt();
        if (l.line > 0) ctx_.goTo.fire(l);
        else if (it->data(Qt::UserRole + 5).toBool()) ctx_.showPanel.fire("decoder");      // a problem of a burned image without its source: the decoder's view shows it
    }
public:
    static QString kindText(const RuntimeProblem& p) {
        const QString what = p.cls == 'A' ? tr("not a legal encoding") : p.cls == 'B' ? tr("breaks a rule of the spec") : tr("the state when it ran");
        return (p.fault ? tr("Fault") : tr("Decoder")) + QString(" %1 \xC2\xB7 ").arg(QChar(p.cls)) + what;
    }
private:
    void refill() {
        table_->setRowCount(0);
        int e = 0, w = 0;
        const QString f = filter_->text();
        // the problems found while debugging come first: they are about what runs now
        int cls[3] = {0, 0, 0};
        for (const RuntimeProblem& p : ctx_.debugger.runtimeProblems()) {
            cls[p.cls == 'A' ? 0 : p.cls == 'B' ? 1 : 2]++;
            e++;
            if (!errors_->isChecked()) continue;
            const QString kind = kindText(p), msg = fromStd(p.msg), file = p.file.empty() ? QString() : shortName(p.file);
            const QString where = QString("%1 pc 0x%2").arg(p.domain == asmcore::Domain::Main ? "Main" : "Peri").arg(p.pc, 2, 16, QChar('0'));
            if (!f.isEmpty() && !fromStd(p.code).contains(f, Qt::CaseInsensitive) && !msg.contains(f, Qt::CaseInsensitive) && !file.contains(f, Qt::CaseInsensitive) && !kind.contains(f, Qt::CaseInsensitive)) continue;
            const int r = table_->rowCount();
            table_->insertRow(r);
            auto* c0 = new QTableWidgetItem(kind);
            c0->setForeground(p.cls == 'B' ? QColor("#e08a2c") : QColor("#f14c4c"));
            c0->setData(Qt::UserRole + 1, fromStd(p.file));
            c0->setData(Qt::UserRole + 2, p.line);
            c0->setData(Qt::UserRole + 5, true);
            const QString tip = tr("%1\n%2: %3\nThe decoder made of the bytes: %4").arg(kind, where, msg, fromStd(p.instruction));
            c0->setToolTip(tip);
            table_->setItem(r, 0, c0);
            table_->setItem(r, 1, new QTableWidgetItem(fromStd(p.code)));
            auto* m = new QTableWidgetItem(where + ": " + msg + (p.instruction.empty() ? QString() : "   [" + fromStd(p.instruction) + "]"));
            m->setToolTip(tip);
            table_->setItem(r, 2, m);
            table_->setItem(r, 3, new QTableWidgetItem(file.isEmpty() ? tr("(no source: see Decoder)") : file));
            table_->setItem(r, 4, new QTableWidgetItem(p.line > 0 ? QString::number(p.line) : QString()));
        }
        if (snap_) {
            for (const asmcore::Diagnostic& d : snap_->result.diags) {
                bool isErr = d.sev == asmcore::Diagnostic::Error;
                (isErr ? e : w)++;
                if (isErr ? !errors_->isChecked() : !warnings_->isChecked()) continue;
                QString msg = fromStd(d.msg), file = shortName(d.loc.file);
                if (!f.isEmpty() && !fromStd(d.code).contains(f, Qt::CaseInsensitive) && !msg.contains(f, Qt::CaseInsensitive) && !file.contains(f, Qt::CaseInsensitive)) continue;
                int r = table_->rowCount();
                table_->insertRow(r);
                auto* c0 = new QTableWidgetItem(tr("Build"));
                c0->setForeground(isErr ? QColor("#f14c4c") : QColor("#cca700"));
                c0->setData(Qt::UserRole + 1, fromStd(d.loc.file));
                c0->setData(Qt::UserRole + 2, d.loc.line);
                c0->setData(Qt::UserRole + 3, d.loc.col);
                c0->setData(Qt::UserRole + 4, d.loc.endCol);
                table_->setItem(r, 0, c0);
                auto* code = new QTableWidgetItem(fromStd(d.code));
                code->setForeground(isErr ? QColor("#f14c4c") : QColor("#cca700"));
                table_->setItem(r, 1, code);
                table_->setItem(r, 2, new QTableWidgetItem(msg));
                table_->setItem(r, 3, new QTableWidgetItem(file));
                table_->setItem(r, 4, new QTableWidgetItem(QString::number(d.loc.line)));
            }
            for (const std::string& n : snap_->notes) {
                int r = table_->rowCount();
                table_->insertRow(r);
                table_->setItem(r, 0, new QTableWidgetItem(tr("Build")));
                table_->setItem(r, 1, new QTableWidgetItem("note"));
                table_->setItem(r, 2, new QTableWidgetItem(fromStd(n)));
                table_->setItem(r, 3, new QTableWidgetItem(QString()));
                table_->setItem(r, 4, new QTableWidgetItem(QString()));
            }
        }
        QString sum = tr("%1 error(s), %2 warning(s)").arg(e).arg(w);
        if (cls[0] + cls[1] + cls[2]) sum += "  \xC2\xB7  " + tr("while debugging: A %1, B %2, C %3").arg(cls[0]).arg(cls[1]).arg(cls[2]);
        summary_->setText(sum);
    }

    AppContext& ctx_;
    SnapshotPtr snap_;
    QCheckBox *errors_, *warnings_;
    QLineEdit* filter_;
    QLabel* summary_;
    QTableWidget* table_;
    ConnectionBag conns_;
};

// ---- outline --------------------------------------------------------------------------------------------------------------------------
class OutlinePanel : public QWidget {
    Q_DECLARE_TR_FUNCTIONS(OutlinePanel)
public:
    explicit OutlinePanel(AppContext& ctx) : ctx_(ctx) {
        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(2, 2, 2, 2);
        tree_ = new QTreeWidget;
        tree_->setHeaderLabels({tr("Symbol"), tr("Detail")});
        tree_->setObjectName("outlineTree");
        lay->addWidget(tree_);
        connect(tree_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* it, int) {
            if (it->data(0, Qt::UserRole).toInt() <= 0) return;
            Location l;
            l.file = toStd(it->data(0, Qt::UserRole + 1).toString());
            l.line = it->data(0, Qt::UserRole).toInt();
            l.col = it->data(0, Qt::UserRole + 2).toInt();
            l.endCol = it->data(0, Qt::UserRole + 3).toInt();
            ctx_.goTo.fire(l);
        });
        conns_.add(ctx.snapshotChanged, [this](SnapshotPtr s) { fill(s); });
        fill(ctx.snapshot);
    }

private:
    void fill(const SnapshotPtr& s) {
        tree_->clear();
        if (!s) return;
        static const char* order[] = {"proc", "label", "equ", "csr", "service", "const", "devmap", "wcet"};
        static const char* names[] = {QT_TR_NOOP("Procedures"), QT_TR_NOOP("Labels"), QT_TR_NOOP("Constants"), QT_TR_NOOP("CSRs"), QT_TR_NOOP("Services"), QT_TR_NOOP("Data"), QT_TR_NOOP("Devices"), QT_TR_NOOP("WCET queries")};
        for (size_t k = 0; k < sizeof order / sizeof *order; k++) {
            QTreeWidgetItem* group = nullptr;
            for (const asmcore::IndexSymbol& sym : s->index.symbols()) {
                if (sym.kind != order[k]) continue;
                if (!group) { group = new QTreeWidgetItem(tree_, {tr(names[k])}); group->setExpanded(true); }
                QString detail = sym.scope.empty() ? QString() : fromStd(sym.scope);
                if (sym.kind == "proc")
                    for (const asmcore::ProcInfo& p : s->result.procs) if (p.name == sym.name) detail += QString("  %1 call site(s)").arg(p.callSites);
                if (sym.kind == "equ" || sym.kind == "csr" || sym.kind == "service")
                    for (const asmcore::SymbolInfo& si : s->result.symbols) if (si.name == sym.name) detail = QString("= %1 (0x%2)").arg(si.value).arg(si.value, 0, 16);
                auto* it = new QTreeWidgetItem(group, {fromStd(sym.name), detail.trimmed()});
                it->setData(0, Qt::UserRole, sym.def.line);
                it->setData(0, Qt::UserRole + 1, fromStd(sym.def.file));
                it->setData(0, Qt::UserRole + 2, sym.def.col);
                it->setData(0, Qt::UserRole + 3, sym.def.endCol);
            }
        }
        tree_->resizeColumnToContents(0);
    }

    AppContext& ctx_;
    QTreeWidget* tree_;
    ConnectionBag conns_;
};

// ---- listing --------------------------------------------------------------------------------------------------------------------------
class ListingPanel : public QWidget {
    Q_DECLARE_TR_FUNCTIONS(ListingPanel)
public:
    // the columns
    enum { kMark = 0, kDom, kPc, kBytes, kCyc, kHits, kText, kLine, kColumns };

    // The colours of the rows go with the table, not with the editor: the panels keep the system's (light or dark) look whatever the editor theme is, and the
    // "Main is here" colour of a dark editor under the black text of a light table cannot be read.
    static Theme rowColours(const QWidget* w) { return w->palette().color(QPalette::Base).lightness() > 128 ? Theme::light() : Theme::dark(); }

    explicit ListingPanel(AppContext& ctx) : ctx_(ctx), theme_(rowColours(this)) {
        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(2, 2, 2, 2);
        // what the listing shows: the newest build, or -- while debugging -- the program that is running, with what the simulator says about it
        status_ = new QLabel;
        status_->setObjectName("listingStatus");
        status_->setWordWrap(true);
        lay->addWidget(status_);
        table_ = new QTableWidget(0, kColumns);
        table_->setObjectName("listingTable");
        table_->setHorizontalHeaderLabels({QString(), tr("Dom"), tr("PC"), tr("Bytes"), tr("Cyc"), tr("Runs"), tr("Instruction"), tr("Line")});
        table_->horizontalHeaderItem(kMark)->setToolTip(tr("Where Main (M) and Peri (P) are, breakpoints, faults, and code that differs from what the decoder fetched"));
        table_->horizontalHeaderItem(kCyc)->setToolTip(tr("The assembler's planning figure for the instruction (not measured)"));
        table_->horizontalHeaderItem(kHits)->setToolTip(tr("How many times the simulator has executed the instruction in this session"));
        table_->horizontalHeader()->setSectionResizeMode(kText, QHeaderView::Stretch);
        table_->verticalHeader()->hide();
        table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table_->setFont(editorFont(ctx.settings));
        lay->addWidget(table_);
        connect(table_, &QTableWidget::cellClicked, this, [this](int row, int) { follow_ = false; open(row); follow_ = true; });
        conns_.add(ctx.snapshotChanged, [this](SnapshotPtr) { refresh(ctx_.debugger.session().view(), false); });
        // the caret of an editor selects the instructions of its line
        conns_.add(ctx.cursorMoved, [this](const std::string& file, int line) { select(file, line); });
        // the simulator: where the sequencers are, what ran how often, breakpoints, faults, what the decoder fetched
        conns_.add(ctx.debugger.updated, [this](const SimUpdate& u) { refresh(u.view, u.state == SimState::Paused); });
        conns_.add(ctx.debugger.focusChanged, [this](asmcore::Domain) { refresh(ctx_.debugger.session().view(), true); });
        refresh(ctx.debugger.session().view(), false);
    }

private:
    void open(int row) {
        auto* it = table_->item(row, kLine);
        if (!it) return;
        Location l;
        l.file = toStd(it->data(Qt::UserRole + 1).toString());
        l.line = it->data(Qt::UserRole).toInt();
        if (l.line > 0) ctx_.goTo.fire(l);
    }

    // The program on show: while a session is on, the one that is being debugged (not the newest build, which may be of a later text); else the newest build.
    SnapshotPtr wanted() const {
        if (ctx_.debugger.active()) if (SnapshotPtr p = ctx_.debugger.session().program()) return p;
        return ctx_.snapshot;
    }

    void fill(const SnapshotPtr& s) {
        snap_ = s;
        rowOf_[0].clear();
        rowOf_[1].clear();
        shownHits_.clear();
        shownMark_.clear();
        shownLook_.clear();
        table_->setRowCount(0);
        if (!s) return;
        const int n = (int)s->result.instrs.size();
        table_->setRowCount(n);
        shownHits_.assign((size_t)n, -1);
        shownMark_.assign((size_t)n, QString());
        shownLook_.assign((size_t)n, 0);
        int r = 0;
        for (const asmcore::InstrInfo& in : s->result.instrs) {
            const asmcore::DomainImage& im = s->result.image(in.domain);
            QString bytes;
            for (uint32_t i = 0; i < in.length && in.pc + i < im.bytes.size(); i++) bytes += QString("%1 ").arg(im.bytes[in.pc + i], 2, 16, QChar('0')).toUpper();
            QString text = fromStd(in.text);
            if (!in.frames.empty()) text += "    ; " + fromStd(in.frames.back().proc) + " @ line " + QString::number(in.frames.back().callSite.line);
            if (in.synthetic) text += "    ; generated";
            auto* mark = new QTableWidgetItem;
            mark->setTextAlignment(Qt::AlignCenter);
            table_->setItem(r, kMark, mark);
            table_->setItem(r, kDom, new QTableWidgetItem(in.domain == asmcore::Domain::Main ? "M" : "P"));
            table_->setItem(r, kPc, new QTableWidgetItem(QString("%1").arg(in.pc, 3, 16, QChar('0')).toUpper()));
            auto* by = new QTableWidgetItem(bytes.trimmed());
            by->setData(Qt::UserRole, bytes.trimmed());                    // the build's bytes (the cell shows the fetched ones when they differ)
            table_->setItem(r, kBytes, by);
            table_->setItem(r, kCyc, new QTableWidgetItem(QString::number(in.cycles)));
            auto* hits = new QTableWidgetItem;
            hits->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            table_->setItem(r, kHits, hits);
            table_->setItem(r, kText, new QTableWidgetItem(text));
            auto* ln = new QTableWidgetItem(QString::number(in.loc.line));
            ln->setData(Qt::UserRole, in.loc.line);
            ln->setData(Qt::UserRole + 1, fromStd(in.loc.file));
            table_->setItem(r, kLine, ln);
            rowOf_[in.domain == asmcore::Domain::Main ? 0 : 1][in.pc] = r;
            r++;
        }
        table_->resizeColumnsToContents();
        // (room for a breakpoint, both sequencers and the "differs" sign on one row, so that the marks are never cut off)
        table_->setColumnWidth(kMark, QFontMetrics(table_->font()).horizontalAdvance(QString::fromUtf8("\xE2\x97\x8FM\xE2\x96\xB6P\xE2\x96\xB6\xE2\x89\xA0")) + 14);
    }

    // Everything the simulator knows about the rows. Only rows whose look changes are touched (this runs about 30 times a second while the program runs).
    void refresh(const ViewPtr& view, bool reveal) {
        const SnapshotPtr want = wanted();
        if (want != snap_) fill(want);
        const bool live = ctx_.debugger.active() && view && snap_ && ctx_.debugger.session().program() == snap_;
        const int n = table_->rowCount();
        // per row: the marks, how often it ran, how it is drawn (bit 0 Main is here, 1 Peri is here, 2 a fault, 3 never ran, 4 differs from what was fetched)
        std::vector<QString> mark((size_t)n);
        std::vector<long long> hits((size_t)n, -1);
        std::vector<int> look((size_t)n, 0);
        std::map<int, QString> fetchedBytes, tips;
        int differs = 0, focusRow = -1;
        if (live) {
            for (int d = 0; d < 2; d++) {
                for (const auto& kv : rowOf_[d]) {
                    const long long h = kv.first < view->hits[d].size() ? (long long)view->hits[d][kv.first] : 0;
                    hits[(size_t)kv.second] = h;
                    if (h == 0) look[(size_t)kv.second] |= 8;
                }
                for (const asmcore::sim::DecodedLine& dl : view->decoded[d]) {
                    if (dl.matches && dl.problemClass() == 0) continue;
                    const auto it = rowOf_[d].find(dl.pc);
                    if (it == rowOf_[d].end()) continue;
                    differs++;
                    look[(size_t)it->second] |= 16;
                    mark[(size_t)it->second] += QString::fromUtf8("\xE2\x89\xA0");                          // not equal
                    QString fb;
                    for (uint8_t b : dl.bytes) fb += QString("%1 ").arg(b, 2, 16, QChar('0')).toUpper();
                    fetchedBytes[it->second] = fb.trimmed();
                    QString tip = tr("The decoder fetched: %1  (%2)").arg(fb.trimmed(), fromStd(dl.text));
                    if (!dl.problem.empty()) tip += "\n" + fromStd(dl.problem);
                    if (!dl.specProblem.empty()) tip += "\n" + fromStd(dl.specProblem);
                    tips[it->second] = tip;
                }
            }
            for (const asmcore::sim::Breakpoint& bp : view->breakpoints) {
                if (bp.oneShot) continue;
                const int d = bp.domain == asmcore::Domain::Main ? 0 : 1;
                const auto it = rowOf_[d].find(bp.pc);
                if (it != rowOf_[d].end()) mark[(size_t)it->second] += bp.enabled ? QString::fromUtf8("\xE2\x97\x8F") : QString::fromUtf8("\xE2\x97\x8B");      // a dot, hollow when off
            }
            for (int d = 0; d < 2; d++) {
                const auto it = rowOf_[d].find(view->pc[d]);
                if (it == rowOf_[d].end()) continue;
                const bool fault = view->faulted[d];
                look[(size_t)it->second] |= fault ? 4 : (d == 0 ? 1 : 2);
                // an arrow where it is about to execute, a square when it has stopped for good, a cross on a fault
                mark[(size_t)it->second] += QString(d == 0 ? "M" : "P") + (fault ? QString::fromUtf8("\xE2\x9C\x96") : view->halted[d] ? QString::fromUtf8("\xE2\x96\xA0") : QString::fromUtf8("\xE2\x96\xB6"));
                if ((ctx_.debugger.focus() == asmcore::Domain::Main) == (d == 0)) focusRow = it->second;
            }
        }
        const QColor dim = theme_.lineNumberFg, normal = table_->palette().color(QPalette::Text);
        for (int r = 0; r < n; r++) {
            const size_t i = (size_t)r;
            if (hits[i] != shownHits_[i]) {
                table_->item(r, kHits)->setText(hits[i] < 0 ? QString() : QString::number(hits[i]));
                shownHits_[i] = hits[i];
            }
            if (mark[i] != shownMark_[i]) {
                table_->item(r, kMark)->setText(mark[i]);
                shownMark_[i] = mark[i];
            }
            if (look[i] != shownLook_[i]) {
                const int l = look[i];
                QColor faultBack = theme_.error;
                faultBack.setAlpha(90);
                const QBrush back = (l & 4) ? QBrush(faultBack) : (l & 1) ? QBrush(theme_.execMain) : (l & 2) ? QBrush(theme_.execPeri) : QBrush();
                const QColor fore = (l & 8) && !(l & 7) ? dim : normal;                   // never ran: faded (code that the run did not reach)
                for (int c = 0; c < kColumns; c++) {
                    QTableWidgetItem* it = table_->item(r, c);
                    if (l & 7) it->setSelected(false);
                    it->setBackground(back);
                    it->setForeground(c == kBytes && (l & 16) ? theme_.warning : fore);
                }
                QTableWidgetItem* by = table_->item(r, kBytes);
                by->setText((l & 16) && fetchedBytes.count(r) ? fetchedBytes[r] : by->data(Qt::UserRole).toString());
                const QString tip = (l & 16) && tips.count(r) ? tips[r] + "\n" + tr("The build has: %1").arg(by->data(Qt::UserRole).toString()) : QString();
                by->setToolTip(tip);
                table_->item(r, kMark)->setToolTip(tip);
                shownLook_[i] = l;
            }
        }
        // what is on show, in words
        QString st;
        if (!snap_) st = tr("Nothing is built yet.");
        else if (!live) st = tr("The newest build. Start debugging to see where the program is and what has run.");
        else {
            st = tr("The program being debugged, cycle %1. M / P: where Main and Peri are; Runs: how often each instruction has executed.").arg((qulonglong)view->cycle);
            if (ctx_.debugger.stale()) st += " " + tr("The sources were changed since it was built: this is what is running, not what the editor shows.");
            if (differs > 0) st += " " + tr("%1 instruction(s) differ from what the decoder fetched from the EEPROM (marked, see the tooltip of the bytes).").arg(differs);
        }
        if (st != status_->text()) status_->setText(st);
        const bool alarm = live && (ctx_.debugger.stale() || differs > 0);
        if (alarm != alarm_) { alarm_ = alarm; status_->setStyleSheet(alarm ? QString("color: %1; font-weight: bold;").arg(theme_.warning.name()) : QString()); }
        // stopped: bring the row of the sequencer that is looked at into view
        if (reveal && focusRow >= 0 && focusRow != revealed_) table_->scrollToItem(table_->item(focusRow, kPc), QAbstractItemView::PositionAtCenter);
        if (reveal) revealed_ = focusRow;
    }

    void select(const std::string& file, int line) {
        if (!follow_ || !snap_) return;
        table_->clearSelection();
        int first = -1;
        for (int r = 0; r < table_->rowCount(); r++) {
            auto* it = table_->item(r, kLine);
            if (it && it->data(Qt::UserRole).toInt() == line && toStd(it->data(Qt::UserRole + 1).toString()) == file) {
                // (not the row a sequencer is on: the selection would paint over the colour that says so -- the editor is taken to that line at every stop)
                if ((size_t)r < shownLook_.size() && (shownLook_[(size_t)r] & 7)) { if (first < 0) first = r; continue; }
                table_->selectRow(r);
                if (first < 0) first = r;
            }
        }
        if (first >= 0) table_->scrollToItem(table_->item(first, kDom));
    }

    AppContext& ctx_;
    Theme theme_;
    SnapshotPtr snap_;
    QLabel* status_;
    QTableWidget* table_;
    std::map<uint32_t, int> rowOf_[2];                       // pc -> row, [0] Main [1] Peri
    std::vector<long long> shownHits_;
    std::vector<QString> shownMark_;
    std::vector<int> shownLook_;
    int revealed_ = -1;
    bool alarm_ = false;
    bool follow_ = true;
    ConnectionBag conns_;
};

// ---- output ---------------------------------------------------------------------------------------------------------------------------
class OutputPanel : public QPlainTextEdit {
public:
    explicit OutputPanel(AppContext& ctx) {
        setReadOnly(true);
        setObjectName("outputText");
        setFont(editorFont(ctx.settings));
        setMaximumBlockCount(5000);
        conns_.add(ctx.log, [this](const QString& line) { appendPlainText(line); });
    }

private:
    ConnectionBag conns_;
};

// ---- references / search results ---------------------------------------------------------------------------------------------------------
class ReferencesPanel : public QWidget {
    Q_DECLARE_TR_FUNCTIONS(ReferencesPanel)
public:
    explicit ReferencesPanel(AppContext& ctx) : ctx_(ctx) {
        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(2, 2, 2, 2);
        title_ = new QLabel(tr("No results"));
        list_ = new QListWidget;
        list_->setObjectName("referencesList");
        lay->addWidget(title_);
        lay->addWidget(list_);
        connect(list_, &QListWidget::itemActivated, this, [this](QListWidgetItem* it) {
            int i = it->data(Qt::UserRole).toInt();
            if (i >= 0 && i < (int)places_.size()) ctx_.goTo.fire(places_[(size_t)i]);
        });
        conns_.add(ctx.showReferences, [this](const QString& title, const std::vector<Location>& v) {
            places_ = v;
            title_->setText(title);
            list_->clear();
            for (size_t i = 0; i < v.size(); i++) {
                auto* it = new QListWidgetItem(QString("%1:%2   %3").arg(shortName(v[i].file)).arg(v[i].line).arg(fromStd(lineOf(ctx_, v[i].file, v[i].line))));
                it->setData(Qt::UserRole, (int)i);
                list_->addItem(it);
            }
            if (QWidget* dock = parentWidget()) { dock->show(); dock->raise(); }
        });
    }

private:
    AppContext& ctx_;
    QLabel* title_;
    QListWidget* list_;
    std::vector<Location> places_;
    ConnectionBag conns_;
};

// ---- project --------------------------------------------------------------------------------------------------------------------------
class ProjectPanel : public QWidget {
    Q_DECLARE_TR_FUNCTIONS(ProjectPanel)
public:
    explicit ProjectPanel(AppContext& ctx) : ctx_(ctx) {
        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(2, 2, 2, 2);
        tree_ = new QTreeWidget;
        tree_->setHeaderHidden(true);
        tree_->setObjectName("projectTree");
        lay->addWidget(tree_);
        connect(tree_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* it, int) {
            QString p = it->data(0, Qt::UserRole).toString();
            if (!p.isEmpty()) { Location l; l.file = toStd(p); ctx_.goTo.fire(l); }
        });
        conns_.add(ctx.workspace.projectChanged, [this] { fill(); });
        conns_.add(ctx.workspace.documentChanged, [this](const std::string&) { fillDirty(); });
        fill();
    }

private:
    void fill() {
        tree_->clear();
        const Project& p = ctx_.workspace.project();
        if (!ctx_.workspace.hasProject()) return;
        auto* root = new QTreeWidgetItem(tree_, {fromStd(p.name)});
        root->setExpanded(true);
        const std::map<std::string, std::string> generated = generatedFiles(ctx_.workspace);       // generated file -> its block program
        auto group = [&](const QString& name, const std::vector<std::string>& v) {
            if (v.empty()) return;
            auto* g = new QTreeWidgetItem(root, {name});
            g->setExpanded(true);
            for (const std::string& f : v) {
                const QString label = fromStd(f);
                auto* it = new QTreeWidgetItem(g, {label});
                it->setData(0, Qt::UserRole, fromStd(p.absolute(f)));
                it->setData(0, Qt::UserRole + 1, label);
                auto gi = generated.find(p.absolute(f));
                if (gi != generated.end()) {                                                           // generated: not edited as text
                    QFont font = it->font(0);
                    font.setItalic(true);
                    it->setFont(0, font);
                    it->setToolTip(0, tr("Generated from %1 - edit the blocks, not this file").arg(QFileInfo(fromStd(gi->second)).fileName()));
                    it->setText(0, label + tr("  (generated)"));
                    it->setData(0, Qt::UserRole + 1, label + tr("  (generated)"));
                }
            }
        };
        group(tr("Sources"), p.sources);
        group(tr("Blocks"), p.blocks);
        group(tr("Scenarios"), p.scenarios);
        if (!p.hardware.empty()) group(tr("Hardware"), {p.hardware});
        if (!p.simParams.empty()) group(tr("Simulator parameters"), {p.simParams});
        std::vector<std::string> misc;
        if (!p.profile.empty()) misc.push_back(p.profile);
        if (!p.environment.empty()) misc.push_back(p.environment);
        group(tr("Profile / environment"), misc);
        fillDirty();
    }
    void fillDirty() {
        // mark unsaved sources with a bullet
        for (QTreeWidgetItemIterator it(tree_); *it; ++it) {
            QString path = (*it)->data(0, Qt::UserRole).toString();
            if (path.isEmpty()) continue;
            Document* d = ctx_.workspace.find(toStd(path));
            QString base = QFileInfo(path).fileName();
            QString rel = (*it)->data(0, Qt::UserRole + 1).toString();
            (*it)->setText(0, d && d->dirty ? rel + QString::fromUtf8(" \xE2\x80\xA2") : rel);
            (void)base;
        }
    }

    AppContext& ctx_;
    QTreeWidget* tree_;
    ConnectionBag conns_;
};

}  // namespace

void registerBuiltinPanels(PanelRegistry& r) {
    r.add({"project", QT_TRANSLATE_NOOP("QObject", "Project"), Qt::LeftDockWidgetArea, true, [](AppContext& c) -> QWidget* { return new ProjectPanel(c); }});
    r.add({"outline", QT_TRANSLATE_NOOP("QObject", "Outline"), Qt::LeftDockWidgetArea, true, [](AppContext& c) -> QWidget* { return new OutlinePanel(c); }});
    r.add({"problems", QT_TRANSLATE_NOOP("QObject", "Problems"), Qt::BottomDockWidgetArea, true, [](AppContext& c) -> QWidget* { return new ProblemsPanel(c); }});
    r.add({"output", QT_TRANSLATE_NOOP("QObject", "Output"), Qt::BottomDockWidgetArea, true, [](AppContext& c) -> QWidget* { return new OutputPanel(c); }});
    r.add({"references", QT_TRANSLATE_NOOP("QObject", "References"), Qt::BottomDockWidgetArea, false, [](AppContext& c) -> QWidget* { return new ReferencesPanel(c); }});
    r.add({"listing", QT_TRANSLATE_NOOP("QObject", "Listing"), Qt::RightDockWidgetArea, true, [](AppContext& c) -> QWidget* { return new ListingPanel(c); }});
    registerDebugPanels(r);
}

}  // namespace acide
