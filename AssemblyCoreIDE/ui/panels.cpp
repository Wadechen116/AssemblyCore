// The built-in panels. Each is a small widget that listens to the AppContext and shows one aspect of the newest build.
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
        table_ = new QTableWidget(0, 4);
        table_->setHorizontalHeaderLabels({tr("Code"), tr("Message"), tr("File"), tr("Line")});
        table_->horizontalHeader()->setStretchLastSection(false);
        table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
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
    }
    void refill() {
        table_->setRowCount(0);
        int e = 0, w = 0;
        if (snap_) {
            for (const asmcore::Diagnostic& d : snap_->result.diags) {
                bool isErr = d.sev == asmcore::Diagnostic::Error;
                (isErr ? e : w)++;
                if (isErr ? !errors_->isChecked() : !warnings_->isChecked()) continue;
                QString msg = fromStd(d.msg), file = shortName(d.loc.file);
                QString f = filter_->text();
                if (!f.isEmpty() && !fromStd(d.code).contains(f, Qt::CaseInsensitive) && !msg.contains(f, Qt::CaseInsensitive) && !file.contains(f, Qt::CaseInsensitive)) continue;
                int r = table_->rowCount();
                table_->insertRow(r);
                auto* c0 = new QTableWidgetItem(fromStd(d.code));
                c0->setForeground(isErr ? QColor("#f14c4c") : QColor("#cca700"));
                c0->setData(Qt::UserRole + 1, fromStd(d.loc.file));
                c0->setData(Qt::UserRole + 2, d.loc.line);
                c0->setData(Qt::UserRole + 3, d.loc.col);
                c0->setData(Qt::UserRole + 4, d.loc.endCol);
                table_->setItem(r, 0, c0);
                table_->setItem(r, 1, new QTableWidgetItem(msg));
                table_->setItem(r, 2, new QTableWidgetItem(file));
                table_->setItem(r, 3, new QTableWidgetItem(QString::number(d.loc.line)));
            }
            for (const std::string& n : snap_->notes) {
                int r = table_->rowCount();
                table_->insertRow(r);
                table_->setItem(r, 0, new QTableWidgetItem("note"));
                table_->setItem(r, 1, new QTableWidgetItem(fromStd(n)));
                table_->setItem(r, 2, new QTableWidgetItem(QString()));
                table_->setItem(r, 3, new QTableWidgetItem(QString()));
            }
        }
        summary_->setText(tr("%1 error(s), %2 warning(s)").arg(e).arg(w));
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
    explicit ListingPanel(AppContext& ctx) : ctx_(ctx) {
        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(2, 2, 2, 2);
        table_ = new QTableWidget(0, 6);
        table_->setObjectName("listingTable");
        table_->setHorizontalHeaderLabels({tr("Dom"), tr("PC"), tr("Bytes"), tr("Cyc"), tr("Instruction"), tr("Line")});
        table_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
        table_->verticalHeader()->hide();
        table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table_->setFont(editorFont(ctx.settings));
        lay->addWidget(table_);
        connect(table_, &QTableWidget::cellClicked, this, [this](int row, int) { follow_ = false; open(row); follow_ = true; });
        conns_.add(ctx.snapshotChanged, [this](SnapshotPtr s) { fill(s); });
        // the caret of an editor selects the instructions of its line
        conns_.add(ctx.cursorMoved, [this](const std::string& file, int line) { select(file, line); });
        fill(ctx.snapshot);
    }

private:
    void open(int row) {
        auto* it = table_->item(row, 5);
        if (!it) return;
        Location l;
        l.file = toStd(it->data(Qt::UserRole + 1).toString());
        l.line = it->data(Qt::UserRole).toInt();
        if (l.line > 0) ctx_.goTo.fire(l);
    }
    void fill(const SnapshotPtr& s) {
        snap_ = s;
        table_->setRowCount(0);
        if (!s) return;
        table_->setRowCount((int)s->result.instrs.size());
        int r = 0;
        for (const asmcore::InstrInfo& in : s->result.instrs) {
            const asmcore::DomainImage& im = s->result.image(in.domain);
            QString bytes;
            for (uint32_t i = 0; i < in.length && in.pc + i < im.bytes.size(); i++) bytes += QString("%1 ").arg(im.bytes[in.pc + i], 2, 16, QChar('0')).toUpper();
            QString text = fromStd(in.text);
            if (!in.frames.empty()) text += "    ; " + fromStd(in.frames.back().proc) + " @ line " + QString::number(in.frames.back().callSite.line);
            if (in.synthetic) text += "    ; generated";
            table_->setItem(r, 0, new QTableWidgetItem(in.domain == asmcore::Domain::Main ? "M" : "P"));
            table_->setItem(r, 1, new QTableWidgetItem(QString("%1").arg(in.pc, 3, 16, QChar('0')).toUpper()));
            table_->setItem(r, 2, new QTableWidgetItem(bytes.trimmed()));
            table_->setItem(r, 3, new QTableWidgetItem(QString::number(in.cycles)));
            table_->setItem(r, 4, new QTableWidgetItem(text));
            auto* ln = new QTableWidgetItem(QString::number(in.loc.line));
            ln->setData(Qt::UserRole, in.loc.line);
            ln->setData(Qt::UserRole + 1, fromStd(in.loc.file));
            table_->setItem(r, 5, ln);
            r++;
        }
        table_->resizeColumnsToContents();
    }
    void select(const std::string& file, int line) {
        if (!follow_ || !snap_) return;
        table_->clearSelection();
        int first = -1;
        for (int r = 0; r < table_->rowCount(); r++) {
            auto* it = table_->item(r, 5);
            if (it && it->data(Qt::UserRole).toInt() == line && toStd(it->data(Qt::UserRole + 1).toString()) == file) {
                table_->selectRow(r);
                if (first < 0) first = r;
            }
        }
        if (first >= 0) table_->scrollToItem(table_->item(first, 0));
    }

    AppContext& ctx_;
    SnapshotPtr snap_;
    QTableWidget* table_;
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
        auto group = [&](const QString& name, const std::vector<std::string>& v) {
            if (v.empty()) return;
            auto* g = new QTreeWidgetItem(root, {name});
            g->setExpanded(true);
            for (const std::string& f : v) {
                auto* it = new QTreeWidgetItem(g, {fromStd(f)});
                it->setData(0, Qt::UserRole, fromStd(p.absolute(f)));
            }
        };
        group(tr("Sources"), p.sources);
        group(tr("Scenarios"), p.scenarios);
        if (!p.hardware.empty()) group(tr("Hardware"), {p.hardware});
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
            QString rel = (*it)->text(0);
            if (rel.endsWith(QString::fromUtf8(" \xE2\x80\xA2"))) rel.chop(2);
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
