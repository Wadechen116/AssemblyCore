#include "hardware_designer.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTableWidget>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>

#include "asmcore/sim.h"
#include "hardware_link.h"

namespace acide {

namespace {

QString q(const std::string& s) { return QString::fromStdString(s); }
std::string s(const QString& t) { return t.toStdString(); }

bool parseNumber(const QString& text, long long& out) {
    QString t = text.trimmed();
    if (t.isEmpty()) return false;
    bool ok = false;
    long long v = 0;
    if (t.startsWith("0x") || t.startsWith("0X")) v = t.mid(2).toLongLong(&ok, 16);
    else if (t.startsWith("0b") || t.startsWith("0B")) v = t.mid(2).toLongLong(&ok, 2);
    else v = t.toLongLong(&ok, 10);
    if (ok) out = v;
    return ok;
}
QString hex(long long v, int digits) { return "0x" + QString::number(v, 16).toUpper().rightJustified(digits, '0'); }

QStringList actionNames() {
    QStringList l;
    const asmcore::sim::Extensions ext = asmcore::sim::Extensions::defaults();          // keep it alive: actions() refers into it
    for (auto& kv : ext.actions()) l << q(kv.first);
    l.sort();
    return l;
}
QStringList deviceKinds() {
    QStringList l;
    const asmcore::sim::Extensions ext = asmcore::sim::Extensions::defaults();
    for (auto& kv : ext.devices()) l << q(kv.first);
    l.sort();
    return l;
}

const char* const kRoleNames[] = {"", "timer_start", "timer_clear", "poll_arm", "poll_clear"};
const char* const kDevKeyTypes[] = {"reg", "regseq", "block", "default", "nack", "write_nack", "extra_cycles"};
bool keyHasAddress(const QString& type) { return type == "reg" || type == "regseq" || type == "block"; }

}  // namespace

HardwareDesigner::HardwareDesigner(AppContext& ctx, const std::string& achwPath, QWidget* parent) : QWidget(parent), ctx_(ctx), path_(achwPath) {
    setObjectName("hardwareDesigner");
    build();
    std::vector<std::string> errs;
    loaded_ = loadHardware(ctx_.workspace, path_, hw_, errs);
    if (!loaded_) {
        QString m = tr("This hardware description cannot be read:") + "\n";
        for (auto& e : errs) m += q(e) + "\n";
        banner_->setText(m);
        banner_->show();
        tree_->setEnabled(false);
    } else {
        banner_->hide();
        fillTree(Sel());
        refresh();
    }
    timer_.setSingleShot(true);
    timer_.setInterval(150);
    connect(&timer_, &QTimer::timeout, this, &HardwareDesigner::refresh);
    connection_ = ctx_.workspace.documentChanged.connect([this](const std::string& p) { if (p == path_ && !applying_) reloadFromWorkspace(); });
}

HardwareDesigner::~HardwareDesigner() { ctx_.workspace.documentChanged.disconnect(connection_); }

// ---- building the window ---------------------------------------------------------------------------------------------------------
void HardwareDesigner::build() {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    banner_ = new QLabel;
    banner_->setWordWrap(true);
    banner_->setStyleSheet("background:#fff3cd;color:#664d03;padding:6px;");
    outer->addWidget(banner_);
    auto* split = new QSplitter(Qt::Vertical);
    outer->addWidget(split, 1);
    auto* top = new QSplitter(Qt::Horizontal);
    split->addWidget(top);

    // left: the components
    auto* left = new QWidget;
    auto* ll = new QVBoxLayout(left);
    ll->setContentsMargins(4, 4, 4, 4);
    auto* bar = new QHBoxLayout;
    auto* add = new QToolButton;
    add->setObjectName("hwAdd");
    add->setText(tr("Add"));
    add->setPopupMode(QToolButton::InstantPopup);
    auto* menu = new QMenu(add);
    menu->addAction(tr("CSR register"), this, [this] { addComponent("csr"); });
    menu->addAction(tr("I2C device"), this, [this] { addComponent("device"); });
    menu->addAction(tr("Service"), this, [this] { addComponent("service"); });
    menu->addAction(tr("Constant"), this, [this] { addComponent("const"); });
    menu->addAction(tr("Named value (.equ)"), this, [this] { addComponent("equ"); });
    menu->addAction(tr("Environment section"), this, [this] { addComponent("env"); });
    add->setMenu(menu);
    removeBtn_ = new QPushButton(tr("Remove"));
    removeBtn_->setObjectName("hwRemove");
    connect(removeBtn_, &QPushButton::clicked, this, [this] { removeCurrent(); });
    bar->addWidget(add);
    bar->addWidget(removeBtn_);
    bar->addStretch(1);
    ll->addLayout(bar);
    tree_ = new QTreeWidget;
    tree_->setObjectName("hwTree");
    tree_->setHeaderHidden(true);
    tree_->setMinimumWidth(220);
    ll->addWidget(tree_, 1);
    top->addWidget(left);
    connect(tree_, &QTreeWidget::currentItemChanged, this, [this] { onSelectionChanged(); });

    // right: the property form of the selected component
    pages_ = new QStackedWidget;
    pageGeneral_ = pages_->addWidget(buildGeneral());
    pageEqu_ = pages_->addWidget(buildEqu());
    pageConst_ = pages_->addWidget(buildConst());
    pageCsr_ = pages_->addWidget(buildCsr());
    pageDevice_ = pages_->addWidget(buildDevice());
    pageService_ = pages_->addWidget(buildService());
    pageEnv_ = pages_->addWidget(buildEnv());
    pageRaw_ = pages_->addWidget(buildRaw());
    pageGroup_ = pages_->addWidget(buildGroup());
    top->addWidget(pages_);
    top->setStretchFactor(1, 1);

    // bottom: problems and the generated files
    bottom_ = new QTabWidget;
    bottom_->setObjectName("hwBottom");
    auto* pw = new QWidget;
    auto* pl = new QVBoxLayout(pw);
    pl->setContentsMargins(2, 2, 2, 2);
    summary_ = new QLabel;
    summary_->setObjectName("hwSummary");
    pl->addWidget(summary_);
    problemsTable_ = new QTableWidget(0, 3);
    problemsTable_->setObjectName("hwProblems");
    problemsTable_->setHorizontalHeaderLabels({tr("Kind"), tr("Where"), tr("Problem")});
    problemsTable_->horizontalHeader()->setStretchLastSection(true);
    problemsTable_->verticalHeader()->hide();
    problemsTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    problemsTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    connect(problemsTable_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) { showProblemTarget(row); });
    pl->addWidget(problemsTable_, 1);
    bottom_->addTab(pw, tr("Problems"));
    auto view = [&](QPlainTextEdit*& edit, const QString& name, const char* objectName) {
        edit = new QPlainTextEdit;
        edit->setObjectName(objectName);
        edit->setReadOnly(true);
        edit->setLineWrapMode(QPlainTextEdit::NoWrap);
        QFont f("Consolas");
        f.setStyleHint(QFont::Monospace);
        edit->setFont(f);
        bottom_->addTab(edit, name);
    };
    view(incView_, tr("Generated definitions (.inc)"), "hwInc");
    view(envView_, tr("Generated environment"), "hwEnv");
    split->addWidget(bottom_);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 1);
}

