// The simulator's parameter panel and the execution view: change a hardware parameter a little and see where, and how much, the two sequencers move.
//   * ParamsPanel: every parameter of asmcore/sim_params.h in groups, edited in place; where a value comes from (planning / measured / confirmed); the
//     .simparams file of the project (load, save).
//   * ExecutionPanel: the program runs in a simulator of its own (sim_compare.h); Main and Peri are two lanes on a cycle axis, the run of the INITIAL values (or a
//     saved run) lies under them as a ghost lane, a strip shows how far each lane has drifted, a table lists what moved. Zoom, pan, click an instruction to see its line.
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QShortcut>
#include <QSplitter>
#include <QGuiApplication>
#include <QScreen>
#include <QToolButton>
#include <QFrame>
#include <QSpinBox>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QTimer>
#include <QToolTip>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "app_context.h"
#include "document_tab.h"
#include "isa_top_view.h"
#include "sim_compare.h"
#include "sim_panels.h"
#include "sim_params_store.h"
#include "text_util.h"

namespace acide {

using asmcore::ParamDef;
using asmcore::SimParams;
using asmcore::paramCatalog;
using asmcore::sim::Cycle;

namespace {

QString groupName(const std::string& g) { return QCoreApplication::translate("SimParamGroup", g.c_str()); }

// ---- the parameters kept at hand ----------------------------------------------------------------------------------------------------
// The "pinned" parameters are the ones shown in the quick popup without searching (the settings keep the list; until it is first changed these are the ones).
std::vector<std::string> pinnedKeys(Settings& s) {
    if (s.getBool("simparams.pinned.set", false)) return s.getList("simparams.pinned");
    return {"sim.speed", "main.fetch_cycles", "peri.fetch_cycles", "csr.access_main", "csr.access_peri", "latency.alu", "timer.main.clock_div", "timer.peri.clock_div", "i2c.stretch", "i2ch.overhead", "reset.boot_cycles"};
}
bool isPinned(Settings& s, const std::string& key) {
    const std::vector<std::string> v = pinnedKeys(s);
    return std::find(v.begin(), v.end(), key) != v.end();
}
void setPinned(Settings& s, const std::string& key, bool on) {
    std::vector<std::string> v = pinnedKeys(s);
    v.erase(std::remove(v.begin(), v.end(), key), v.end());
    if (on) v.push_back(key);
    s.setList("simparams.pinned", v);
    s.setBool("simparams.pinned.set", true);
}

// How well a parameter fits what was typed: 0 not at all; the name starting with it is best, then the name containing it, then the group, the unit or the help.
int paramScore1(const ParamDef& d, const QString& f);
// Several words: every one of them has to fit (somewhere in the name, group, unit or help); the score is the worst of them.
int paramScore(const ParamDef& d, const QString& text) {
    const QStringList words = text.split(' ', Qt::SkipEmptyParts);
    if (words.isEmpty()) return 1;
    int worst = 4;
    for (const QString& w : words) worst = std::min(worst, paramScore1(d, w));
    return worst;
}
int paramScore1(const ParamDef& d, const QString& f) {
    const QString key = fromStd(d.key);
    if (key.startsWith(f, Qt::CaseInsensitive)) return 4;
    if (key.contains(f, Qt::CaseInsensitive)) return 3;
    if (groupName(d.group).contains(f, Qt::CaseInsensitive) || fromStd(d.group).contains(f, Qt::CaseInsensitive) || fromStd(d.unit).contains(f, Qt::CaseInsensitive)) return 2;
    if (fromStd(d.help).contains(f, Qt::CaseInsensitive) || fromStd(d.measure).contains(f, Qt::CaseInsensitive)) return 1;
    return 0;
}

// The editor of a value, wired to the store. Used by the quick popup (the table has its own delegate).
QWidget* valueEditor(SimParamsStore& store, const ParamDef& d, QWidget* parent) {
    const std::string key = d.key;
    auto apply = [&store, key](const std::string& text) { std::string e; store.set(key, text, e); };
    if (d.kind == ParamDef::Int) {
        auto* s = new QSpinBox(parent);
        s->setRange((int)std::max<int64_t>(d.lo, INT32_MIN), (int)std::min<int64_t>(d.hi, INT32_MAX));
        s->setKeyboardTracking(false);                                     // a value is taken when Enter is pressed or the box is left, not at every digit
        s->setValue((int)store.params().get(key));
        QObject::connect(s, QOverload<int>::of(&QSpinBox::valueChanged), s, [apply](int v) { apply(std::to_string(v)); });
        return s;
    }
    if (d.kind == ParamDef::Bool) {
        auto* c = new QCheckBox(parent);
        c->setChecked(store.params().get(key) != 0);
        QObject::connect(c, &QCheckBox::toggled, c, [apply](bool on) { apply(on ? "true" : "false"); });
        return c;
    }
    auto* c = new QComboBox(parent);
    for (const std::string& o : d.options) c->addItem(fromStd(o));
    c->setCurrentIndex((int)store.params().get(key));
    QObject::connect(c, QOverload<int>::of(&QComboBox::activated), c, [apply, c](int) { apply(toStd(c->currentText())); });
    return c;
}
void syncEditor(QWidget* e, SimParamsStore& store, const std::string& key) {
    const int64_t v = store.params().get(key);
    if (auto* s = qobject_cast<QSpinBox*>(e)) { if (!s->hasFocus() && s->value() != v) { QSignalBlocker b(s); s->setValue((int)v); } }
    else if (auto* c = qobject_cast<QCheckBox*>(e)) { QSignalBlocker b(c); c->setChecked(v != 0); }
    else if (auto* cb = qobject_cast<QComboBox*>(e)) { QSignalBlocker b(cb); cb->setCurrentIndex((int)v); }
}

// ---- the parameter panel --------------------------------------------------------------------------------------------------------------

class ParamDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& opt, const QModelIndex& index) const override {
        const asmcore::ParamDef* d = asmcore::findParam(toStd(index.siblingAtColumn(0).data(Qt::UserRole).toString()));          // the key is kept on the name column
        if (!d) return nullptr;
        if (index.column() == 1) {
            if (d->kind == ParamDef::Int) {
                auto* s = new QSpinBox(parent);
                s->setRange((int)std::max<int64_t>(d->lo, INT32_MIN), (int)std::min<int64_t>(d->hi, INT32_MAX));
                s->setObjectName("paramEditor");
                return s;
            }
            auto* c = new QComboBox(parent);
            c->setObjectName("paramEditor");
            if (d->kind == ParamDef::Bool) c->addItems({"false", "true"});
            else for (const std::string& o : d->options) c->addItem(fromStd(o));
            return c;
        }
        if (index.column() == 4) {
            auto* c = new QComboBox(parent);
            c->addItems({"planning", "measured", "confirmed"});
            return c;
        }
        if (index.column() == 5) return QStyledItemDelegate::createEditor(parent, opt, index);
        return nullptr;
    }
    void setEditorData(QWidget* e, const QModelIndex& index) const override {
        if (auto* s = qobject_cast<QSpinBox*>(e)) { s->setValue(index.data(Qt::EditRole).toString().toInt()); return; }
        if (auto* c = qobject_cast<QComboBox*>(e)) { c->setCurrentText(index.data(Qt::EditRole).toString()); return; }
        QStyledItemDelegate::setEditorData(e, index);
    }
    void setModelData(QWidget* e, QAbstractItemModel* m, const QModelIndex& index) const override {
        if (auto* s = qobject_cast<QSpinBox*>(e)) { m->setData(index, QString::number(s->value())); return; }
        if (auto* c = qobject_cast<QComboBox*>(e)) { m->setData(index, c->currentText()); return; }
        QStyledItemDelegate::setModelData(e, m, index);
    }
};

class ParamsPanel : public QWidget {
    Q_DECLARE_TR_FUNCTIONS(ParamsPanel)
public:
    void focusSearch() { filter_->setFocus(); filter_->selectAll(); }          // Edit > Find (Ctrl+F) while the page is shown
    // Brings a parameter into view: the search is cleared, the row selected and its value edited.
    void showParameter(const std::string& key) {
        filter_->clear();
        changedOnly_->setChecked(false);
        applyFilter();
        QTreeWidgetItem* row = nullptr;
        eachRow([&](QTreeWidgetItem* it) { if (toStd(it->data(0, Qt::UserRole).toString()) == key) row = it; });
        if (!row) return;
        tree_->setCurrentItem(row);
        tree_->scrollToItem(row);
        tree_->setFocus();
        tree_->editItem(row, 1);
    }
    explicit ParamsPanel(AppContext& ctx) : ctx_(ctx), store_(ctx.debugger.simParams()) {
        setObjectName("paramsPanel");
        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(2, 2, 2, 2);
        auto* top = new QHBoxLayout;
        auto button = [&](const char* name, const QString& text) { auto* b = new QPushButton(text); b->setObjectName(name); top->addWidget(b); return b; };
        auto* load = button("paramsLoad", tr("Load..."));
        auto* save = button("paramsSave", tr("Save"));
        auto* saveAs = button("paramsSaveAs", tr("Save As..."));
        auto* reset = button("paramsResetAll", tr("Reset all"));
        apply_ = button("paramsApply", tr("Restart with these parameters"));
        lay->addLayout(top);
        file_ = new QLabel;
        file_->setObjectName("paramsFile");
        lay->addWidget(file_);
        auto* f = new QHBoxLayout;
        filter_ = new QLineEdit;
        filter_->setObjectName("paramsFilter");
        filter_->setPlaceholderText(tr("search parameters (Ctrl+F)  \xE2\x80\x94  Enter edits the first match"));
        filter_->setClearButtonEnabled(true);
        changedOnly_ = new QCheckBox(tr("changed only"));
        changedOnly_->setObjectName("paramsChangedOnly");
        f->addWidget(filter_, 1);
        f->addWidget(changedOnly_);
        lay->addLayout(f);
        tree_ = new QTreeWidget;
        tree_->setObjectName("paramsTree");
        tree_->setColumnCount(6);
        tree_->setHeaderLabels({tr("Parameter"), tr("Value"), tr("Unit"), tr("Initial"), tr("Status"), tr("Note")});
        tree_->setItemDelegate(new ParamDelegate(tree_));
        tree_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed | QAbstractItemView::SelectedClicked);
        tree_->setContextMenuPolicy(Qt::CustomContextMenu);
        tree_->setUniformRowHeights(true);
        tree_->header()->setStretchLastSection(true);
        lay->addWidget(tree_, 1);
        help_ = new QLabel;
        help_->setObjectName("paramsHelp");
        help_->setWordWrap(true);
        help_->setMinimumHeight(54);
        help_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        lay->addWidget(help_);
        build();

