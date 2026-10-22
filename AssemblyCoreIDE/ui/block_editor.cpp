#include "block_editor.h"

#include <functional>

#include <QButtonGroup>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QRadioButton>
#include <QShortcut>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include "hardware_link.h"

namespace acide {

using namespace blocks;

static blocks::BlockTheme blockThemeFor(const Settings& s, const Theme& t) {
    const bool dark = t.background.lightness() < 128;
    return BlockTheme::fromSettings(s, dark);
}

BlockEditor::BlockEditor(AppContext& ctx, const std::string& path, QWidget* parent) : QWidget(parent), ctx_(ctx), path_(path) {
    setObjectName("BlockEditor");
    const Theme theme = Theme::fromSettings(ctx.settings);
    QFont mono = editorFont(ctx.settings);
    mono.setPointSize(std::max(8, mono.pointSize() - 2));
    QFont ui = font();
    ui.setPointSize(std::max(8, ui.pointSize()));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // the switch between the two pages
    auto* bar = new QWidget(this);
    auto* bl = new QHBoxLayout(bar);
    bl->setContentsMargins(8, 4, 8, 4);
    blocksRadio_ = new QRadioButton(tr("Blocks"), bar);
    codeRadio_ = new QRadioButton(tr("Assembly code"), bar);
    blocksRadio_->setObjectName("pageBlocks");
    codeRadio_->setObjectName("pageCode");
    blocksRadio_->setToolTip(tr("Show the program as blocks"));
    codeRadio_->setToolTip(tr("Show the assembler text the blocks generate"));
    blocksRadio_->setChecked(true);
    auto* grp = new QButtonGroup(bar);
    grp->addButton(blocksRadio_);
    grp->addButton(codeRadio_);
    bl->addWidget(blocksRadio_);
    bl->addWidget(codeRadio_);
    bl->addSpacing(16);
    info_ = new QLabel(bar);
    info_->setObjectName("blockInfo");
    bl->addWidget(info_, 1);
    zoomBar_ = new QWidget(bar);
    auto* zl = new QHBoxLayout(zoomBar_);
    zl->setContentsMargins(0, 0, 0, 0);
    zl->setSpacing(2);
    auto button = [&](const QString& text, const QString& tip, const char* name) {
        auto* b = new QToolButton(zoomBar_);
        b->setText(text);
        b->setToolTip(tip);
        b->setObjectName(name);
        b->setAutoRaise(true);
        zl->addWidget(b);
        return b;
    };
    QToolButton* zout = button("\xE2\x88\x92", tr("Zoom out (Ctrl+-)"), "zoomOut");
    zoomLabel_ = new QLabel("100%", zoomBar_);
    zoomLabel_->setMinimumWidth(40);
    zoomLabel_->setAlignment(Qt::AlignCenter);
    zl->addWidget(zoomLabel_);
    QToolButton* zin = button("+", tr("Zoom in (Ctrl++)"), "zoomIn");
    QToolButton* z100 = button("1:1", tr("Actual size (Ctrl+0)"), "zoomReset");
    QToolButton* zfit = button(tr("Fit"), tr("Show the whole program"), "zoomFit");
    bl->addWidget(zoomBar_);
    root->addWidget(bar);

    banner_ = new QLabel(this);
    banner_->setObjectName("blockBanner");
    banner_->setWordWrap(true);
    banner_->setStyleSheet("background:#fdecea;color:#b3261e;padding:6px 10px;");
    banner_->hide();
    root->addWidget(banner_);

    pages_ = new QStackedWidget(this);
    scene_ = new BlockScene(session_, blockThemeFor(ctx.settings, theme), ui, mono, this);
    view_ = new BlockView(scene_, this);
    view_->setObjectName("blockView");
    view_->setTheme(scene_->theme());
    code_ = new GeneratedCodePane(ctx.language, theme, mono, this);
    pages_->addWidget(view_);
    pages_->addWidget(code_);
    root->addWidget(pages_, 1);

    auto* selectAll = new QShortcut(QKeySequence::SelectAll, view_);
    selectAll->setContext(Qt::WidgetWithChildrenShortcut);
    connect(selectAll, &QShortcut::activated, this, [this] { scene_->selectAll(); });
    auto* escape = new QShortcut(QKeySequence(Qt::Key_Escape), view_);
    escape->setContext(Qt::WidgetWithChildrenShortcut);
    connect(escape, &QShortcut::activated, this, [this] { if (scene_->dragging()) scene_->cancelDrag(); else session_.clearSelection(); });
    auto shortcut = [&](const QKeySequence& k, std::function<void()> fn) {
        auto* sc = new QShortcut(k, view_);
        sc->setContext(Qt::WidgetWithChildrenShortcut);
        connect(sc, &QShortcut::activated, this, [fn] { fn(); });
    };
    shortcut(QKeySequence::Delete, [this] { if (!scene_->dragging()) session_.deleteSelection(); });
    connect(blocksRadio_, &QRadioButton::toggled, this, [this](bool on) { if (on) setPage(Page::Blocks); });
    connect(codeRadio_, &QRadioButton::toggled, this, [this](bool on) { if (on) setPage(Page::Code); });
    connect(zout, &QToolButton::clicked, view_, &BlockView::zoomOut);
    connect(zin, &QToolButton::clicked, view_, &BlockView::zoomIn);
    connect(z100, &QToolButton::clicked, view_, &BlockView::resetZoom);
    connect(zfit, &QToolButton::clicked, view_, &BlockView::fitAll);
    connect(view_, &BlockView::zoomChanged, this, [this](double z) { zoomLabel_->setText(QString::number(qRound(z * 100)) + "%"); });
    connect(code_, &GeneratedCodePane::lineClicked, this, [this](int line) {
        if (syncing_) return;
        const std::string id = session_.blockAtLine(line);
        syncing_ = true;
        if (id.empty()) session_.clearSelection(); else session_.select({id});
        syncing_ = false;
    });

    conns_->add(scene_->blockClicked, [this](const std::string& id, Qt::KeyboardModifiers m) { onBlockClicked(id, m); });
    conns_->add(scene_->emptyClicked, [this](Qt::KeyboardModifiers m) { if (!(m & Qt::ControlModifier)) session_.clearSelection(); });
    conns_->add(session_.changed, [this] { refreshCode(); refreshInfo(); });
    conns_->add(session_.selectionChanged, [this] {
        if (!syncing_) code_->setMarkedLines(session_.selectedLines(), page() == Page::Code);
        refreshInfo();
    });
    conns_->add(ctx.workspace.documentChanged, [this](const std::string& p) {
        if (p == path_) reload();
        else if (!symbolsFrom_.empty() && p.compare(0, symbolsFrom_.size() - 5, symbolsFrom_, 0, symbolsFrom_.size() - 5) == 0) refreshSymbols();     // the description or the include generated from it
    });

    conns_->add(ctx.workspace.projectChanged, [this] { refreshSymbols(); });                    // a hardware description was attached / changed
    reload();
}

// The scene listens to the session, which is a member of this class and so dies before the (child) scene would: take the views down first.
BlockEditor::~BlockEditor() {
    conns_.reset();
    delete view_;
    delete scene_;
    view_ = nullptr;
    scene_ = nullptr;
}

QString BlockEditor::tabTitle() const { return tr("Blocks: %1").arg(QFileInfo(QString::fromStdString(path_)).fileName()); }

BlockEditor::Page BlockEditor::page() const { return pages_->currentWidget() == code_ ? Page::Code : Page::Blocks; }

void BlockEditor::setPage(Page p) {
    pages_->setCurrentWidget(p == Page::Code ? static_cast<QWidget*>(code_) : static_cast<QWidget*>(view_));
    blocksRadio_->setChecked(p == Page::Blocks);
    codeRadio_->setChecked(p == Page::Code);
    zoomBar_->setVisible(p == Page::Blocks);
    if (p == Page::Code) code_->setMarkedLines(session_.selectedLines(), true);
    else if (!session_.selection().empty()) view_->showBlock(scene_->blockRect(*session_.selection().begin()));
}

QString BlockEditor::errorText() const { return banner_->isHidden() ? QString() : banner_->text(); }
QString BlockEditor::infoText() const { return info_->text(); }

void BlockEditor::reload() {
    Document* doc = ctx_.workspace.find(path_);
    if (!doc) return;
    if (doc->text == lastText_ && !lastText_.empty()) return;          // our own change coming back
    lastText_ = doc->text;
    std::string err;
    if (!session_.load(doc->text, err)) {
        banner_->setText(tr("This file cannot be shown as blocks: %1").arg(QString::fromStdString(err)));
        banner_->show();
        info_->clear();
        return;
    }
    banner_->hide();
    refreshSymbols();
    view_->showContent();
    refreshCode();
    refreshInfo();
}

// The program's hardware description (the one the file names, else the project's) defines names; they are known to the checks.
void BlockEditor::refreshSymbols() {
    std::string achw = session_.document().hardware;
    if (!achw.empty() && achw[0] != '/' && !(achw.size() > 1 && achw[1] == ':')) {
        const size_t slash = path_.find_last_of('/');
        achw = (slash == std::string::npos ? std::string() : path_.substr(0, slash + 1)) + achw;
    }
    if (achw.empty() && ctx_.workspace.hasProject() && !ctx_.workspace.project().hardware.empty()) achw = ctx_.workspace.project().absolute(ctx_.workspace.project().hardware);
    if (achw.empty() || achw.size() < 5) { symbolsFrom_.clear(); return; }
    symbolsFrom_ = achw;
    asmcore::blocks::SymbolTable table;
    if (hardwareSymbols(ctx_.workspace, achw, table)) session_.setExternalSymbols(table);
}

void BlockEditor::refreshCode() { code_->setCode(QString::fromUtf8(session_.generated().text.c_str())); }

void BlockEditor::refreshInfo() {
    const BlockSession::Stats s = session_.stats();
    QString t = tr("%n block(s)", nullptr, (int)s.blocks);
    if (s.floatingStacks) t += tr(", %n floating stack(s)", nullptr, (int)s.floatingStacks);
    if (s.rawBlocks) t += tr(", %n text line(s)", nullptr, (int)s.rawBlocks);
    if (s.problems) t += tr(", %n problem(s)", nullptr, (int)s.problems);
    if (!session_.selection().empty()) t += tr(" \xC2\xB7 %n selected", nullptr, (int)session_.selection().size());
    info_->setText(t);
}

void BlockEditor::onBlockClicked(const std::string& id, Qt::KeyboardModifiers m) {
    if (m & Qt::ControlModifier) session_.toggle(id);
    else session_.select({id});
}

void BlockEditor::selectBlock(const std::string& id, bool reveal) {
    session_.select({id});
    if (reveal) view_->showBlock(scene_->blockRect(id));
}

}  // namespace acide