QWidget* HardwareDesigner::buildGroup() {
    auto* w = new QWidget;
    auto* l = new QVBoxLayout(w);
    auto* t = new QLabel(tr("Select a component on the left, or use Add.\n\n"
                            "The hardware description is the single place where the board is defined: CSR registers and what their bits do, I2C devices, "
                            "service payload contracts and constants. The assembler definitions and the simulator environment are generated from it, so "
                            "they can never disagree."));
    t->setWordWrap(true);
    t->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    l->addWidget(t);
    l->addStretch(1);
    return w;
}

QWidget* HardwareDesigner::buildGeneral() {
    auto* w = new QWidget;
    auto* f = new QFormLayout(w);
    gName_ = new QLineEdit;
    gDesc_ = new QLineEdit;
    gInc_ = new QLineEdit;
    gInc_->setPlaceholderText(tr("(default: next to the description, NAME.gen.inc)"));
    gEnv_ = new QLineEdit;
    gEnv_->setPlaceholderText(tr("(default: next to the description, NAME.gen.env.cfg)"));
    gName_->setObjectName("hwGName");
    f->addRow(tr("Name"), gName_);
    f->addRow(tr("Description"), gDesc_);
    f->addRow(tr("Definitions file"), gInc_);
    f->addRow(tr("Environment file"), gEnv_);
    auto* help = new QLabel(tr("The generated files are written to these names (relative to this description). Pointing them at the files a project already "
                               "uses lets the project adopt the designer without changing any source."));
    help->setWordWrap(true);
    f->addRow(help);
    auto edit = [this](auto fn) { return [this, fn](const QString& t) { if (loading_) return; fn(t); edited(); }; };
    connect(gName_, &QLineEdit::textEdited, this, edit([this](const QString& t) { hw_.name = s(t); }));
    connect(gDesc_, &QLineEdit::textEdited, this, edit([this](const QString& t) { hw_.description = s(t); }));
    connect(gInc_, &QLineEdit::textEdited, this, edit([this](const QString& t) { hw_.includeFile = s(t.trimmed()); }));
    connect(gEnv_, &QLineEdit::textEdited, this, edit([this](const QString& t) { hw_.envFile = s(t.trimmed()); }));
    return w;
}

QWidget* HardwareDesigner::buildEqu() {
    auto* w = new QWidget;
    auto* f = new QFormLayout(w);
    eName_ = new QLineEdit;
    eName_->setObjectName("hwEName");
    eValue_ = new QLineEdit;
    eDesc_ = new QLineEdit;
    f->addRow(tr("Name"), eName_);
    f->addRow(tr("Value (expression)"), eValue_);
    f->addRow(tr("Description"), eDesc_);
    auto bind = [this](QLineEdit* e, auto fn) {
        connect(e, &QLineEdit::textEdited, this, [this, fn](const QString& t) {
            if (loading_) return;
            Sel c = current();
            if (c.kind != Kind::Equ || c.index < 0) return;
            fn(hw_.equs[c.index], t);
            edited();
        });
    };
    bind(eName_, [](asmcore::HwEqu& x, const QString& t) { x.name = s(t.trimmed()); });
    bind(eValue_, [](asmcore::HwEqu& x, const QString& t) { x.value = s(t.trimmed()); });
    bind(eDesc_, [](asmcore::HwEqu& x, const QString& t) { x.description = s(t); });
    return w;
}

QWidget* HardwareDesigner::buildConst() {
    auto* w = new QWidget;
    auto* f = new QFormLayout(w);
    kName_ = new QLineEdit;
    kName_->setObjectName("hwKName");
    kWidth_ = new QComboBox;
    kWidth_->addItem(tr("1 byte (db)"), 1);
    kWidth_->addItem(tr("2 bytes (dw)"), 2);
    kValues_ = new QLineEdit;
    kValues_->setPlaceholderText(tr("comma separated, e.g. 300, 25"));
    kDesc_ = new QLineEdit;
    f->addRow(tr("Name"), kName_);
    f->addRow(tr("Width"), kWidth_);
    f->addRow(tr("Values"), kValues_);
    f->addRow(tr("Description"), kDesc_);
    auto help = new QLabel(tr("Constants live in MAIN_CONST_POOL (256 bytes). A program refers to one with MAIN_CONST_POOL[@NAME]."));
    help->setWordWrap(true);
    f->addRow(help);
    auto cur = [this]() -> asmcore::HwConst* { Sel c = current(); return c.kind == Kind::Const && c.index >= 0 ? &hw_.consts[c.index] : nullptr; };
    connect(kName_, &QLineEdit::textEdited, this, [this, cur](const QString& t) { if (loading_ || !cur()) return; cur()->name = s(t.trimmed()); edited(); });
    connect(kDesc_, &QLineEdit::textEdited, this, [this, cur](const QString& t) { if (loading_ || !cur()) return; cur()->description = s(t); edited(); });
    connect(kValues_, &QLineEdit::textEdited, this, [this, cur](const QString& t) {
        if (loading_ || !cur()) return;
        cur()->values.clear();
        for (const QString& p : t.split(',', Qt::SkipEmptyParts)) cur()->values.push_back(s(p.trimmed()));
        edited();
    });
    connect(kWidth_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, cur](int) { if (loading_ || !cur()) return; cur()->width = kWidth_->currentData().toInt(); edited(); });
    return w;
}