        connect(filter_, &QLineEdit::textChanged, this, [this] { applyFilter(); });
        connect(filter_, &QLineEdit::returnPressed, this, [this] { editFirstMatch(); });
        conns_.add(ctx.settings.changed, [this](const std::string& key) { if (key.compare(0, 16, "simparams.pinned") == 0) refresh(); });
        connect(changedOnly_, &QCheckBox::toggled, this, [this] { applyFilter(); });
        connect(tree_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* it, int col) { edited(it, col); });
        connect(tree_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* it) { showHelp(it); });
        connect(tree_, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& at) { menu(at); });
        connect(load, &QPushButton::clicked, this, [this] { loadFile(); });
        connect(save, &QPushButton::clicked, this, [this] { saveFile(false); });
        connect(saveAs, &QPushButton::clicked, this, [this] { saveFile(true); });
        connect(reset, &QPushButton::clicked, this, [this] { store_.clearAll(); });
        connect(apply_, &QPushButton::clicked, this, [this] {
            if (!ctx_.debugger.active()) return;
            std::string why;
            ctx_.debugger.stop();
            if (!ctx_.debugger.start(why)) ctx_.status.fire(fromStd(why));
            else ctx_.status.fire(tr("The simulator was restarted from cycle 0 with these parameters"));
        });
        conns_.add(store_.changed, [this] { refresh(); });
        conns_.add(ctx.debugger.updated, [this](const SimUpdate&) { refreshButtons(); });
        refresh();
    }

private:
    void build() {
        updating_ = true;
        tree_->clear();
        QTreeWidgetItem* group = nullptr;
        std::string current;
        for (const ParamDef& d : paramCatalog()) {
            if (d.group != current || !group) {
                current = d.group;
                group = new QTreeWidgetItem(tree_, {groupName(d.group)});
                group->setFlags(Qt::ItemIsEnabled);
                QFont gf = group->font(0);
                gf.setBold(true);
                group->setFont(0, gf);
                group->setExpanded(true);
            }
            auto* it = new QTreeWidgetItem(group, {fromStd(d.key), QString(), fromStd(d.unit), QString::fromStdString(SimParams().text(d.key)), QString(), QString()});
            it->setData(0, Qt::UserRole, fromStd(d.key));
            it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable);
            it->setToolTip(0, fromStd(d.help + (d.measure.empty() ? std::string() : "\n" + std::string("measure: ") + d.measure)));
        }
        for (int c = 0; c < 5; c++) tree_->resizeColumnToContents(c);
        tree_->setColumnWidth(0, std::max(tree_->columnWidth(0), 190));
        updating_ = false;
    }
    template <class F>
    void eachRow(F f) {
        for (int g = 0; g < tree_->topLevelItemCount(); g++) {
            QTreeWidgetItem* gi = tree_->topLevelItem(g);
            for (int i = 0; i < gi->childCount(); i++) f(gi->child(i));
        }
    }
    void refresh() {
        updating_ = true;
        const SimParams& p = store_.params();
        eachRow([&](QTreeWidgetItem* it) {
            const std::string key = toStd(it->data(0, Qt::UserRole).toString());
            const ParamDef* d = asmcore::findParam(key);
            const bool set = p.isSet(key) && !p.atDefault(key);
            it->setText(1, fromStd(p.text(key)));
            it->setText(4, fromStd(p.status(key)));
            it->setText(5, fromStd(p.note(key)));
            QFont f = it->font(1);
            f.setBold(set);
            it->setFont(1, f);
            QFont nf = it->font(0);
            nf.setItalic(d && !d->active);
            it->setFont(0, nf);
            const QPalette pal = tree_->palette();
            QColor text = pal.color(QPalette::Text);
            if (d && !d->active) text = pal.color(QPalette::Disabled, QPalette::Text);
            it->setForeground(0, text);
            it->setForeground(1, set ? QColor("#e08a00") : text);
            const std::string st = p.status(key);
            it->setForeground(4, st == "measured" ? QColor("#2e9d4f") : st == "confirmed" ? QColor("#3b82f6") : text);
            it->setText(0, QString(isPinned(ctx_.settings, key) ? QString::fromUtf8("\xE2\x98\x85 ") : QString()) + fromStd(key) + (d && !d->active ? tr("  (not used yet)") : QString()));
        });
        updating_ = false;
        applyFilter();
        refreshButtons();
        const QString name = store_.path().empty() ? tr("(no file: the values are not saved)") : QFileInfo(fromStd(store_.path())).fileName();
        file_->setText(name + (store_.dirty() ? QString::fromUtf8("  \xE2\x97\x8F ") + tr("changed") : QString()));
    }
    void refreshButtons() { apply_->setEnabled(ctx_.debugger.active()); }
    void applyFilter() {
        const QString f = filter_->text().trimmed();
        const bool onlyChanged = changedOnly_->isChecked();
        const SimParams& p = store_.params();
        for (int g = 0; g < tree_->topLevelItemCount(); g++) {
            QTreeWidgetItem* gi = tree_->topLevelItem(g);
            int shown = 0;
            for (int i = 0; i < gi->childCount(); i++) {
                QTreeWidgetItem* it = gi->child(i);
                const std::string key = toStd(it->data(0, Qt::UserRole).toString());
                const ParamDef* pd = asmcore::findParam(key);
                bool show = f.isEmpty() || (pd && paramScore(*pd, f) > 0);
                if (onlyChanged && !(p.isSet(key) && (!p.atDefault(key) || p.status(key) != "planning" || !p.note(key).empty()))) show = false;
                it->setHidden(!show);
                if (show) shown++;
            }
            gi->setHidden(shown == 0);
        }
    }
    void showHelp(QTreeWidgetItem* it) {
        if (!it || !it->parent()) { help_->clear(); return; }
        const ParamDef* d = asmcore::findParam(toStd(it->data(0, Qt::UserRole).toString()));
        if (!d) return;
        QString range;
        if (d->kind == ParamDef::Enum) { for (const std::string& o : d->options) range += (range.isEmpty() ? "" : " | ") + fromStd(o); }
        else if (d->kind == ParamDef::Bool) range = "true | false";
        else range = QString("%1 .. %2").arg(d->lo).arg(d->hi);
        QString t = "<b>" + fromStd(d->key).toHtmlEscaped() + "</b>  (" + range.toHtmlEscaped() + (d->unit.empty() ? QString() : ", " + fromStd(d->unit).toHtmlEscaped()) + ")<br>" + fromStd(d->help).toHtmlEscaped();
        if (!d->measure.empty()) t += "<br><i>" + tr("on the FPGA:") + "</i> " + fromStd(d->measure).toHtmlEscaped();
        if (!d->active) t += "<br><i>" + tr("The simulator does not use this one yet: it is kept in the file for when it does.") + "</i>";
        help_->setText(t);
    }
    void edited(QTreeWidgetItem* it, int col) {
        if (updating_ || !it->parent()) return;
        const std::string key = toStd(it->data(0, Qt::UserRole).toString());
        std::string err;
        bool ok = true;
        if (col == 1) ok = store_.set(key, toStd(it->text(1)), err);
        else if (col == 4) ok = store_.setStatus(key, toStd(it->text(4)), err);
        else if (col == 5) store_.setNote(key, toStd(it->text(5)));
        if (!ok) { ctx_.status.fire(fromStd(err)); refresh(); }
    }
    void menu(const QPoint& at) {
        QTreeWidgetItem* it = tree_->itemAt(at);
        if (!it || !it->parent()) return;
        const std::string key = toStd(it->data(0, Qt::UserRole).toString());
        QMenu m(this);
        const bool pinned = isPinned(ctx_.settings, key);
        QAction* pin = m.addAction(pinned ? tr("Unpin from the quick parameters") : tr("Pin to the quick parameters"));
        connect(pin, &QAction::triggered, this, [this, key, pinned] { setPinned(ctx_.settings, key, !pinned); });
        QAction* reset = m.addAction(tr("Back to the initial value"));
        reset->setEnabled(store_.params().isSet(key));
        connect(reset, &QAction::triggered, this, [this, key] { store_.clear(key); });
        m.exec(tree_->viewport()->mapToGlobal(at));
    }
    // the first row the search leaves visible: selected, in view, and its value is being edited (type the number, Enter)
    void editFirstMatch() {
        QTreeWidgetItem* first = nullptr;
        int best = 0;
        const QString f = filter_->text().trimmed();
        eachRow([&](QTreeWidgetItem* it) {                                                  // the best fit (the name starts with it), else the first one left
            if (it->isHidden()) return;
            const ParamDef* pd = asmcore::findParam(toStd(it->data(0, Qt::UserRole).toString()));
            const int s = pd ? paramScore(*pd, f) : 0;
            if (!first || s > best) { first = it; best = s; }
        });
        if (!first) return;
        tree_->setCurrentItem(first);
        tree_->scrollToItem(first);
        tree_->setFocus();
        tree_->editItem(first, 1);
    }
    void loadFile() {
        const QString f = QFileDialog::getOpenFileName(this, tr("Load simulator parameters"), fromStd(ctx_.workspace.hasProject() ? ctx_.workspace.project().baseDir() : std::string()), tr("Simulator parameters (*.simparams);;All files (*)"));
        if (f.isEmpty()) return;
        std::vector<std::string> errs;
        store_.load(toStd(f), errs);
        for (const std::string& e : errs) ctx_.log.fire(fromStd(e));
    }
    void saveFile(bool as) {
        std::string err;
        if (as || store_.path().empty()) {
            QString f = QFileDialog::getSaveFileName(this, tr("Save simulator parameters"), fromStd(ctx_.workspace.hasProject() ? ctx_.workspace.project().baseDir() + "/target.simparams" : std::string("target.simparams")), tr("Simulator parameters (*.simparams)"));
            if (f.isEmpty()) return;
            if (!f.endsWith(".simparams")) f += ".simparams";
            if (!store_.saveAs(toStd(f), err)) ctx_.status.fire(fromStd(err));
        } else if (!store_.save(err)) ctx_.status.fire(fromStd(err));
    }

    AppContext& ctx_;
    SimParamsStore& store_;
    QTreeWidget* tree_;
    QLineEdit* filter_;
    QCheckBox* changedOnly_;
    QLabel *file_, *help_;
    QPushButton* apply_;
    bool updating_ = false;
    ConnectionBag conns_;
};

// ---- the execution view -----------------------------------------------------------------------------------------------------------------

QColor kindColor(const std::string& what) {
    auto starts = [&](const char* p) { return what.compare(0, std::strlen(p), p) == 0; };
    if (starts("MOV")) return QColor("#3a78b8");
    if (starts("ALU")) return QColor("#4e9641");
    if (starts("EXEC_CSR")) return QColor("#b87c2c");
    if (starts("EXEC_XFER")) return QColor("#2c9588");
    if (starts("EXEC_PUB")) return QColor("#25a9c4");
    if (starts("BR") || starts("JMP") || starts("CASCADE") || starts("CALL") || starts("RET")) return QColor("#a8558a");
    return QColor("#6c717d");
}