QWidget* HardwareDesigner::buildCsr() {
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    auto* f = new QFormLayout;
    cForm_ = f;
    v->addLayout(f);
    cName_ = new QLineEdit;
    cName_->setObjectName("hwCName");
    cAddr_ = new QLineEdit;
    cAddr_->setObjectName("hwCAddr");
    cAddr_->setPlaceholderText("0x000 .. 0xFFF");
    cKind_ = new QComboBox;
    cKind_->setObjectName("hwCKind");
    cKind_->addItem(tr("ram - a plain 8-bit register"), "ram");
    cKind_->addItem(tr("value - reads / writes a simulator signal"), "value");
    cKind_->addItem(tr("action - writing 1 to a bit does something"), "action");
    cKind_->addItem(tr("status - read-only bits from expressions"), "status");
    cKind_->addItem(tr("const - read-only constant"), "const");
    cDesc_ = new QLineEdit;
    cSim_ = new QCheckBox(tr("The simulator has this register (untick for a name only the assembler uses, e.g. the second byte of a 2-byte value)"));
    f->addRow(tr("Name"), cName_);
    f->addRow(tr("Address"), cAddr_);
    f->addRow(tr("Kind"), cKind_);
    f->addRow(tr("Description"), cDesc_);
    f->addRow(QString(), cSim_);
    cReset_ = new QSpinBox;
    cReset_->setRange(0, 255);
    cResetRow_ = cReset_;
    f->addRow(tr("Reset value"), cReset_);
    cBind_ = new QComboBox;
    cBind_->setEditable(true);
    cBind_->addItems({"", "timer_period", "poll_mask"});
    cBindRow_ = cBind_;
    f->addRow(tr("Signal (bind)"), cBind_);
    cBytes_ = new QSpinBox;
    cBytes_->setRange(1, 4);
    cBytesRow_ = cBytes_;
    f->addRow(tr("Bytes of the signal"), cBytes_);
    cConst_ = new QLineEdit;
    cConstRow_ = cConst_;
    f->addRow(tr("Constant"), cConst_);
    cBitsHelp_ = new QLabel;
    cBitsHelp_->setWordWrap(true);
    v->addWidget(cBitsHelp_);
    cBits_ = new QTableWidget(8, 3);
    cBits_->setObjectName("hwBits");
    cBits_->setHorizontalHeaderLabels({tr("Bit"), tr("Simulator: action / expression"), tr("Assembler role (.csrbit)")});
    cBits_->verticalHeader()->hide();
    cBits_->horizontalHeader()->setStretchLastSection(true);
    cBits_->setColumnWidth(0, 40);
    cBits_->setColumnWidth(1, 300);
    cBits_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    const QStringList actions = actionNames();
    for (int b = 0; b < 8; b++) {
        auto* num = new QTableWidgetItem(QString::number(b));
        num->setFlags(Qt::ItemIsEnabled);
        cBits_->setItem(b, 0, num);
        auto* act = new QComboBox;
        act->setEditable(true);
        act->addItem(QString());
        act->addItems(actions);
        cBits_->setCellWidget(b, 1, act);
        auto* role = new QComboBox;
        for (const char* r : kRoleNames) role->addItem(r[0] ? q(r) : tr("(none)"), q(r));
        cBits_->setCellWidget(b, 2, role);
        connect(act, &QComboBox::editTextChanged, this, [this, b](const QString& t) {
            if (loading_) return;
            Sel c = current();
            if (c.kind != Kind::Csr || c.index < 0) return;
            hw_.csrs[c.index].bit[b] = s(t.trimmed());
            edited();
        });
        connect(role, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, b, role](int) {
            if (loading_) return;
            Sel c = current();
            if (c.kind != Kind::Csr || c.index < 0) return;
            hw_.csrs[c.index].role[b] = s(role->currentData().toString());
            edited();
        });
    }
    v->addWidget(cBits_, 1);
    auto cur = [this]() -> asmcore::HwCsr* { Sel c = current(); return c.kind == Kind::Csr && c.index >= 0 ? &hw_.csrs[c.index] : nullptr; };
    connect(cName_, &QLineEdit::textEdited, this, [this, cur](const QString& t) { if (loading_ || !cur()) return; cur()->name = s(t.trimmed()); edited(); });
    connect(cDesc_, &QLineEdit::textEdited, this, [this, cur](const QString& t) { if (loading_ || !cur()) return; cur()->description = s(t); edited(); });
    connect(cAddr_, &QLineEdit::textEdited, this, [this, cur](const QString& t) {
        if (loading_ || !cur()) return;
        long long a;
        cur()->addr = parseNumber(t, a) ? (int)a : -1;
        edited();
    });
    connect(cKind_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, cur](int) {
        if (loading_ || !cur()) return;
        cur()->kind = s(cKind_->currentData().toString());
        load(current());                                   // rows and the bit column depend on the kind
        edited();
    });
    connect(cSim_, &QCheckBox::toggled, this, [this, cur](bool on) { if (loading_ || !cur()) return; cur()->simulated = on; edited(); });
    connect(cReset_, QOverload<int>::of(&QSpinBox::valueChanged), this, [this, cur](int x) { if (loading_ || !cur()) return; cur()->reset = x; edited(); });
    connect(cBytes_, QOverload<int>::of(&QSpinBox::valueChanged), this, [this, cur](int x) { if (loading_ || !cur()) return; cur()->bytes = x; edited(); });
    connect(cBind_, &QComboBox::editTextChanged, this, [this, cur](const QString& t) { if (loading_ || !cur()) return; cur()->bind = s(t.trimmed()); edited(); });
    connect(cConst_, &QLineEdit::textEdited, this, [this, cur](const QString& t) { if (loading_ || !cur()) return; long long a = 0; parseNumber(t, a); cur()->constVal = a; edited(); });
    return w;
}

QWidget* HardwareDesigner::buildDevice() {
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    auto* f = new QFormLayout;
    v->addLayout(f);
    dName_ = new QLineEdit;
    dName_->setObjectName("hwDName");
    dSymbol_ = new QLineEdit;
    dSymbol_->setPlaceholderText(tr("(default DEV_<NAME>)"));
    dAddr_ = new QLineEdit;
    dAddr_->setObjectName("hwDAddr");
    dAddr_->setPlaceholderText("0x00 .. 0x7F");
    dIndex_ = new QComboBox;
    dIndex_->addItem(tr("(not in the DeviceMap)"), -1);
    for (int i = 0; i < 8; i++) dIndex_->addItem(tr("DeviceMap entry %1").arg(i), i);
    dKind_ = new QComboBox;
    dKind_->addItems(deviceKinds());
    dSim_ = new QCheckBox(tr("Simulate this device (untick: the DeviceMap entry exists for the assembler, the simulator answers NACK)"));
    dDesc_ = new QLineEdit;
    f->addRow(tr("Name"), dName_);
    f->addRow(tr("DeviceMap name (EXEC_XFER dev=...)"), dSymbol_);
    f->addRow(tr("I2C address (7 bit)"), dAddr_);
    f->addRow(tr("DeviceMap"), dIndex_);
    f->addRow(tr("Kind"), dKind_);
    f->addRow(QString(), dSim_);
    f->addRow(tr("Description"), dDesc_);
    auto* help = new QLabel(tr("What the device returns (the simulator's scripted device):\n"
                               "reg  A = v1, v2, ...  reads of register A return these in turn, then stay on the last\n"
                               "regseq A = ...  the same, but cycling\n"
                               "block A = b0 b1 b2 ...  consecutive registers from A\n"
                               "default, nack, write_nack, extra_cycles: whole-device behaviour"));
    help->setWordWrap(true);
    v->addWidget(help);
    dRegs_ = new QTableWidget(0, 3);
    dRegs_->setObjectName("hwRegs");
    dRegs_->setHorizontalHeaderLabels({tr("Type"), tr("Register"), tr("Values")});
    dRegs_->horizontalHeader()->setStretchLastSection(true);
    dRegs_->setColumnWidth(0, 130);
    dRegs_->setColumnWidth(1, 90);
    dRegs_->verticalHeader()->hide();
    v->addWidget(dRegs_, 1);
    auto* bar = new QHBoxLayout;
    auto* addReg = new QPushButton(tr("Add row"));
    addReg->setObjectName("hwRegAdd");
    auto* delReg = new QPushButton(tr("Remove row"));
    bar->addWidget(addReg);
    bar->addWidget(delReg);
    bar->addStretch(1);
    v->addLayout(bar);
    auto cur = [this]() -> asmcore::HwDevice* { Sel c = current(); return c.kind == Kind::Device && c.index >= 0 ? &hw_.devices[c.index] : nullptr; };
    // table -> keys
    auto store = [this, cur] {
        if (loading_ || !cur()) return;
        cur()->keys.clear();
        for (int r = 0; r < dRegs_->rowCount(); r++) {
            auto* type = qobject_cast<QComboBox*>(dRegs_->cellWidget(r, 0));
            if (!type) continue;
            const QString t = type->currentText();
            QString key = t;
            if (keyHasAddress(t)) key += " " + (dRegs_->item(r, 1) ? dRegs_->item(r, 1)->text().trimmed() : QString());
            cur()->keys.push_back({s(key), s(dRegs_->item(r, 2) ? dRegs_->item(r, 2)->text().trimmed() : QString())});
        }
        edited();
    };
    auto addRow = [this, store](const QString& type, const QString& addr, const QString& values) {
        const int r = dRegs_->rowCount();
        dRegs_->insertRow(r);
        auto* c = new QComboBox;
        for (const char* k : kDevKeyTypes) c->addItem(k);
        c->setCurrentText(type);
        dRegs_->setCellWidget(r, 0, c);
        auto* a = new QTableWidgetItem(addr);
        if (!keyHasAddress(type)) a->setFlags(Qt::NoItemFlags);
        dRegs_->setItem(r, 1, a);
        dRegs_->setItem(r, 2, new QTableWidgetItem(values));
        connect(c, &QComboBox::currentTextChanged, this, [this, c, store](const QString& t) {
            for (int row = 0; row < dRegs_->rowCount(); row++)                      // rows move when others are removed: find this one
                if (dRegs_->cellWidget(row, 0) == c)
                    if (auto* a2 = dRegs_->item(row, 1)) a2->setFlags(keyHasAddress(t) ? (Qt::ItemIsEnabled | Qt::ItemIsEditable | Qt::ItemIsSelectable) : Qt::NoItemFlags);
            store();
        });
    };
    connect(dRegs_, &QTableWidget::itemChanged, this, [store](QTableWidgetItem*) { store(); });
    connect(addReg, &QPushButton::clicked, this, [this, addRow, store] { if (loading_) return; addRow("reg", "0x00", "0x00"); store(); });
    connect(delReg, &QPushButton::clicked, this, [this, store] { if (loading_) return; int r = dRegs_->currentRow(); if (r >= 0) { dRegs_->removeRow(r); store(); } });
    connect(dName_, &QLineEdit::textEdited, this, [this, cur](const QString& t) { if (loading_ || !cur()) return; cur()->name = s(t.trimmed()); edited(); });
    connect(dSymbol_, &QLineEdit::textEdited, this, [this, cur](const QString& t) { if (loading_ || !cur()) return; cur()->symbol = s(t.trimmed()); edited(); });
    connect(dDesc_, &QLineEdit::textEdited, this, [this, cur](const QString& t) { if (loading_ || !cur()) return; cur()->description = s(t); edited(); });
    connect(dAddr_, &QLineEdit::textEdited, this, [this, cur](const QString& t) { if (loading_ || !cur()) return; long long a; cur()->addr = parseNumber(t, a) ? (int)a : -1; edited(); });
    connect(dIndex_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, cur](int) { if (loading_ || !cur()) return; cur()->index = dIndex_->currentData().toInt(); edited(); });
    connect(dKind_, &QComboBox::currentTextChanged, this, [this, cur](const QString& t) { if (loading_ || !cur()) return; cur()->kind = s(t); edited(); });
    connect(dSim_, &QCheckBox::toggled, this, [this, cur](bool on) { if (loading_ || !cur()) return; cur()->simulated = on; edited(); });
    addRowFn_ = addRow;                                  // load() fills the table with it
    return w;
}