class ExecutionView : public QWidget {
public:
    ExecutionView() {
        setObjectName("executionView");
        setMouseTracking(true);
        setMinimumHeight(110);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }
    void setData(const RunTrace* cur, const RunTrace* base, const TraceDiff* diff) {
        live_ = false; gaps_ = nullptr; follow_ = false;
        cur_ = cur; base_ = base; diff_ = diff;
        for (int d = 0; d < 2; d++) { idx_[d].clear(); bidx_[d].clear(); }
        if (cur_) for (size_t i = 0; i < cur_->rows.size(); i++) idx_[cur_->rows[i].domain].push_back(i);
        if (base_) for (size_t i = 0; i < base_->rows.size(); i++) bidx_[base_->rows[i].domain].push_back(i);
        maxDrift_ = 1;
        if (diff_) for (const RowDelta& r : diff_->moved) maxDrift_ = std::max<long long>(maxDrift_, std::llabs(r.delta));
        links_ = cur_ ? traceLinks(*cur_) : std::vector<TraceLink>();
        fit();
    }
    double t0() const { return t0_; }
    double t1() const { return t1_; }
    double end() const { return cur_ ? std::max<double>((double)cur_->endAt, base_ ? (double)base_->endAt : 0) : 1; }
    void fit() {
        t0_ = live_ && cur_ && !cur_->rows.empty() ? (double)cur_->rows.front().at : 0;
        t1_ = std::max(t0_ + 8.0, end() * 1.02);
        if (live_) follow_ = false;
        publish();
        update();
    }
    // ---- live: the history of the running (or stopped) debug session. The index is kept up to date by adding the rows that came, and made again when the old end was cut.
    void startLive() { live_ = true; liveFresh_ = true; epoch_ = (uint64_t)-1; follow_ = true; idxRows_ = 0; publish(); }
    void endLive() { live_ = false; gaps_ = nullptr; follow_ = false; cur_ = nullptr; for (int d = 0; d < 2; d++) idx_[d].clear(); publish(); update(); }
    void setLive(const RunTrace* cur, const std::vector<TraceRing::Gap>* gaps, uint64_t epoch) {
        live_ = true;
        cur_ = cur; base_ = nullptr; diff_ = nullptr; gaps_ = gaps;
        if (epoch != epoch_ || idxRows_ > cur_->rows.size()) {
            for (int d = 0; d < 2; d++) idx_[d].clear();
            idxRows_ = 0;
            epoch_ = epoch;
        }
        for (; idxRows_ < cur_->rows.size(); idxRows_++) idx_[cur_->rows[idxRows_].domain].push_back(idxRows_);
        links_ = traceLinks(*cur_);
        maxDrift_ = 1;
        if (liveFresh_ || t1_ <= t0_) {
            liveFresh_ = false;
            t1_ = std::max(8.0, end());
            t0_ = t1_ - 5000;                                                               // a window of 5000 cycles to begin with (the history may be shorter: the left is empty then)
        }
        if (follow_) followNow();
        publish();
        update();
    }
    bool live() const { return live_; }
    bool follow() const { return follow_; }
    void setFollow(bool f) { follow_ = f; if (f && live_) followNow(); publish(); update(); }
    enum Show { ShowEvents = 1, ShowBus = 2, ShowResults = 4, ShowTimers = 8, ShowBlanking = 16 };
    void setShown(unsigned mask) { shown_ = mask; update(); }
    unsigned shown() const { return shown_; }
    const std::vector<TraceLink>& links() const { return links_; }
    Signal<int> markerClicked;                             // the index of a marker (RunTrace::marks) that was clicked
    QString describeMarker(int i) const;
    QString describeLink(const TraceLink& l) const;
    Signal<> userMoved;                                    // the user zoomed or panned the live view: it no longer follows
    Signal<double> rewindRequested;                        // a double click on the live time line: the cycle
    double plotLeft() const { return kLeft; }
    double plotRight() const { return width() - kRight; }
    void zoomTo(double a, double b) {
        if (b - a < 8) { const double mid = (a + b) / 2; a = mid - 4; b = mid + 4; }
        t0_ = a; t1_ = b;
        update();
    }
    void focusRow(int domain, size_t k) {                 // the k-th instruction of a domain: bring it to the middle at a readable scale
        if (!cur_ || k >= idx_[domain].size()) return;
        const TraceRow& r = cur_->rows[idx_[domain][k]];
        const double span = std::max(60.0, (double)(cur_->endOf(idx_[domain][k]) - r.at) * 6);
        zoomTo((double)r.at - span / 3, (double)r.at + span * 2 / 3);
        sel_[0] = domain; sel_[1] = (int)k;
    }
    // the layout, public so the tests can aim at it
    QRectF laneRect(int d) const { return QRectF(kLeft, kAxis + d * (kLane + kGhost + kGap), width() - kLeft - kRight, kLane); }
    QRectF ghostRect(int d) const { return laneRect(d).adjusted(0, kLane + 2, 0, kGhost - 2 + 0); }
    QRectF driftRect() const { return QRectF(kLeft, kAxis + 2 * (kLane + kGhost + kGap) + 8, width() - kLeft - kRight, kDrift); }
    double xOf(double t) const { return kLeft + (t - t0_) / (t1_ - t0_) * (width() - kLeft - kRight); }
    double tOf(double x) const { return t0_ + (x - kLeft) / (width() - kLeft - kRight) * (t1_ - t0_); }
    QRectF rowRect(int d, size_t k) const {
        const size_t i = idx_[d][k];
        const double x0 = xOf((double)cur_->rows[i].at), x1 = xOf((double)cur_->endOf(i));
        const QRectF l = laneRect(d);
        return QRectF(x0, l.top(), std::max(1.0, x1 - x0 - 1), l.height());
    }
    size_t rowsOf(int d) const { return idx_[d].size(); }
    // The row (index into the trace) of the k-th instruction of a domain.
    size_t rowIndex(int d, size_t k) const { return idx_[d][k]; }
    int hoverDomain() const { return hov_[0]; }
    int hoverIndex() const { return hov_[1]; }
    Signal<int, size_t> clicked;                          // domain, the n-th instruction of it

    QString describe(int d, size_t k) const {
        const size_t i = idx_[d][k];
        const TraceRow& r = cur_->rows[i];
        QString s = QString("%1  #%2  pc %3  %4\ncycle %5 \xE2\x86\x92 %6 (%7 cycles)").arg(d == 0 ? "Main" : "Peri").arg(k + 1).arg(r.pc).arg(fromStd(r.what)).arg(r.at).arg(cur_->endOf(i)).arg(cur_->endOf(i) - r.at);
        if (diff_ && base_ && k < diff_->matched[d] && k < bidx_[d].size()) {
            const TraceRow& b = base_->rows[bidx_[d][k]];
            const long long delta = (long long)r.at - (long long)b.at;
            s += QString("\nbaseline: cycle %1 \xE2\x86\x92 %2 (%3 cycles)\nshift: %4%5 cycles").arg(b.at).arg(base_->endOf(bidx_[d][k])).arg(base_->endOf(bidx_[d][k]) - b.at).arg(delta > 0 ? "+" : "").arg(delta);
        }
        return s;
    }

    static QString cyc(Cycle v) { return QString::number((qulonglong)v); }
    static const char* lane(int d) { return d == 1 ? "Peri" : "Main"; }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, false);
        const QPalette pal = palette();
        p.fillRect(rect(), pal.color(QPalette::Base));
        const QColor text = pal.color(QPalette::Text), faint = pal.color(QPalette::Mid);
        p.setPen(text);
        if (!cur_ || cur_->rows.empty()) {
            p.drawText(rect().adjusted(12, 12, -12, -12), Qt::AlignTop | Qt::AlignLeft | Qt::TextWordWrap, message_.isEmpty() ? tr("Nothing to show yet: build the program, then Run.") : message_);
            return;
        }
        // the cycle axis
        const double span = t1_ - t0_;
        const double target = span / std::max(1.0, (width() - kLeft - kRight) / 90.0);
        const double mag = std::pow(10.0, std::floor(std::log10(std::max(target, 1.0))));
        double step = mag;
        for (double m : {1.0, 2.0, 5.0, 10.0}) if (mag * m >= target) { step = mag * m; break; }
        p.setPen(faint);
        for (double t = std::ceil(t0_ / step) * step; t <= t1_; t += step) {
            const double x = xOf(t);
            p.drawLine(QPointF(x, kAxis - 6), QPointF(x, driftRect().bottom()));
            p.setPen(text);
            p.drawText(QPointF(x + 3, kAxis - 8), QString::number((long long)t));
            p.setPen(faint);
        }
        const QFontMetrics fm(font());
        for (int d = 0; d < 2; d++) {
            const QRectF lane = laneRect(d), ghost = ghostRect(d);
            p.setPen(text);
            p.drawText(QRectF(4, lane.top(), kLeft - 8, lane.height()), Qt::AlignVCenter | Qt::AlignLeft, d == 0 ? "Main" : "Peri");
            p.drawText(QRectF(4, ghost.top(), kLeft - 8, ghost.height()), Qt::AlignVCenter | Qt::AlignLeft, base_ ? tr("baseline") : QString());
            p.fillRect(lane.adjusted(0, 0, 0, 0), pal.color(QPalette::AlternateBase));
            if (idx_[d].empty()) { p.setPen(faint); p.drawText(lane.adjusted(8, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, tr("no program")); p.setPen(text); }
            p.save();
            p.setClipRect(QRectF(kLeft, lane.top() - 14, width() - kLeft - kRight, lane.height() + kGhost + 18));
            if (base_) {                                                                     // the ghost lane: the baseline, outlined
                for (size_t k = 0; k < bidx_[d].size(); k++) {
                    const size_t i = bidx_[d][k];
                    const double x0 = xOf((double)base_->rows[i].at), x1 = xOf((double)base_->endOf(i));
                    if (x1 < kLeft || x0 > width() - kRight) continue;
                    QColor c = kindColor(base_->rows[i].what);
                    p.setPen(c);
                    p.setBrush(Qt::NoBrush);
                    p.drawRect(QRectF(x0, ghost.top() + 2, std::max(1.0, x1 - x0 - 1), ghost.height() - 4));
                }
            }
            if (live_ && gaps_) {                                                            // what the session could not deliver
                for (const TraceRing::Gap& g : *gaps_) {
                    const double x0 = xOf((double)g.from), x1 = xOf((double)g.to);
                    if (x1 < kLeft || x0 > width() - kRight) continue;
                    p.setPen(Qt::NoPen);
                    p.setBrush(QBrush(QColor(160, 160, 160, 150), Qt::BDiagPattern));
                    p.drawRect(QRectF(std::max(x0, (double)kLeft), lane.top(), std::max(2.0, std::min(x1, width() - kRight) - std::max(x0, (double)kLeft)), lane.height()));
                    if (x1 - x0 > 60) { p.setPen(faint); p.drawText(QRectF(x0 + 2, lane.top(), x1 - x0 - 4, lane.height()), Qt::AlignCenter, tr("events missing")); }
                }
            }
            for (size_t li = 0; li < links_.size(); li++) {                                  // blanking windows behind the instructions; what an event / a timer spans on the lane
                const TraceLink& l = links_[li];
                if (!linkShown(l)) continue;
                const double x0 = xOf((double)l.from), x1 = xOf((double)l.to);
                if (x1 < kLeft || x0 > width() - kRight) continue;
                const bool hot = (int)li == hovLink_ || (hovMark_ >= 0 && (l.fromMark == hovMark_ || l.toMark == hovMark_));
                if (l.kind == TraceLink::Blanking) {
                    p.fillRect(QRectF(x0, lane.top(), std::max(1.0, x1 - x0), lane.height()), QColor(80, 140, 220, hot ? 70 : 38));
                } else if (l.kind != TraceLink::Result && (l.domain == 1 ? 1 : 0) == d) {
                    const QRectF bar = linkBar(l);
                    QColor c = l.kind == TraceLink::Timer ? QColor("#f59e0b") : QColor("#d946ef");
                    if (l.kind == TraceLink::EventWait) { p.setPen(QPen(c, 1)); p.setBrush(QBrush(c, Qt::BDiagPattern)); }
                    else if (l.lost) { p.setPen(QPen(c, 1, Qt::DashLine)); p.setBrush(Qt::NoBrush); }
                    else { p.setPen(Qt::NoPen); p.setBrush(c); }
                    p.drawRect(bar.adjusted(0, 0, 0, hot ? 1 : 0));
                    if (hot) { p.setPen(QPen(text, 1)); p.setBrush(Qt::NoBrush); p.drawRect(bar.adjusted(-0.5, -0.5, 0.5, 1.5)); }
                    if (l.open) { p.setPen(QPen(c, 1, Qt::DotLine)); p.drawLine(QPointF(bar.right(), bar.center().y()), QPointF(std::min((double)width() - kRight, bar.right() + 18), bar.center().y())); }
                }
            }
            size_t kFirst = 0;                                                               // (only what is in view: a long history is not walked every time)
            {
                size_t lo = 0, hi = idx_[d].size();
                while (lo < hi) { const size_t mid = (lo + hi) / 2; if ((double)cur_->rows[idx_[d][mid]].at < t0_) lo = mid + 1; else hi = mid; }
                kFirst = lo > 0 ? lo - 1 : 0;
            }
            for (size_t k = kFirst; k < idx_[d].size(); k++) {                               // the current run
                const size_t i = idx_[d][k];
                const QRectF r = rowRect(d, k);
                if (r.left() > width() - kRight) break;
                if (r.right() < kLeft) continue;
                QColor c = kindColor(cur_->rows[i].what);
                p.setPen(Qt::NoPen);
                p.setBrush(c);
                p.drawRect(r);
                const bool sel = (sel_[0] == d && sel_[1] == (int)k), hov = (hov_[0] == d && hov_[1] == (int)k);
                if (sel || hov) { p.setPen(QPen(text, sel ? 2 : 1)); p.setBrush(Qt::NoBrush); p.drawRect(r.adjusted(0.5, 0.5, -0.5, -0.5)); }
                if (diff_ && k < diff_->matched[d] && base_ && k < bidx_[d].size()) {       // late: a red tick on the top edge (the dimmer the earlier)
                    const long long delta = (long long)cur_->rows[i].at - (long long)base_->rows[bidx_[d][k]].at;
                    if (delta != 0) { p.setPen(Qt::NoPen); p.setBrush(delta > 0 ? QColor("#e5484d") : QColor("#3b82f6")); p.drawRect(QRectF(r.left(), r.top() - 3, std::max(2.0, std::min(r.width(), 6.0)), 3)); }
                }
                if (r.width() > 30) {
                    p.setPen(Qt::white);
                    p.drawText(r.adjusted(3, 0, -2, 0), Qt::AlignVCenter | Qt::AlignLeft, fm.elidedText(fromStd(cur_->rows[i].what), Qt::ElideRight, (int)r.width() - 5));
                }
            }
            const auto firstMark = std::lower_bound(cur_->marks.begin(), cur_->marks.end(), t0_, [](const TraceMarker& m, double t) { return (double)m.at < t; });
            for (auto mi = firstMark; mi != cur_->marks.end(); ++mi) {                       // what happens between the sequencers
                const TraceMarker& m = *mi;
                if ((double)m.at > t1_) break;
                if (!kindShown(m.kind)) continue;
                const int md = m.domain == 1 ? 1 : 0;
                if (md != d && m.kind != 'e') continue;
                if (m.domain >= 0 && m.domain != d) continue;
                const double x = xOf((double)m.at);
                if (x < kLeft || x > width() - kRight) continue;
                const double y = lane.top() - 6;
                const bool hot = (int)(mi - cur_->marks.begin()) == hovMark_;
                p.setPen(Qt::NoPen);
                const double z = hot ? 1.5 : 1.0;
                if (m.kind == 'e') { p.setBrush(QColor("#d946ef")); p.drawPolygon(QPolygonF({QPointF(x - 4 * z, y - 4 * z), QPointF(x + 4 * z, y - 4 * z), QPointF(x, y + 3 * z)})); }
                else if (m.kind == 'b') { p.setBrush(QColor("#14b8a6")); p.drawRect(QRectF(x - 3 * z, y - 3 * z, 6 * z, 6 * z)); }
                else if (m.kind == 'p') { p.setBrush(QColor("#22d3ee")); p.drawPolygon(QPolygonF({QPointF(x, y - 4 * z), QPointF(x + 4 * z, y), QPointF(x, y + 4 * z), QPointF(x - 4 * z, y)})); }
                else if (m.kind == 'c') { p.setBrush(QColor("#84cc16")); p.drawEllipse(QPointF(x, y), 3.5 * z, 3.5 * z); }
                else if (m.kind == 's') { p.setBrush(QColor("#f59e0b")); p.drawPolygon(QPolygonF({QPointF(x - 2, y - 4 * z), QPointF(x + 5 * z, y), QPointF(x - 2, y + 4 * z)})); }
                else if (m.kind == 'd') { p.setBrush(QColor("#f59e0b")); p.drawRect(QRectF(x - 1.5 * z, y - 4 * z, 3 * z, 8 * z)); }
                else { p.setBrush(QColor("#ef4444")); p.drawRect(QRectF(x - 4 * z, y - 4 * z, 8 * z, 8 * z)); }
                if (hot) { p.setPen(QPen(text, 1)); p.setBrush(Qt::NoBrush); p.drawEllipse(QPointF(x, y), 7, 7); p.setPen(Qt::NoPen); }
            }
            p.restore();
        }
        if (shown_ & ShowResults) {                                                          // a result: from the publish (Peri) to the take (Main)
            p.save();
            p.setClipRect(QRectF(kLeft, kAxis - 16, width() - kLeft - kRight, driftRect().top() - kAxis + 16));
            for (size_t li = 0; li < links_.size(); li++) {
                const TraceLink& l = links_[li];
                if (l.kind != TraceLink::Result) continue;
                const double x0 = xOf((double)l.from), x1 = xOf((double)l.to);
                if (std::max(x0, x1) < kLeft || std::min(x0, x1) > width() - kRight) continue;
                const bool hot = (int)li == hovLink_ || (hovMark_ >= 0 && (l.fromMark == hovMark_ || l.toMark == hovMark_));
                const double yP = laneRect(1).top() - 6;
                if (l.lost) {                                                                // overwritten: a cross at the publish
                    p.setPen(QPen(QColor("#ef4444"), hot ? 3 : 2));
                    p.drawLine(QPointF(x0 - 5, yP - 5), QPointF(x0 + 5, yP + 5));
                    p.drawLine(QPointF(x0 - 5, yP + 5), QPointF(x0 + 5, yP - 5));
                    continue;
                }
                const QPolygonF poly = resultPath(l);
                QColor c("#84cc16");
                p.setPen(QPen(c, hot ? 3.5 : 1.8, l.open ? Qt::DashLine : Qt::SolidLine));
                p.setBrush(Qt::NoBrush);
                p.drawPolyline(poly);
                if (!l.open) {
                    const QPointF tip = poly.last();
                    p.setPen(Qt::NoPen);
                    p.setBrush(c);
                    p.drawPolygon(QPolygonF({tip, QPointF(tip.x() - 4, tip.y() + 8), QPointF(tip.x() + 4, tip.y() + 8)}));
                    if (x1 - x0 > 44) {                                                      // how long the result waited in the slot
                        p.setPen(text);
                        p.drawText(QPointF(x0 + 4, (poly[1].y()) - 2), tr("%1 cycles").arg((qulonglong)l.length()));
                    }
                }
            }
            p.restore();
        }
        if (live_) {                                                                         // where the simulator is now
            const double x = xOf((double)cur_->endAt);
            if (x >= kLeft && x <= width() - kRight) {
                p.setPen(QPen(QColor("#e5484d"), 2, Qt::DashLine));
                p.drawLine(QPointF(x, kAxis - 6), QPointF(x, driftRect().bottom()));
                p.setPen(QColor("#e5484d"));
                p.drawText(QPointF(std::max(x - 28, (double)kLeft), kAxis - 12), tr("now"));
            }
        }
        // how far each lane has drifted from the baseline
        const QRectF dr = driftRect();
        p.setPen(faint);
        p.setBrush(Qt::NoBrush);
        p.drawRect(dr);
        p.drawLine(QPointF(dr.left(), dr.center().y()), QPointF(dr.right(), dr.center().y()));
        p.setPen(text);
        p.drawText(QRectF(4, dr.top(), kLeft - 8, dr.height()), Qt::AlignVCenter | Qt::AlignLeft | Qt::TextWordWrap, tr("shift (cycles)"));
        p.drawText(QPointF(dr.left() + 4, dr.top() + 11), QString("+%1").arg(maxDrift_));
        p.drawText(QPointF(dr.left() + 4, dr.bottom() - 3), QString("-%1").arg(maxDrift_));
        if (diff_ && base_) {
            p.save();
            p.setClipRect(dr);
            for (int d = 0; d < 2; d++) {
                QPolygonF poly;
                auto yOf = [&](long long v) { return dr.center().y() - (double)v / (double)maxDrift_ * (dr.height() / 2 - 4); };
                long long cur = 0;
                poly << QPointF(xOf(0), yOf(0));
                for (const RowDelta& r : diff_->moved) {
                    if (r.domain != d) continue;
                    poly << QPointF(xOf((double)r.curAt), yOf(cur));
                    cur = r.delta;
                    poly << QPointF(xOf((double)r.curAt), yOf(cur));
                }
                poly << QPointF(xOf(t1_), yOf(cur));
                p.setPen(QPen(d == 0 ? QColor("#3a78b8") : QColor("#2c9588"), 2));
                p.drawPolyline(poly);
            }
            p.restore();
        }
    }
    void wheelEvent(QWheelEvent* e) override {
        if (!cur_) return;
        const double f = std::pow(1.25, -e->angleDelta().y() / 120.0);
        const double mid = tOf(e->position().x());
        double a = mid - (mid - t0_) * f, b = mid + (t1_ - mid) * f;
        if (b - a < 8) { a = mid - 4; b = mid + 4; }
        t0_ = std::max(-end() * 0.02, a);
        t1_ = std::min(end() * 1.5 + 8, b);
        if (live_ && follow_) { follow_ = false; userMoved.fire(); }
        publish();
        update();
        e->accept();
    }
    void mousePressEvent(QMouseEvent* e) override { pressX_ = e->position().x(); panFrom_ = t0_; panTo_ = t1_; moved_ = false; }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (!cur_) return;
        if (e->buttons() & Qt::LeftButton) {
            const double dx = e->position().x() - pressX_;
            if (std::fabs(dx) > 3) moved_ = true;
            const double dt = -dx / (width() - kLeft - kRight) * (panTo_ - panFrom_);
            t0_ = panFrom_ + dt;
            t1_ = panTo_ + dt;
            if (live_ && follow_) { follow_ = false; userMoved.fire(); }
            publish();
            update();
            return;
        }
        int mark = -1, link = -1;
        hitMarkOrLink(e->position(), mark, link);
        if (mark < 0 && link < 0) hitBlanking(e->position(), link);
        if (mark != hovMark_ || link != hovLink_) { hovMark_ = mark; hovLink_ = link; update(); }
        if (mark >= 0 || link >= 0) {
            hov_[0] = hov_[1] = -1;
            QString tip;
            if (mark >= 0) { for (size_t k = 0; k < nearMarks_.size(); k++) tip += (k ? "\n\n" : "") + describeMarker(nearMarks_[k]); }
            else tip = describeLink(links_[(size_t)link]);
            setProperty("hoverText", tip);
            QToolTip::showText(e->globalPosition().toPoint(), tip, this);
            return;
        }
        setProperty("hoverText", QString());
        int d = -1, k = -1;
        hit(e->position(), d, k);
        if (d != hov_[0] || k != hov_[1]) { hov_[0] = d; hov_[1] = k; update(); }
        if (d >= 0) QToolTip::showText(e->globalPosition().toPoint(), describe(d, (size_t)k), this);
        else QToolTip::hideText();
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        if (moved_ || !cur_) return;
        {
            int mark = -1, link = -1;
            hitMarkOrLink(e->position(), mark, link);
            if (mark >= 0) { markerClicked.fire(mark); return; }
        }
        int d = -1, k = -1;
        hit(e->position(), d, k);
        if (d >= 0) { sel_[0] = d; sel_[1] = k; update(); clicked.fire(d, (size_t)k); }
    }
    void mouseDoubleClickEvent(QMouseEvent* e) override {
        if (!live_) { fit(); return; }
        int d = -1, k = -1;
        hit(e->position(), d, k);
        double t = tOf(e->position().x());
        if (d >= 0) t = (double)cur_->rows[idx_[d][(size_t)k]].at;                          // on an instruction: the cycle it starts
        const double first = cur_ && !cur_->rows.empty() ? (double)cur_->rows.front().at : 0;
        rewindRequested.fire(std::max(first, std::min(t, end())));
    }
    void leaveEvent(QEvent*) override { hov_[0] = hov_[1] = -1; hovMark_ = hovLink_ = -1; update(); }
    void resizeEvent(QResizeEvent* e) override { QWidget::resizeEvent(e); publish(); }