QWidget* HardwareDesigner::buildService() {
    auto* w = new QWidget;
    auto* f = new QFormLayout(w);
    sName_ = new QLineEdit;
    sName_->setObjectName("hwSName");
    sId_ = new QSpinBox;
    sId_->setObjectName("hwSId");
    sId_->setRange(0, 255);
    sLen_ = new QSpinBox;
    sLen_->setRange(0, 64);
    sLen_->setSpecialValueText(tr("not declared"));
    sWire_ = new QComboBox;
    sWire_->addItem(tr("not declared"), "?");
    sWire_->addItem(tr("big - high byte first (as the sensor sends it)"), "B");
    sWire_->addItem(tr("little - low byte first"), "L");
    sDesc_ = new QLineEdit;
    f->addRow(tr("Name"), sName_);
    f->addRow(tr("Service id (1..15)"), sId_);
    f->addRow(tr("Payload bytes"), sLen_);
    f->addRow(tr("Byte order on the wire"), sWire_);
    f->addRow(tr("Description"), sDesc_);
    auto* help = new QLabel(tr("The contract between Peri and Main for one result: which id identifies it, how many bytes it carries and in which order a multi-byte value "
                               "arrives. With it the assembler checks the publish and consume lengths and warns when a high-byte-first value goes into the ALU unswapped."));
    help->setWordWrap(true);
    f->addRow(help);
    auto cur = [this]() -> asmcore::HwService* { Sel c = current(); return c.kind == Kind::Service && c.index >= 0 ? &hw_.services[c.index] : nullptr; };
    connect(sName_, &QLineEdit::textEdited, this, [this, cur](const QString& t) { if (loading_ || !cur()) return; cur()->name = s(t.trimmed()); edited(); });
    connect(sDesc_, &QLineEdit::textEdited, this, [this, cur](const QString& t) { if (loading_ || !cur()) return; cur()->description = s(t); edited(); });
    connect(sId_, QOverload<int>::of(&QSpinBox::valueChanged), this, [this, cur](int x) { if (loading_ || !cur()) return; cur()->id = x; edited(); });
    connect(sLen_, QOverload<int>::of(&QSpinBox::valueChanged), this, [this, cur](int x) { if (loading_ || !cur()) return; cur()->len = x == 0 ? -1 : x; edited(); });
    connect(sWire_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, cur](int) { if (loading_ || !cur()) return; cur()->wire = sWire_->currentData().toString().at(0).toLatin1(); edited(); });
    return w;
}

QWidget* HardwareDesigner::buildEnv() {
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    auto* f = new QFormLayout;
    v->addLayout(f);
    vKind_ = new QComboBox;
    vKind_->addItems({"sim", "timing", "latency", "bus", "signal", "condition", "inject"});
    vName_ = new QLineEdit;
    vName_->setPlaceholderText(tr("(e.g. a signal name, or 'main 0xE' for a condition)"));
    f->addRow(tr("Section"), vKind_);
    f->addRow(tr("Name"), vName_);
    auto* help = new QLabel(tr("Simulator environment settings (docs/SIM.md): one 'key = value' per line.\n"
                               "sim: tie_order, unmapped_csr, fault_stops    timing: frame_cycles, vblank_start, vblank_len ...    inject: 'at 5000 = nack dev=0 count=1'"));
    help->setWordWrap(true);
    v->addWidget(help);
    vText_ = new QPlainTextEdit;
    vText_->setObjectName("hwEnvText");
    QFont mono("Consolas");
    mono.setStyleHint(QFont::Monospace);
    vText_->setFont(mono);
    v->addWidget(vText_, 1);
    auto cur = [this]() -> asmcore::HwSection* { Sel c = current(); return c.kind == Kind::Env && c.index >= 0 ? &hw_.env[c.index] : nullptr; };
    connect(vKind_, &QComboBox::currentTextChanged, this, [this, cur](const QString& t) { if (loading_ || !cur()) return; cur()->kind = s(t); edited(); });
    connect(vName_, &QLineEdit::textEdited, this, [this, cur](const QString& t) { if (loading_ || !cur()) return; cur()->name = s(t.trimmed()); edited(); });
    connect(vText_, &QPlainTextEdit::textChanged, this, [this, cur] {
        if (loading_ || !cur()) return;
        cur()->kv.clear();
        for (const QString& line : vText_->toPlainText().split('\n')) {
            QString l = line.trimmed();
            if (l.isEmpty() || l.startsWith(';')) continue;
            int eq = l.indexOf('=');
            if (eq < 0) continue;
            cur()->kv.push_back({s(l.left(eq).trimmed().toLower()), s(l.mid(eq + 1).trimmed())});
        }
        edited();
    });
    return w;
}

QWidget* HardwareDesigner::buildRaw() {
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    auto* help = new QLabel(tr("Assembler lines the designer does not model (for example .include). They are written to the generated definitions file as they are, one per line."));
    help->setWordWrap(true);
    v->addWidget(help);
    rText_ = new QPlainTextEdit;
    rText_->setObjectName("hwRaw");
    QFont mono("Consolas");
    mono.setStyleHint(QFont::Monospace);
    rText_->setFont(mono);
    v->addWidget(rText_, 1);
    connect(rText_, &QPlainTextEdit::textChanged, this, [this] {
        if (loading_) return;
        hw_.rawInc.clear();
        for (const QString& line : rText_->toPlainText().split('\n')) if (!line.trimmed().isEmpty()) hw_.rawInc.push_back(s(line.trimmed()));
        edited();
    });
    return w;
}

// ---- the tree --------------------------------------------------------------------------------------------------------------------
namespace {
constexpr int kKindRole = Qt::UserRole, kIndexRole = Qt::UserRole + 1;
}