private:
    void hit(const QPointF& at, int& d, int& k) const {
        d = k = -1;
        if (!cur_) return;
        for (int dom = 0; dom < 2; dom++) {
            const QRectF l = laneRect(dom);
            if (at.y() < l.top() || at.y() > l.bottom()) continue;
            const double t = tOf(at.x());
            const auto& v = idx_[dom];
            size_t lo = 0, hi = v.size();
            while (lo < hi) { const size_t mid = (lo + hi) / 2; if ((double)cur_->rows[v[mid]].at <= t) lo = mid + 1; else hi = mid; }
            if (lo == 0) return;
            const size_t kk = lo - 1;
            if (t <= (double)cur_->endOf(v[kk]) || rowRect(dom, kk).contains(at)) { d = dom; k = (int)kk; }
            return;
        }
    }
    static constexpr double kLeft = 64, kRight = 10, kAxis = 26, kLane = 30, kGhost = 18, kGap = 14, kDrift = 70;
    const RunTrace* cur_ = nullptr;
    const RunTrace* base_ = nullptr;
    const TraceDiff* diff_ = nullptr;
    std::vector<size_t> idx_[2], bidx_[2];
    const std::vector<TraceRing::Gap>* gaps_ = nullptr;
    std::vector<TraceLink> links_;
    unsigned shown_ = 31;
    int hovMark_ = -1, hovLink_ = -1;
    mutable std::vector<int> nearMarks_;                    // the markers under the pointer, the most important first
    bool kindShown(char k) const {
        switch (k) {
            case 'e': return shown_ & ShowEvents;
            case 'b': return shown_ & ShowBus;
            case 'p': case 'c': case 'o': return shown_ & ShowResults;
            case 's': case 'd': return shown_ & ShowTimers;
            case 'v': case 'w': return false;
            default: return true;                                                          // faults are always shown
        }
    }
    bool linkShown(const TraceLink& l) const {
        switch (l.kind) {
            case TraceLink::Result: return shown_ & ShowResults;
            case TraceLink::EventWait: case TraceLink::EventHandler: return shown_ & ShowEvents;
            case TraceLink::Timer: return shown_ & ShowTimers;
            case TraceLink::Blanking: return shown_ & ShowBlanking;
        }
        return true;
    }
    // where a marker's glyph is, and the polyline of a result's arrow (Peri's lane up to Main's)
    QPointF glyphAt(const TraceMarker& m) const { return QPointF(xOf((double)m.at), laneRect(m.domain == 1 ? 1 : 0).top() - 6); }
    QPolygonF resultPath(const TraceLink& l) const {
        const double x0 = xOf((double)l.from), x1 = xOf((double)l.to);
        const double yP = laneRect(1).top() - 6, yM = laneRect(0).bottom();
        const double ym = (yP + yM) / 2;
        return QPolygonF({QPointF(x0, yP), QPointF(x0, ym), QPointF(x1, ym), QPointF(x1, yM)});
    }
    static double distanceToSegment(const QPointF& p, const QPointF& a, const QPointF& b) {
        const QPointF ab = b - a;
        const double len2 = ab.x() * ab.x() + ab.y() * ab.y();
        double t = len2 > 0 ? ((p.x() - a.x()) * ab.x() + (p.y() - a.y()) * ab.y()) / len2 : 0;
        t = std::max(0.0, std::min(1.0, t));
        const QPointF q = a + ab * t;
        return std::hypot(p.x() - q.x(), p.y() - q.y());
    }
    QRectF linkBar(const TraceLink& l) const {                                             // events above the lane, timers on its lower edge
        const QRectF lane = laneRect(l.domain == 1 ? 1 : 0);
        const double x0 = xOf((double)l.from), x1 = std::max(x0 + 2, xOf((double)l.to));
        if (l.kind == TraceLink::Timer) return QRectF(x0, lane.bottom() - 6, x1 - x0, 6);
        return QRectF(x0, lane.top() - 4, x1 - x0, 4);
    }
    void hitMarkOrLink(const QPointF& at, int& mark, int& link) const {
        mark = link = -1;
        if (!cur_) return;
        const auto first = std::lower_bound(cur_->marks.begin(), cur_->marks.end(), tOf(at.x() - 8), [](const TraceMarker& m, double t) { return (double)m.at < t; });
        // every marker within reach (several can stand on one spot: a bus transfer and the publish that follows in the same cycle); the most important one is "the" marker
        struct Near { int index; double dist; int rank; };
        std::vector<Near> near;
        auto rankOf = [](char k) { switch (k) { case 'f': return 0; case 'p': return 1; case 'o': return 2; case 'e': return 3; case 'c': return 4; case 's': case 'd': return 5; default: return 6; } };
        for (auto it = first; it != cur_->marks.end() && (double)it->at <= tOf(at.x() + 8); ++it) {
            if (!kindShown(it->kind)) continue;
            const QPointF g = glyphAt(*it);
            const double dd = std::hypot(at.x() - g.x(), at.y() - g.y());
            if (dd < 7.5) near.push_back({(int)(it - cur_->marks.begin()), dd, rankOf(it->kind)});
        }
        nearMarks_.clear();
        if (!near.empty()) {
            std::sort(near.begin(), near.end(), [](const Near& a, const Near& b) { return a.rank != b.rank ? a.rank < b.rank : a.dist < b.dist; });
            for (const Near& n : near) nearMarks_.push_back(n.index);
            mark = near.front().index;
            return;
        }
        for (size_t i = 0; i < links_.size(); i++) {
            const TraceLink& l = links_[i];
            if (!linkShown(l) || l.kind == TraceLink::Blanking) continue;
            if (l.kind == TraceLink::Result) {
                if (l.lost && l.fromMark == l.toMark) continue;
                const QPolygonF poly = resultPath(l);
                for (int k = 0; k + 1 < poly.size(); k++) if (distanceToSegment(at, poly[k], poly[k + 1]) < 4) { link = (int)i; return; }
            } else if (linkBar(l).adjusted(0, -2, 0, 2).contains(at)) { link = (int)i; return; }
        }
    }
    void hitBlanking(const QPointF& at, int& link) const {
        link = -1;
        for (size_t i = 0; i < links_.size(); i++) {
            const TraceLink& l = links_[i];
            if (l.kind != TraceLink::Blanking || !linkShown(l)) continue;
            if (at.x() >= xOf((double)l.from) && at.x() <= xOf((double)l.to) && at.y() >= kAxis - 4 && at.y() <= driftRect().top()) { link = (int)i; return; }
        }
    }
    bool live_ = false, follow_ = false, liveFresh_ = false;
    uint64_t epoch_ = 0;
    size_t idxRows_ = 0;
    void followNow() {                                     // keep the width of the window, put its right end at the newest
        const double span = std::max(8.0, t1_ - t0_);
        t1_ = end() + span * 0.04;
        t0_ = t1_ - span;
    }
    void publish() {                                       // for the tests: what the live view is doing
        setProperty("live", live_);
        setProperty("follow", follow_);
        setProperty("t0", t0_);
        setProperty("t1", t1_);
        setProperty("liveEnd", cur_ ? (double)cur_->endAt : 0.0);
        setProperty("gaps", gaps_ ? (int)gaps_->size() : 0);
        setProperty("laneTop0", laneRect(0).top());
        setProperty("laneTop1", laneRect(1).top());
        setProperty("links", (int)links_.size());
        setProperty("plotLeft", kLeft);
        setProperty("plotRight", (double)(width() - kRight));
    }
    double t0_ = 0, t1_ = 100, panFrom_ = 0, panTo_ = 1, pressX_ = 0;
    long long maxDrift_ = 1;
    int hov_[2] = {-1, -1}, sel_[2] = {-1, -1};
    bool moved_ = false;
    QString message_;
public:
    void setMessage(const QString& m) { message_ = m; update(); }
    static QString tr(const char* s) { return QCoreApplication::translate("acide::ExecutionView", s); }
};