QString HardwareDesigner::labelFor(Kind k, int i) const {
    switch (k) {
        case Kind::Equ: return q(hw_.equs[i].name) + "  =  " + q(hw_.equs[i].value);
        case Kind::Const: return q(hw_.consts[i].name);
        case Kind::Csr: {
            const asmcore::HwCsr& c = hw_.csrs[i];
            return q(c.name) + "  " + (c.addr >= 0 ? hex(c.addr, 3) : QString("?")) + "  (" + q(c.kind) + ")";
        }
        case Kind::Device: {
            const asmcore::HwDevice& d = hw_.devices[i];
            return q(d.name) + "  " + hex(d.addr, 2) + (d.index >= 0 ? tr("  - DeviceMap %1").arg(d.index) : QString());
        }
        case Kind::Service: return q(hw_.services[i].name) + tr("  id %1").arg(hw_.services[i].id);
        case Kind::Env: return q(hw_.env[i].kind) + (hw_.env[i].name.empty() ? QString() : " " + q(hw_.env[i].name));
        default: return QString();
    }
}

void HardwareDesigner::fillTree(const Sel& keep) {
    const bool was = loading_;
    loading_ = true;
    tree_->clear();
    auto group = [&](const QString& text) {
        auto* it = new QTreeWidgetItem(tree_, QStringList(text));
        it->setData(0, kKindRole, (int)Kind::None);
        QFont f = it->font(0);
        f.setBold(true);
        it->setFont(0, f);
        it->setExpanded(true);
        return it;
    };
    auto leaf = [&](QTreeWidgetItem* parent, Kind k, int i) {
        auto* it = new QTreeWidgetItem(parent, QStringList(labelFor(k, i)));
        it->setData(0, kKindRole, (int)k);
        it->setData(0, kIndexRole, i);
        return it;
    };
    auto* general = new QTreeWidgetItem(tree_, QStringList(tr("Board") + ": " + q(hw_.name)));
    general->setData(0, kKindRole, (int)Kind::General);
    QFont bf = general->font(0);
    bf.setBold(true);
    general->setFont(0, bf);
    QTreeWidgetItem* gc = group(tr("Constants"));
    for (size_t i = 0; i < hw_.equs.size(); i++) leaf(gc, Kind::Equ, (int)i);
    for (size_t i = 0; i < hw_.consts.size(); i++) leaf(gc, Kind::Const, (int)i);
    QTreeWidgetItem* gr = group(tr("CSR registers"));
    for (size_t i = 0; i < hw_.csrs.size(); i++) leaf(gr, Kind::Csr, (int)i);
    QTreeWidgetItem* gd = group(tr("I2C devices"));
    for (size_t i = 0; i < hw_.devices.size(); i++) leaf(gd, Kind::Device, (int)i);
    QTreeWidgetItem* gs = group(tr("Services"));
    for (size_t i = 0; i < hw_.services.size(); i++) leaf(gs, Kind::Service, (int)i);
    QTreeWidgetItem* ge = group(tr("Simulator environment"));
    for (size_t i = 0; i < hw_.env.size(); i++) leaf(ge, Kind::Env, (int)i);
    auto* raw = new QTreeWidgetItem(tree_, QStringList(tr("Assembler lines") + QString(" (%1)").arg(hw_.rawInc.size())));
    raw->setData(0, kKindRole, (int)Kind::Raw);
    loading_ = was;
    // restore the selection
    QTreeWidgetItem* target = general;
    if (keep.kind != Kind::None) {
        QTreeWidgetItemIterator it(tree_);
        while (*it) {
            if ((*it)->data(0, kKindRole).toInt() == (int)keep.kind && (keep.index < 0 || (*it)->data(0, kIndexRole).toInt() == keep.index)) { target = *it; break; }
            ++it;
        }
    }
    tree_->setCurrentItem(target);
    onSelectionChanged();
}

HardwareDesigner::Sel HardwareDesigner::current() const {
    Sel s;
    QTreeWidgetItem* it = tree_->currentItem();
    if (!it) return s;
    s.kind = (Kind)it->data(0, kKindRole).toInt();
    s.index = it->data(0, kIndexRole).isValid() ? it->data(0, kIndexRole).toInt() : -1;
    return s;
}

void HardwareDesigner::onSelectionChanged() {
    Sel c = current();
    removeBtn_->setEnabled(c.kind != Kind::None && c.kind != Kind::General && c.kind != Kind::Raw);
    load(c);
}

void HardwareDesigner::updateCurrentLabel() {
    Sel c = current();
    QTreeWidgetItem* it = tree_->currentItem();
    if (!it) return;
    if (c.kind == Kind::General) it->setText(0, tr("Board") + ": " + q(hw_.name));
    else if (c.kind == Kind::Raw) it->setText(0, tr("Assembler lines") + QString(" (%1)").arg(hw_.rawInc.size()));
    else if (c.index >= 0) it->setText(0, labelFor(c.kind, c.index));
}