QString ExecutionView::describeLink(const TraceLink& l) const {
    switch (l.kind) {
        case TraceLink::Result:
            if (l.lost) return tr("Overrun: %1 published service %2 while the slot was still full (cycle %3): the result was lost").arg(lane(1)).arg(l.a).arg(cyc(l.from));
            if (l.open) return tr("Result of service %1, published by Peri at cycle %2, not taken by Main yet (%3 cycles so far)").arg(l.a).arg(cyc(l.from)).arg(cyc(l.length()));
            return tr("Result of service %1: published by Peri at cycle %2, taken by Main at cycle %3 (it waited %4 cycles)").arg(l.a).arg(cyc(l.from)).arg(cyc(l.to)).arg(cyc(l.length()));
        case TraceLink::EventWait:
            return l.open ? tr("%1 event %2: raised at cycle %3, still waiting for a checkpoint (%4 cycles so far)").arg(lane(l.domain)).arg(l.a).arg(cyc(l.from)).arg(cyc(l.length()))
                          : tr("%1 event %2: raised at cycle %3, dispatched at cycle %4 (it waited %5 cycles for a checkpoint)").arg(lane(l.domain)).arg(l.a).arg(cyc(l.from)).arg(cyc(l.to)).arg(cyc(l.length()));
        case TraceLink::EventHandler:
            return l.open ? tr("%1 handler of event %2: running since cycle %3 (%4 cycles so far)").arg(lane(l.domain)).arg(l.a).arg(cyc(l.from)).arg(cyc(l.length()))
                          : tr("%1 handler of event %2: cycles %3 to %4 (%5 cycles)").arg(lane(l.domain)).arg(l.a).arg(cyc(l.from)).arg(cyc(l.to)).arg(cyc(l.length()));
        case TraceLink::Timer:
            if (l.lost) return tr("%1 timer: started at cycle %2 and started again at cycle %3 before it was done (%4 cycles)").arg(lane(l.domain)).arg(cyc(l.from)).arg(cyc(l.to)).arg(cyc(l.length()));
            return l.open ? tr("%1 timer: started at cycle %2, still counting (%3 cycles so far)").arg(lane(l.domain)).arg(cyc(l.from)).arg(cyc(l.length()))
                          : tr("%1 timer: started at cycle %2, done at cycle %3 (%4 cycles)").arg(lane(l.domain)).arg(cyc(l.from)).arg(cyc(l.to)).arg(cyc(l.length()));
        case TraceLink::Blanking:
            return l.open ? tr("Vertical blanking window: open since cycle %1").arg(cyc(l.from)) : tr("Vertical blanking window: cycles %1 to %2 (%3 cycles)").arg(cyc(l.from)).arg(cyc(l.to)).arg(cyc(l.length()));
    }
    return QString();
}

QString ExecutionView::describeMarker(int i) const {
    if (!cur_ || i < 0 || (size_t)i >= cur_->marks.size()) return QString();
    const TraceMarker& m = cur_->marks[(size_t)i];
    QString s;
    switch (m.kind) {
        case 'p': s = tr("Peri published a result: service %1, %2 byte(s) (cycle %3)").arg(m.a).arg(m.b).arg(cyc(m.at)); break;
        case 'c': s = tr("Main took the result of service %1 (cycle %2)").arg(m.a).arg(cyc(m.at)); break;
        case 'o': s = tr("Overrun: Peri published service %1 while the slot was still full (cycle %2)").arg(m.a).arg(cyc(m.at)); break;
        case 'e': s = tr("%1 event %2: %3 (cycle %4)").arg(lane(m.domain)).arg(m.a).arg(fromStd(m.what)).arg(cyc(m.at)); break;
        case 'b': s = tr("I2C %1: device 0x%2, %3, %4 byte(s) (cycle %5)").arg(fromStd(m.what)).arg(QString::number((qulonglong)m.a, 16).toUpper()).arg(m.b ? tr("acknowledged") : tr("NOT acknowledged")).arg(m.c).arg(cyc(m.at)); break;
        case 'f': s = tr("FAULT on %1: %2 (cycle %3)").arg(lane(m.domain)).arg(fromStd(m.what)).arg(cyc(m.at)); break;
        case 's': s = tr("%1 started (cycle %2)").arg(fromStd(m.what)).arg(cyc(m.at)); break;
        case 'd': s = tr("%1 is done (cycle %2)").arg(fromStd(m.what)).arg(cyc(m.at)); break;
        default: s = fromStd(m.what); break;
    }
    for (const TraceLink& l : links_)                                                        // what it belongs to
        if (l.fromMark == i || l.toMark == i) { s += "\n" + describeLink(l); break; }
    if (m.hasPc) s += "\n" + tr("Click: show the instruction that did it");
    return s;
}

class ExecutionPanel : public QWidget {
    Q_DECLARE_TR_FUNCTIONS(ExecutionPanel)
public:
    explicit ExecutionPanel(AppContext& ctx) : ctx_(ctx), store_(ctx.debugger.simParams()) {
        setObjectName("executionPanel");
        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(2, 2, 2, 2);
        auto* bar = new QHBoxLayout;
        run_ = new QPushButton(tr("Run"));
        run_->setObjectName("executionRun");
        auto_ = new QCheckBox(tr("run when a parameter changes"));
        auto_->setObjectName("executionAuto");
        auto_->setChecked(true);
        compare_ = new QComboBox;
        compare_->setObjectName("executionCompare");
        compare_->addItems({tr("compare with: initial values"), tr("compare with: saved run"), tr("compare with: nothing")});
        saveBase_ = new QPushButton(tr("Save this run"));
        saveBase_->setObjectName("executionSave");
        maxCycles_ = new QSpinBox;
        maxCycles_->setObjectName("executionCycles");
        maxCycles_->setRange(1000, 20000000);
        maxCycles_->setSingleStep(10000);
        maxCycles_->setValue(100000);
        maxCycles_->setSuffix(tr(" cycles"));
        fit_ = new QPushButton(tr("Fit"));
        fit_->setObjectName("executionFit");
        live_ = new QCheckBox(tr("live: the running simulator"));
        live_->setObjectName("executionLive");
        live_->setToolTip(tr("While debugging, the time line shows the history of the running simulator (the last instructions of both sequencers). Double click on it: wind the simulator back to that cycle."));
        live_->setChecked(ctx.settings.get("execution.live", "1") != "0");
        follow_ = new QCheckBox(tr("follow"));
        follow_->setObjectName("executionFollow");
        follow_->setToolTip(tr("Keep the newest part in view while it runs. Zooming or dragging the time line stops it; tick it again to catch up."));
        follow_->setEnabled(false);
        first_ = new QPushButton(tr("First difference"));
        first_->setObjectName("executionFirst");
        for (QWidget* w : std::initializer_list<QWidget*>{live_, follow_, run_, auto_, compare_, saveBase_, maxCycles_, fit_, first_}) bar->addWidget(w);
        bar->addStretch(1);
        lay->addLayout(bar);
        {   // what the time line shows: the kinds can be switched, each in its own colour
            auto* kinds = new QHBoxLayout;
            struct K { const char* name; QString text; const char* colour; unsigned bit; };
            const K ks[] = {{"executionShowEvents", tr("events"), "#d946ef", ExecutionView::ShowEvents}, {"executionShowBus", tr("bus"), "#14b8a6", ExecutionView::ShowBus},
                            {"executionShowResults", tr("results Peri to Main"), "#65a30d", ExecutionView::ShowResults}, {"executionShowTimers", tr("timers"), "#d97706", ExecutionView::ShowTimers},
                            {"executionShowBlanking", tr("blanking window"), "#3b82f6", ExecutionView::ShowBlanking}};
            for (const K& k : ks) {
                auto* c = new QCheckBox(k.text);
                c->setObjectName(k.name);
                c->setChecked(true);
                c->setStyleSheet(QString("QCheckBox { color:%1; font-weight:bold; }").arg(k.colour));
                const unsigned bit = k.bit;
                connect(c, &QCheckBox::toggled, this, [this, bit](bool on) { shownMask_ = on ? (shownMask_ | bit) : (shownMask_ & ~bit); view_->setShown(shownMask_); });
                kinds->addWidget(c);
            }
            kinds->addStretch(1);
            lay->addLayout(kinds);
        }
        summary_ = new QLabel;
        summary_->setObjectName("executionSummary");
        summary_->setWordWrap(true);
        lay->addWidget(summary_);
        stats_ = new QLabel;
        stats_->setObjectName("executionStats");
        stats_->setWordWrap(true);
        lay->addWidget(stats_);
        detail_ = new QLabel(tr("Hover over a marker, a bar or an arrow: what it is. Click a marker: the instruction that did it."));
        detail_->setObjectName("executionDetail");
        detail_->setWordWrap(true);
        detail_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        lay->addWidget(detail_);
        view_ = new ExecutionView;
        lay->addWidget(view_, 3);
        table_ = new QTableWidget(0, 6);
        table_->setObjectName("executionTable");
        table_->setHorizontalHeaderLabels({tr("Sequencer"), tr("#"), tr("Instruction"), tr("Baseline cycle"), tr("Cycle"), tr("Shift")});
        table_->verticalHeader()->hide();
        table_->horizontalHeader()->setStretchLastSection(true);
        table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table_->setMaximumHeight(150);
        lay->addWidget(table_, 1);
        timer_.setSingleShot(true);
        timer_.setInterval(250);
        connect(&timer_, &QTimer::timeout, this, [this] { run(); });
        connect(run_, &QPushButton::clicked, this, [this] { run(); });
        connect(fit_, &QPushButton::clicked, this, [this] { view_->fit(); });
        connect(first_, &QPushButton::clicked, this, [this] { firstDifference(); });
        connect(saveBase_, &QPushButton::clicked, this, [this] { saved_ = cur_; haveSaved_ = cur_.ok() && !cur_.rows.empty(); if (haveSaved_) compare_->setCurrentIndex(1); });
        connect(compare_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) { run(); });
        connect(table_, &QTableWidget::cellClicked, this, [this](int row, int) {
            QTableWidgetItem* it = table_->item(row, 1);
            if (!it) return;
            view_->focusRow(it->data(Qt::UserRole).toInt(), (size_t)it->data(Qt::UserRole + 1).toInt());
        });
        view_->clicked.connect([this](int d, size_t k) { goTo(d, k); });
        view_->markerClicked.connect([this](int i) { markerClicked(i); });
        view_->userMoved.connect([this] { QSignalBlocker b(follow_); follow_->setChecked(false); });
        view_->rewindRequested.connect([this](double t) { rewindTo((Cycle)t); });
        connect(live_, &QCheckBox::toggled, this, [this](bool on) { ctx_.settings.set("execution.live", on ? "1" : "0"); refreshMode(); });
        connect(follow_, &QCheckBox::toggled, this, [this](bool on) { view_->setFollow(on); });
        conns_.add(ctx.debugger.traceChanged, [this] { if (liveShown_) updateLive(); });
        conns_.add(ctx.debugger.updated, [this](const SimUpdate&) { refreshMode(); });
        conns_.add(store_.changed, [this] { if (auto_->isChecked()) timer_.start(); });
        conns_.add(ctx.snapshotChanged, [this](SnapshotPtr) { if (auto_->isChecked()) timer_.start(); });
        refreshMode();
        if (!liveShown_) run();
    }
    ~ExecutionPanel() override { timer_.stop(); }

private:
    // ---- live: while something is debugged the time line is the history of that session
    void refreshMode() {
        const bool active = ctx_.debugger.active();
        live_->setEnabled(active);
        const bool want = live_->isChecked() && active;
        if (want && !liveShown_) enterLive();
        else if (!want && liveShown_) leaveLive();
    }
    void enterLive() {
        liveShown_ = true;
        timer_.stop();
        for (QWidget* w : std::initializer_list<QWidget*>{run_, auto_, compare_, saveBase_, maxCycles_, first_}) w->setEnabled(false);
        table_->setRowCount(0);
        view_->setMessage(QString());
        view_->startLive();
        { QSignalBlocker b(follow_); follow_->setChecked(true); }
        follow_->setEnabled(true);
        updateLive();
    }
    void leaveLive() {
        liveShown_ = false;
        view_->endLive();
        for (QWidget* w : std::initializer_list<QWidget*>{run_, auto_, compare_, saveBase_, maxCycles_}) w->setEnabled(true);
        follow_->setEnabled(false);
        run();
    }
    void updateLive() {
        const TraceRing& ring = ctx_.debugger.trace();
        view_->setLive(&ring.trace(), &ring.gaps(), ring.trimEpoch());
        updateStats(ring.trace());
        const RunTrace& t = ring.trace();
        QString s = ctx_.debugger.running() ? tr("Live: running, at cycle %1").arg((qulonglong)t.endAt) : tr("Live: stopped at cycle %1").arg((qulonglong)t.endAt);
        s += tr("  \xC2\xB7  the history has %1 Main and %2 Peri instructions").arg((qulonglong)t.instrs[0]).arg((qulonglong)t.instrs[1]);
        if (ring.dropped() > 0) s += tr(" (it starts at cycle %1: %2 older ones were dropped)").arg((qulonglong)ring.firstCycle()).arg((qulonglong)ring.dropped());
        if (!ring.gaps().empty()) s += tr("  \xC2\xB7  %1 stretch(es) where the simulator ran too fast to record everything (hatched)").arg((int)ring.gaps().size());
        s += tr("\nDouble click on the time line: wind the simulator back to that cycle (what is later is forgotten; the run goes on from there).");
        summary_->setText(s);
        saveBase_->setEnabled(false);
    }
    const RunTrace& shownTrace() const { return liveShown_ ? ctx_.debugger.trace().trace() : cur_; }
    void markerClicked(int i) {
        const RunTrace& t = shownTrace();
        if (i < 0 || (size_t)i >= t.marks.size()) return;
        detail_->setText(view_->describeMarker(i));
        const TraceMarker& m = t.marks[(size_t)i];
        if (!m.hasPc || (m.domain != 0 && m.domain != 1)) return;
        const DebugLocation l = ctx_.debugger.locationForPc(m.domain == 0 ? asmcore::Domain::Main : asmcore::Domain::Peri, m.pc);
        if (l.valid) { Location loc; loc.file = l.file; loc.line = l.line; ctx_.showExecution.fire(loc); }
    }
    // what the markers add up to
    void updateStats(const RunTrace& t) {
        const TraceStats st = traceStats(t);
        auto avg = [](const TraceStats::Span& s) { return QString::number(s.avg(), 'f', s.avg() < 10 ? 1 : 0); };
        QStringList parts;
        if (st.published || st.taken || st.overruns) {
            QString s = tr("Results: %1 published, %2 taken").arg((qulonglong)st.published).arg((qulonglong)st.taken);
            if (st.resultLatency.n) s += tr(", waited avg %1 / max %2 cycles").arg(avg(st.resultLatency)).arg((qulonglong)st.resultLatency.max);
            if (st.lost || st.overruns) s += tr(", %1 lost (overrun)").arg((qulonglong)std::max(st.lost, st.overruns));
            parts << s;
        }
        if (st.eventsRaised) {
            QString s = tr("Events: %1 raised").arg((qulonglong)st.eventsRaised);
            if (st.eventWait.n) s += tr(", wait for a checkpoint avg %1 / max %2").arg(avg(st.eventWait)).arg((qulonglong)st.eventWait.max);
            if (st.eventHandler.n) s += tr(", handler avg %1 / max %2 cycles").arg(avg(st.eventHandler)).arg((qulonglong)st.eventHandler.max);
            parts << s;
        }
        if (st.timersStarted) {
            QString s = tr("Timers:");
            for (int d = 0; d < 2; d++) if (st.timerRun[d].n) s += tr(" %1 %2 runs avg %3 cycles;").arg(d == 0 ? "Main" : "Peri").arg((qulonglong)st.timerRun[d].n).arg(avg(st.timerRun[d]));
            if (st.timersCancelled) s += tr(" %1 started again before done").arg((qulonglong)st.timersCancelled);
            parts << s;
        }
        if (st.busTransfers) parts << tr("Bus: %1 transfer(s), %2 not acknowledged").arg((qulonglong)st.busTransfers).arg((qulonglong)st.busNacks);
        if (st.faults) parts << tr("%1 fault(s)").arg((qulonglong)st.faults);
        stats_->setText(parts.join("    \xC2\xB7    "));
        stats_->setVisible(!parts.isEmpty());
    }
    void rewindTo(Cycle t) {
        if (!ctx_.debugger.active()) return;
        if (ctx_.debugger.running()) ctx_.debugger.pause();
        ctx_.debugger.session().rewindTo(t);
        ctx_.status.fire(tr("Winding the simulator back to cycle %1").arg((qulonglong)t));
    }
    void run() {
        if (liveShown_) return;                                            // (the time line is the live session)
        SnapshotPtr prog;
        std::string env, why;
        asmcore::Profile pf;
        cur_ = RunTrace();
        base_ = RunTrace();
        diff_ = TraceDiff();
        haveBase_ = false;
        if (!ctx_.debugger.inputs(prog, env, pf, why)) {
            view_->setData(nullptr, nullptr, nullptr);
            view_->setMessage(fromStd(why));
            summary_->setText(fromStd(why));
            fillTable();
            return;
        }
        asmcore::sim::SimConfig cfg;
        cfg.profile = pf;
        cfg.env = env;
        cfg.params = store_.params();
        const Cycle limit = (Cycle)maxCycles_->value();
        cur_ = recordRun(prog->result, cfg, limit);
        if (!cur_.ok()) {
            view_->setData(nullptr, nullptr, nullptr);
            view_->setMessage(fromStd(cur_.error));
            summary_->setText(tr("The simulator could not be made: %1").arg(fromStd(cur_.error)));
            fillTable();
            return;
        }
        const int mode = compare_->currentIndex();
        if (mode == 0) {
            asmcore::sim::SimConfig b = cfg;
            b.params = SimParams();
            base_ = recordRun(prog->result, b, limit);
            haveBase_ = base_.ok();
        } else if (mode == 1 && haveSaved_) {
            base_ = saved_;
            haveBase_ = true;
        }
        if (haveBase_) diff_ = compareTraces(base_, cur_);
        view_->setMessage(QString());
        view_->setData(&cur_, haveBase_ ? &base_ : nullptr, haveBase_ ? &diff_ : nullptr);
        updateStats(cur_);
        fillTable();
        summary_->setText(summaryText());
        first_->setEnabled(haveBase_ && !diff_.moved.empty());
        saveBase_->setEnabled(!cur_.rows.empty());
    }
    QString summaryText() const {
        auto delta = [](long long v) { return QString(v > 0 ? "+" : "") + QString::number(v); };
        QString s = tr("Main: %1 instructions, Peri: %2 instructions; stopped at cycle %3").arg(cur_.instrs[0]).arg(cur_.instrs[1]).arg(cur_.endAt);
        if (cur_.truncated) s += tr(" (the first %1 instructions are shown)").arg(cur_.rows.size());
        if (!haveBase_) return s;
        s += "\n";
        if (diff_.same) return s + tr("The same as the baseline: not one cycle differs.");
        s += tr("Against the baseline: the end moves %1 cycles").arg(delta(diff_.endDelta));
        for (int d = 0; d < 2; d++) {
            if (diff_.total[d][0] == 0 && diff_.total[d][1] == 0) continue;
            s += QString("  \xC2\xB7  %1: ").arg(d == 0 ? "Main" : "Peri");
            if (diff_.maxShift[d] == 0) s += tr("no shift");
            else s += tr("shift up to %1 cycles, %2 at the end, first at instruction %3").arg(diff_.maxShift[d]).arg(delta(diff_.finalShift[d])).arg(diff_.firstMoved[d] + 1);
            if (diff_.diverged[d] != SIZE_MAX) s += tr(", the programs part at instruction %1 (compared up to there)").arg(diff_.diverged[d] + 1);
        }
        if (diff_.markShift != 0) s += tr("  \xC2\xB7  events and bus transfers move up to %1 cycles").arg(diff_.markShift);
        return s;
    }
    void fillTable() {
        table_->setRowCount(0);
        if (!haveBase_) return;
        const size_t n = std::min<size_t>(diff_.moved.size(), 400);
        table_->setRowCount((int)n);
        for (size_t i = 0; i < n; i++) {
            const RowDelta& r = diff_.moved[i];
            auto item = [](const QString& t) { auto* it = new QTableWidgetItem(t); return it; };
            table_->setItem((int)i, 0, item(r.domain == 0 ? "Main" : "Peri"));
            QTableWidgetItem* num = item(QString::number(r.index + 1));
            num->setData(Qt::UserRole, r.domain);
            num->setData(Qt::UserRole + 1, (int)r.index);
            table_->setItem((int)i, 1, num);
            table_->setItem((int)i, 2, item(fromStd(r.what)));
            table_->setItem((int)i, 3, item(QString::number(r.baseAt)));
            table_->setItem((int)i, 4, item(QString::number(r.curAt)));
            QTableWidgetItem* d = item((r.delta > 0 ? "+" : "") + QString::number(r.delta));
            d->setForeground(r.delta > 0 ? QColor("#e5484d") : QColor("#3b82f6"));
            table_->setItem((int)i, 5, d);
        }
        table_->resizeColumnsToContents();
    }
    void firstDifference() {
        if (!haveBase_ || diff_.moved.empty()) return;
        const RowDelta& r = *std::min_element(diff_.moved.begin(), diff_.moved.end(), [](const RowDelta& a, const RowDelta& b) { return a.curAt < b.curAt; });
        view_->focusRow(r.domain, r.index);
    }
    void goTo(int d, size_t k) {
        const RunTrace& source = liveShown_ ? ctx_.debugger.trace().trace() : cur_;
        if (!source.ok()) return;
        size_t n = 0;
        for (const TraceRow& r : source.rows) {
            if (r.domain != d) continue;
            if (n++ != k) continue;
            const DebugLocation l = ctx_.debugger.locationForPc(d == 0 ? asmcore::Domain::Main : asmcore::Domain::Peri, r.pc);
            if (l.valid) { Location loc; loc.file = l.file; loc.line = l.line; ctx_.goTo.fire(loc); }
            return;
        }
    }

    AppContext& ctx_;
    SimParamsStore& store_;
    QPushButton *run_, *saveBase_, *fit_, *first_;
    QCheckBox *auto_, *live_, *follow_;
    bool liveShown_ = false;
    QComboBox* compare_;
    QSpinBox* maxCycles_;
    QLabel *summary_, *stats_, *detail_;
    unsigned shownMask_ = 31;
    ExecutionView* view_;
    QTableWidget* table_;
    QTimer timer_;
    RunTrace cur_, base_, saved_;
    TraceDiff diff_;
    bool haveBase_ = false, haveSaved_ = false;
    ConnectionBag conns_;
};

}  // namespace