// ---- model -> form -----------------------------------------------------------------------------------------------------------------
void HardwareDesigner::load(const Sel& c) {
    loading_ = true;
    switch (c.kind) {
        case Kind::General:
            gName_->setText(q(hw_.name));
            gDesc_->setText(q(hw_.description));
            gInc_->setText(q(hw_.includeFile));
            gEnv_->setText(q(hw_.envFile));
            pages_->setCurrentIndex(pageGeneral_);
            break;
        case Kind::Equ:
            eName_->setText(q(hw_.equs[c.index].name));
            eValue_->setText(q(hw_.equs[c.index].value));
            eDesc_->setText(q(hw_.equs[c.index].description));
            pages_->setCurrentIndex(pageEqu_);
            break;
        case Kind::Const: {
            const asmcore::HwConst& k = hw_.consts[c.index];
            kName_->setText(q(k.name));
            kWidth_->setCurrentIndex(k.width == 2 ? 1 : 0);
            QString v;
            for (size_t i = 0; i < k.values.size(); i++) v += (i ? ", " : "") + q(k.values[i]);
            kValues_->setText(v);
            kDesc_->setText(q(k.description));
            pages_->setCurrentIndex(pageConst_);
            break;
        }
        case Kind::Csr: {
            const asmcore::HwCsr& k = hw_.csrs[c.index];
            cName_->setText(q(k.name));
            cAddr_->setText(k.addr >= 0 ? hex(k.addr, 3) : QString());
            cKind_->setCurrentIndex(std::max(0, cKind_->findData(q(k.kind))));
            cDesc_->setText(q(k.description));
            cSim_->setChecked(k.simulated);
            cReset_->setValue(k.reset);
            cBind_->setEditText(q(k.bind));
            cBytes_->setValue(k.bytes);
            cConst_->setText(hex(k.constVal, 2));
            const bool action = k.kind == "action", status = k.kind == "status";
            cForm_->setRowVisible(cResetRow_, k.kind == "ram");
            cForm_->setRowVisible(cBindRow_, k.kind == "value");
            cForm_->setRowVisible(cBytesRow_, k.kind == "value");
            cForm_->setRowVisible(cConstRow_, k.kind == "const");
            cBitsHelp_->setText(action ? tr("Bits: the action the simulator performs when software writes 1 to the bit (e.g. timer_start, poll_arm, done_set; set:SIGNAL / clr:SIGNAL for any signal). "
                                            "The assembler role makes the flow checks (W515, W516) know what the bit means.")
                                       : status ? tr("Bits: an expression; the bit reads as 1 when it is true (e.g. poll.ready). The assembler role is not used for read-only bits.")
                                                : tr("Bits: only action and status registers give bits a meaning in the simulator. The assembler role (timer_start, timer_clear, poll_arm, poll_clear) "
                                                     "still tells the flow checks what a bit means."));
            for (int b = 0; b < 8; b++) {
                auto* act = qobject_cast<QComboBox*>(cBits_->cellWidget(b, 1));
                auto* role = qobject_cast<QComboBox*>(cBits_->cellWidget(b, 2));
                act->setEditText(q(k.bit[b]));
                act->setEnabled(action || status);
                role->setCurrentIndex(std::max(0, role->findData(q(k.role[b]))));
            }
            pages_->setCurrentIndex(pageCsr_);
            break;
        }
        case Kind::Device: {
            const asmcore::HwDevice& d = hw_.devices[c.index];
            dName_->setText(q(d.name));
            dSymbol_->setText(q(d.symbol));
            dAddr_->setText(hex(d.addr, 2));
            dIndex_->setCurrentIndex(std::max(0, dIndex_->findData(d.index)));
            dKind_->setCurrentText(q(d.kind));
            dSim_->setChecked(d.simulated);
            dDesc_->setText(q(d.description));
            dRegs_->blockSignals(true);
            dRegs_->setRowCount(0);
            for (auto& kv : d.keys) {
                const QString key = q(kv.first);
                const int sp = key.indexOf(' ');
                const QString type = sp < 0 ? key : key.left(sp);
                addRowFn_(type, sp < 0 ? QString() : key.mid(sp + 1), q(kv.second));
            }
            dRegs_->blockSignals(false);
            pages_->setCurrentIndex(pageDevice_);
            break;
        }
        case Kind::Service: {
            const asmcore::HwService& sv = hw_.services[c.index];
            sName_->setText(q(sv.name));
            sId_->setValue(sv.id);
            sLen_->setValue(sv.len < 0 ? 0 : sv.len);
            sWire_->setCurrentIndex(sv.wire == 'B' ? 1 : sv.wire == 'L' ? 2 : 0);
            sDesc_->setText(q(sv.description));
            pages_->setCurrentIndex(pageService_);
            break;
        }
        case Kind::Env: {
            const asmcore::HwSection& e = hw_.env[c.index];
            vKind_->setCurrentText(q(e.kind));
            vName_->setText(q(e.name));
            QString t;
            for (auto& kv : e.kv) t += q(kv.first) + " = " + q(kv.second) + "\n";
            vText_->setPlainText(t);
            pages_->setCurrentIndex(pageEnv_);
            break;
        }
        case Kind::Raw: {
            QString t;
            for (auto& l : hw_.rawInc) t += q(l) + "\n";
            rText_->setPlainText(t);
            pages_->setCurrentIndex(pageRaw_);
            break;
        }
        default: pages_->setCurrentIndex(pageGroup_); break;
    }
    loading_ = false;
}

// ---- form -> model, validation, generated files ----------------------------------------------------------------------------------------
void HardwareDesigner::edited() {
    if (loading_) return;
    updateCurrentLabel();
    timer_.start();
}

void HardwareDesigner::flush() {
    if (timer_.isActive()) { timer_.stop(); refresh(); }
}

void HardwareDesigner::refresh() {
    if (!loaded_) return;
    problems_ = asmcore::validateHardware(hw_);
    bool anyError = false;
    for (auto& p : problems_) anyError |= p.error;
    if (!anyError) for (auto& p : asmcore::checkHardwareWithTools(hw_)) problems_.push_back(p);
    int errors = 0, warnings = 0;
    problemsTable_->setRowCount((int)problems_.size());
    for (size_t i = 0; i < problems_.size(); i++) {
        const asmcore::HwProblem& p = problems_[i];
        (p.error ? errors : warnings)++;
        auto* k = new QTableWidgetItem(p.error ? tr("Error") : tr("Warning"));
        k->setForeground(p.error ? QColor("#b00020") : QColor("#9a6700"));
        problemsTable_->setItem((int)i, 0, k);
        problemsTable_->setItem((int)i, 1, new QTableWidgetItem(q(p.where)));
        problemsTable_->setItem((int)i, 2, new QTableWidgetItem(q(p.message)));
    }
    problemsTable_->resizeColumnToContents(0);
    problemsTable_->resizeColumnToContents(1);
    summary_->setText(errors ? tr("%1 error(s), %2 warning(s): the generated files may not work until the errors are fixed").arg(errors).arg(warnings)
                             : warnings ? tr("No errors, %1 warning(s)").arg(warnings) : tr("No problems"));
    bottom_->setTabText(0, errors || warnings ? tr("Problems (%1)").arg(errors + warnings) : tr("Problems"));
    const std::string src = QFileInfo(q(path_)).fileName().toStdString();
    incView_->setPlainText(q(asmcore::generateInc(hw_, src)));
    envView_->setPlainText(q(asmcore::generateEnv(hw_, src)));
    applying_ = true;
    applyHardware(ctx_.workspace, path_, hw_);
    applying_ = false;
}