namespace {

// ---- the quick popup ---------------------------------------------------------------------------------------------------------------------
class QuickParams : public QFrame {
    Q_DECLARE_TR_FUNCTIONS(QuickParams)
public:
    QuickParams(AppContext& ctx, std::function<void(const std::string&)> showInTab) : QFrame(nullptr, Qt::Popup), ctx_(ctx), store_(ctx.debugger.simParams()), showInTab_(std::move(showInTab)) {
        setObjectName("quickParams");
        setAttribute(Qt::WA_DeleteOnClose);
        setFrameShape(QFrame::StyledPanel);
        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(6, 6, 6, 6);
        search_ = new QLineEdit;
        search_->setObjectName("quickSearch");
        search_->setPlaceholderText(tr("search parameters (name, group, what it does)"));
        search_->setClearButtonEnabled(true);
        lay->addWidget(search_);
        head_ = new QLabel;
        head_->setObjectName("quickHead");
        lay->addWidget(head_);
        auto* area = new QScrollArea;
        area->setFrameShape(QFrame::NoFrame);
        area->setWidgetResizable(true);
        area->setMinimumHeight(220);
        rows_ = new QWidget;
        rowsLay_ = new QVBoxLayout(rows_);
        rowsLay_->setContentsMargins(0, 0, 0, 0);
        rowsLay_->setSpacing(2);
        rowsLay_->addStretch(1);
        area->setWidget(rows_);
        lay->addWidget(area, 1);
        auto* foot = new QLabel(tr("\xE2\x86\x92 shows it in the Simulator page  \xC2\xB7  \xE2\x98\x85 pins it here  \xC2\xB7  \xE2\x86\xBA back to the initial value"));
        foot->setEnabled(false);
        lay->addWidget(foot);
        resize(520, 360);
        connect(search_, &QLineEdit::textChanged, this, [this] { rebuild(); });
        connect(search_, &QLineEdit::returnPressed, this, [this] { if (!rows_vec_.empty()) rows_vec_[0].editor->setFocus(); });
        search_->installEventFilter(this);
        conns_.add(store_.changed, [this] { sync(); });
        conns_.add(ctx.settings.changed, [this](const std::string& key) { if (key.compare(0, 16, "simparams.pinned") == 0 && search_->text().trimmed().isEmpty()) rebuild(); });
        rebuild();
    }
    void popupUnder(QWidget* anchor) {
        QPoint at = anchor ? anchor->mapToGlobal(QPoint(0, anchor->height())) : QCursor::pos();
        if (QScreen* s = QGuiApplication::screenAt(at)) {
            const QRect a = s->availableGeometry();
            at.setX(std::max(a.left(), std::min(at.x(), a.right() - width())));
            at.setY(std::min(at.y(), a.bottom() - height()));
        }
        move(at);
        show();
        search_->setFocus();
    }

protected:
    bool eventFilter(QObject* o, QEvent* e) override {
        if (o == search_ && e->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(e)->key() == Qt::Key_Down && !rows_vec_.empty()) { rows_vec_[0].editor->setFocus(); return true; }
        return QFrame::eventFilter(o, e);
    }

private:
    struct Row { std::string key; QWidget* box; QWidget* editor; QLabel* name; QToolButton* pin; };
    void rebuild() {
        for (Row& r : rows_vec_) {                                            // gone from the popup at once (a button of a row may be the one that asked for this)
            rowsLay_->removeWidget(r.box);
            r.box->hide();
            r.box->setParent(nullptr);
            r.box->deleteLater();
        }
        rows_vec_.clear();
        const QString f = search_->text().trimmed();
        std::vector<std::pair<int, const ParamDef*>> found;
        if (f.isEmpty()) {
            for (const std::string& k : pinnedKeys(ctx_.settings)) if (const ParamDef* d = asmcore::findParam(k)) found.push_back({1, d});
            head_->setText(found.empty() ? tr("Nothing is pinned: search above, or pin a parameter with \xE2\x98\x85.") : tr("Pinned"));
        } else {
            for (const ParamDef& d : paramCatalog()) if (const int s = paramScore(d, f)) found.push_back({s, &d});
            std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            if (found.size() > 14) found.resize(14);
            head_->setText(found.empty() ? tr("No parameter matches.") : tr("Matches"));
        }
        for (const auto& fd : found) {
            const ParamDef& d = *fd.second;
            auto* box = new QWidget;
            auto* h = new QHBoxLayout(box);
            h->setContentsMargins(2, 0, 2, 0);
            auto* name = new QLabel(fromStd(d.key));
            name->setToolTip(fromStd(d.help + (d.measure.empty() ? std::string() : "\nmeasure: " + d.measure)));
            name->setMinimumWidth(170);
            QWidget* ed = valueEditor(store_, d, box);
            auto* unit = new QLabel(fromStd(d.unit));
            unit->setEnabled(false);
            auto* pin = new QToolButton;
            pin->setAutoRaise(true);
            pin->setToolTip(tr("Pin here"));
            auto* reset = new QToolButton;
            reset->setAutoRaise(true);
            reset->setText(QString::fromUtf8("\xE2\x86\xBA"));
            reset->setToolTip(tr("Back to the initial value"));
            auto* go = new QToolButton;
            go->setAutoRaise(true);
            go->setText(QString::fromUtf8("\xE2\x86\x92"));
            go->setToolTip(tr("Show it in the Simulator page"));
            h->addWidget(name);
            h->addWidget(ed);
            h->addWidget(unit, 1);
            h->addWidget(pin);
            h->addWidget(reset);
            h->addWidget(go);
            const std::string key = d.key;
            connect(pin, &QToolButton::clicked, this, [this, key] { setPinned(ctx_.settings, key, !isPinned(ctx_.settings, key)); sync(); });
            connect(reset, &QToolButton::clicked, this, [this, key] { store_.clear(key); });
            connect(go, &QToolButton::clicked, this, [this, key] { close(); if (showInTab_) showInTab_(key); });
            rowsLay_->insertWidget(rowsLay_->count() - 1, box);
            rows_vec_.push_back({key, box, ed, name, pin});
        }
        sync();
    }
    // the values, the "changed" look and the pins follow the store (an editor that has the focus is left alone)
    void sync() {
        for (Row& r : rows_vec_) {
            syncEditor(r.editor, store_, r.key);
            QFont f = r.name->font();
            f.setBold(!store_.params().atDefault(r.key));
            r.name->setFont(f);
            const bool pinned = isPinned(ctx_.settings, r.key);
            r.pin->setText(pinned ? QString::fromUtf8("\xE2\x98\x85") : QString::fromUtf8("\xE2\x98\x86"));
            const ParamDef* d = asmcore::findParam(r.key);
            r.name->setText(fromStd(r.key) + (d && !d->active ? tr("  (not used yet)") : QString()));
        }
    }
    AppContext& ctx_;
    SimParamsStore& store_;
    std::function<void(const std::string&)> showInTab_;
    QLineEdit* search_;
    QLabel* head_;
    QWidget* rows_;
    QVBoxLayout* rowsLay_;
    std::vector<Row> rows_vec_;
    ConnectionBag conns_;
};

// ---- the simulator page ---------------------------------------------------------------------------------------------------------------------
class SimulatorTab : public QWidget, public DocumentTab {
    Q_DECLARE_TR_FUNCTIONS(SimulatorTab)
public:
    explicit SimulatorTab(AppContext& ctx) : ctx_(ctx) {
        setObjectName("simulatorTab");
        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(0, 0, 0, 0);
        auto* split = new QSplitter(Qt::Horizontal);
        split->setObjectName("simulatorSplit");
        params_ = new ParamsPanel(ctx);
        right_ = new QSplitter(Qt::Vertical);
        right_->setObjectName("simulatorRight");
        right_->addWidget(createIsaTopPanel(ctx));                         // the ISA Top diagram, live ...
        right_->addWidget(new ExecutionPanel(ctx));                        // ... above the time line of the same run
        right_->setStretchFactor(0, 3);
        right_->setStretchFactor(1, 2);
        right_->setSizes({420, 300});
        split->addWidget(params_);
        split->addWidget(right_);
        split->setStretchFactor(0, 2);
        split->setStretchFactor(1, 3);
        split->setSizes({470, 760});
        lay->addWidget(split);
    }
    // DocumentTab: one page per window. The buffer of the parameter file (if the values belong to one) is what makes the tab show "unsaved".
    std::string tabPath() const override { return "simulator:"; }
    QString tabTitle() const override { return tr("Simulator"); }
    std::vector<std::string> workspacePaths() const override {
        const std::string& p = ctx_.debugger.simParams().path();
        return p.empty() ? std::vector<std::string>() : std::vector<std::string>{p};
    }
    QWidget* widget() override { return this; }
    void showParameter(const std::string& key) { params_->showParameter(key); }
    void focusSearch() { params_->focusSearch(); }

private:
    AppContext& ctx_;
    ParamsPanel* params_;
    QSplitter* right_;
};

}  // namespace

QWidget* createSimulatorTab(AppContext& ctx) { return new SimulatorTab(ctx); }

void showParameterInTab(QWidget* tab, const std::string& key) {
    if (auto* t = dynamic_cast<SimulatorTab*>(tab)) t->showParameter(key);
}

void focusParameterSearch(QWidget* tab) {
    if (auto* t = dynamic_cast<SimulatorTab*>(tab)) t->focusSearch();
}

void showQuickParams(AppContext& ctx, QWidget* anchor, std::function<void(const std::string&)> showInTab) {
    auto* q = new QuickParams(ctx, std::move(showInTab));
    q->popupUnder(anchor);
}

QString simParamsSummary(AppContext& ctx) {
    SimParamsStore& s = ctx.debugger.simParams();
    const int n = s.changedCount();
    QString t = n == 0 ? QCoreApplication::translate("SimParams", "Parameters: initial values") : QCoreApplication::translate("SimParams", "Parameters: %n changed", nullptr, n);
    if (!s.path().empty()) t += QString::fromUtf8(" \xC2\xB7 ") + QFileInfo(fromStd(s.path())).fileName();
    if (s.dirty()) t += " *";
    return t;
}

}  // namespace acide