void HardwareDesigner::reloadFromWorkspace() {
    std::vector<std::string> errs;
    asmcore::Hardware fresh;
    if (!loadHardware(ctx_.workspace, path_, fresh, errs)) {
        banner_->setText(tr("This hardware description cannot be read:") + "\n" + q(errs.empty() ? std::string() : errs[0]));
        banner_->show();
        return;
    }
    banner_->hide();
    loaded_ = true;
    hw_ = fresh;
    fillTree(current());
    refresh();
}

// ---- adding and removing ---------------------------------------------------------------------------------------------------------------
QString HardwareDesigner::uniqueName(const QString& base) const {
    auto used = [&](const QString& n) {
        const std::string x = n.toStdString();
        for (auto& e : hw_.equs) if (e.name == x) return true;
        for (auto& e : hw_.consts) if (e.name == x) return true;
        for (auto& e : hw_.csrs) if (e.name == x) return true;
        for (auto& e : hw_.devices) if (e.name == x || e.symbol == x) return true;
        for (auto& e : hw_.services) if (e.name == x) return true;
        return false;
    };
    if (!used(base)) return base;
    for (int i = 2;; i++) if (!used(base + "_" + QString::number(i))) return base + "_" + QString::number(i);
}

QString HardwareDesigner::addComponent(const QString& kind) {
    if (!loaded_) return QString();
    Sel select;
    QString name;
    if (kind == "csr") {
        asmcore::HwCsr c;
        name = uniqueName("NEW_CSR");
        c.name = s(name);
        int addr = 0x010;
        for (bool again = true; again;) {
            again = false;
            for (auto& o : hw_.csrs) if (o.addr == addr || (o.kind == "value" && addr >= o.addr && addr < o.addr + o.bytes)) { addr++; again = true; }
        }
        c.addr = addr;
        hw_.csrs.push_back(c);
        select = {Kind::Csr, (int)hw_.csrs.size() - 1};
    } else if (kind == "device") {
        asmcore::HwDevice d;
        name = uniqueName("new_device");
        d.name = s(name);
        int addr = 0x50;
        for (bool again = true; again;) { again = false; for (auto& o : hw_.devices) if (o.addr == addr) { addr++; again = true; } }
        d.addr = addr;
        std::vector<bool> used(8, false);
        for (auto& o : hw_.devices) if (o.index >= 0 && o.index < 8) used[o.index] = true;
        for (int i = 0; i < 8; i++) if (!used[i]) { d.index = i; break; }
        d.symbol = "DEV_" + QString::fromStdString(d.name).toUpper().toStdString();
        hw_.devices.push_back(d);
        select = {Kind::Device, (int)hw_.devices.size() - 1};
    } else if (kind == "service") {
        asmcore::HwService sv;
        name = uniqueName("NEW_SERVICE");
        sv.name = s(name);
        std::vector<bool> used(16, false);
        for (auto& o : hw_.services) if (o.id >= 0 && o.id < 16) used[o.id] = true;
        for (int i = 1; i < 16; i++) if (!used[i]) { sv.id = i; break; }
        hw_.services.push_back(sv);
        select = {Kind::Service, (int)hw_.services.size() - 1};
    } else if (kind == "const") {
        asmcore::HwConst k;
        name = uniqueName("NEW_CONST");
        k.name = s(name);
        k.values = {"0"};
        hw_.consts.push_back(k);
        select = {Kind::Const, (int)hw_.consts.size() - 1};
    } else if (kind == "equ") {
        asmcore::HwEqu e;
        name = uniqueName("NEW_VALUE");
        e.name = s(name);
        e.value = "0";
        hw_.equs.push_back(e);
        select = {Kind::Equ, (int)hw_.equs.size() - 1};
    } else if (kind == "env") {
        asmcore::HwSection e;
        e.kind = "sim";
        hw_.env.push_back(e);
        name = "sim";
        select = {Kind::Env, (int)hw_.env.size() - 1};
    } else {
        return QString();
    }
    fillTree(select);
    refresh();
    return name;
}

void HardwareDesigner::removeCurrent() {
    Sel c = current();
    if (c.index < 0) return;
    auto erase = [&](auto& v) { if (c.index < (int)v.size()) v.erase(v.begin() + c.index); };
    switch (c.kind) {
        case Kind::Equ: erase(hw_.equs); break;
        case Kind::Const: erase(hw_.consts); break;
        case Kind::Csr: erase(hw_.csrs); break;
        case Kind::Device: erase(hw_.devices); break;
        case Kind::Service: erase(hw_.services); break;
        case Kind::Env: erase(hw_.env); break;
        default: return;
    }
    fillTree(Sel());
    refresh();
}

bool HardwareDesigner::select(const QString& kind, const QString& name) {
    Kind k = Kind::None;
    int idx = -1;
    const std::string n = name.toStdString();
    auto find = [&](auto& v) { for (size_t i = 0; i < v.size(); i++) if (v[i].name == n) return (int)i; return -1; };
    if (kind == "csr") { k = Kind::Csr; idx = find(hw_.csrs); }
    else if (kind == "device") { k = Kind::Device; idx = find(hw_.devices); }
    else if (kind == "service") { k = Kind::Service; idx = find(hw_.services); }
    else if (kind == "const") { k = Kind::Const; idx = find(hw_.consts); }
    else if (kind == "equ") { k = Kind::Equ; idx = find(hw_.equs); }
    if (idx < 0) return false;
    QTreeWidgetItemIterator it(tree_);
    while (*it) {
        if ((*it)->data(0, kKindRole).toInt() == (int)k && (*it)->data(0, kIndexRole).toInt() == idx) { tree_->setCurrentItem(*it); return true; }
        ++it;
    }
    return false;
}

void HardwareDesigner::showProblemTarget(int row) {
    if (row < 0 || row >= (int)problems_.size()) return;
    const QString where = q(problems_[row].where);
    const int sp = where.indexOf(' ');
    if (sp < 0) return;
    QString name = where.mid(sp + 1);
    const int paren = name.indexOf(" (");
    if (paren >= 0) name = name.left(paren);
    const int bit = name.indexOf(" bit ");
    if (bit >= 0) name = name.left(bit);
    select(where.left(sp), name);
}

}  // namespace acide
