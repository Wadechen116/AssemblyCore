#include "block_editor.h"

#include <functional>
#include <set>

#include <QButtonGroup>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QRadioButton>
#include <QShortcut>
#include <QStackedWidget>
#include <QToolButton>
#include <QToolTip>
#include <QMenu>
#include <QMimeData>
#include <QClipboard>
#include <QApplication>
#include <QVBoxLayout>

#include "blocks/block_tidy.h"
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
    QToolButton* ztidy = button(tr("Tidy"), tr("Tidy up: give every stack a place so that nothing overlaps (the program does not change)"), "tidy");
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
    {
        blocks::LayoutStyle style;
        style.showEmptyOptional = true;                      // an editor shows the optional parts that are still empty (a shift amount, a flag): they are there to be filled in
        scene_->setLayoutStyle(style);
    }
    scene_->setAnimationsEnabled(ctx.settings.get("blocks.animate", "1") != "0");      // the soft, bouncy effects (blocks.animate = 0 switches them off)
    view_->setTheme(scene_->theme());
    fieldEditor_ = new FieldEditor(this);                  // (like the comment editor: a child of the editor, so the view's shortcuts do not take its keys)
    commentEdit_ = new CommentEdit(this);                  // a child of the editor, not of the view: the view's Esc / Delete shortcuts must not reach it
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
    connect(ztidy, &QToolButton::clicked, this, [this] { tidy(); });
    connect(view_, &BlockView::zoomChanged, this, [this](double z) { zoomLabel_->setText(QString::number(qRound(z * 100)) + "%"); });
    connect(code_, &GeneratedCodePane::lineClicked, this, [this](int line) {
        if (syncing_) return;
        const std::string id = session_.blockAtLine(line);
        syncing_ = true;
        if (id.empty()) session_.clearSelection(); else session_.select({id});
        syncing_ = false;
    });

    conns_->add(scene_->contextRequested, [this](const QPointF& p, const QPoint& g, const std::string& id) { showContextMenu(p, g, id); });
    shortcut(QKeySequence("Ctrl+E"), [this] { session_.toggleDisabled(); });
    conns_->add(scene_->fieldEdit, [this](const BlockScene::EditTarget& t) { beginFieldEdit(t); });
    connect(fieldEditor_, &FieldEditor::committed, this, [this](const QString& text, int navigate) {
        const BlockScene::EditTarget t = fieldEditing_;
        const std::string value = text.trimmed().toUtf8().toStdString();
        if (value != t.text) {
            session_.beginGroup("edit:" + std::to_string(++editSerial_));                       // each kept edit is its own undo step
            const asmcore::blocks::Outcome o = session_.apply([&](asmcore::blocks::Editor& ed) { return ed.setPart(t.blockId, t.part, value); });
            session_.endGroup();
            if (!o.ok) QToolTip::showText(QCursor::pos(), QString::fromUtf8(o.error.c_str()), this);
            else scene_->popEffect(t.blockId);
        }
        view_->setFocus();
        if (navigate != 0) {                                                                   // Tab / Shift+Tab: the next / previous part
            const std::vector<BlockScene::EditTarget> all = scene_->editTargets();
            for (size_t i = 0; i < all.size(); i++)
                if (all[i].blockId == t.blockId && all[i].part == t.part) {
                    const size_t n = all.size();
                    for (size_t step = 1; step <= n; step++) {
                        const BlockScene::EditTarget& next = all[(i + (navigate > 0 ? step : n - step)) % n];
                        if (!next.flag) { beginFieldEdit(next); return; }
                    }
                }
        }
    });
    connect(fieldEditor_, &FieldEditor::cancelled, this, [this] { view_->setFocus(); });
    conns_->add(scene_->commentEdit, [this](const BlockScene::CommentTarget& t) { beginCommentEdit(t); });
    connect(commentEdit_, &CommentEdit::committed, this, [this](const QString& text) {
        std::string t = text.toUtf8().toStdString();
        while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ' || t.back() == '\t')) t.pop_back();       // trailing blank lines and spaces are not a comment
        session_.beginGroup("comment:" + std::to_string(++commentSerial_));              // each kept comment is its own undo step (typing in the editor is not an edit yet)
        struct EndGroup { BlockSession& s; ~EndGroup() { s.endGroup(); } } endGroup{session_};
        if (editing_.section) {
            session_.apply([&](asmcore::blocks::Editor& ed) { return ed.setSectionComment(editing_.stack, t); });
        } else if (!editing_.blockId.empty() && session_.document().find(editing_.blockId)) {
            const bool changed = t != editing_.text;
            session_.apply([&](asmcore::blocks::Editor& ed) { return ed.setComment(editing_.blockId, t); });
            if (changed) scene_->popEffect(editing_.blockId);                                  // the block settles with a little wobble
        }
        view_->setFocus();
    });
    connect(commentEdit_, &CommentEdit::cancelled, this, [this] { view_->setFocus(); });
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

void BlockEditor::tidy() {
    if (blocks::tidyUp(session_, scene_->layout()) > 0) view_->fitAll();
}

namespace {
class SystemClipboard : public IBlockClipboard {
public:
    void set(const std::string& text, const std::string& blocks) override {
        auto* mime = new QMimeData;
        mime->setText(QString::fromUtf8(text.c_str()));                                       // for a text editor
        mime->setData("application/x-acblk", QByteArray(blocks.c_str()));                    // for another block editor: the blocks themselves
        QApplication::clipboard()->setMimeData(mime);
    }
    void get(std::string& text, std::string& blocks) override {
        const QMimeData* mime = QApplication::clipboard()->mimeData();
        if (!mime) return;
        if (mime->hasFormat("application/x-acblk")) blocks = mime->data("application/x-acblk").toStdString();
        text = mime->text().toUtf8().toStdString();
    }
};
}  // namespace

void BlockEditor::copy() {
    if (session_.selection().empty()) return;
    const BlockSession::Clip clip = session_.copySelection();
    if (!clipboard_) clipboard_ = std::make_shared<SystemClipboard>();
    clipboard_->set(clip.text, clip.blocks);
    ctx_.status.fire(tr("Copied %n block(s)", nullptr, (int)session_.selection().size()));
}

void BlockEditor::cut() {
    if (session_.selection().empty()) return;
    BlockSession::Clip clip;
    session_.cutSelection(clip);
    if (!clipboard_) clipboard_ = std::make_shared<SystemClipboard>();
    clipboard_->set(clip.text, clip.blocks);
}

void BlockEditor::paste() {
    if (!clipboard_) clipboard_ = std::make_shared<SystemClipboard>();
    std::string text, blocks;
    clipboard_->get(text, blocks);
    const std::string use = blocks.empty() ? text : blocks;                                  // our own blocks first, else whatever text there is
    const QPointF centre = view_->mapToScene(view_->viewport()->rect().center());
    std::string note;
    const size_t n = session_.paste(use, centre.x() - 60, centre.y() - 20, note);
    if (n == 0) { ctx_.status.fire(note.empty() ? tr("Nothing to paste") : QString::fromUtf8(note.c_str())); return; }
    if (!note.empty()) ctx_.status.fire(QString::fromUtf8(note.c_str()));
    for (const std::string& id : session_.selection()) scene_->popEffect(id);
}

void BlockEditor::showContextMenu(const QPointF& scenePos, const QPoint& globalPos, const std::string& blockId) {
    if (fieldEditor_->isOpen() || commentEdit_->isOpen()) return;
    if (!blockId.empty() && !session_.isSelected(blockId)) session_.select({blockId});
    QMenu menu(this);
    menu.setObjectName("blockContextMenu");
    const bool any = !session_.selection().empty();
    auto add = [&](const QString& text, std::function<void()> fn, bool enabled = true, const QKeySequence& key = QKeySequence()) {
        QAction* a = menu.addAction(text);
        a->setEnabled(enabled);
        if (!key.isEmpty()) a->setShortcut(key);
        a->setShortcutVisibleInContextMenu(true);
        connect(a, &QAction::triggered, this, [fn] { fn(); });
        return a;
    };
    if (!blockId.empty()) {
        BlockScene::EditTarget et;
        if (scene_->editTargetAt(scenePos, et) && !et.flag) add(tr("Edit..."), [this, et] { beginFieldEdit(et); });
        BlockScene::CommentTarget ct;
        if (scene_->commentTargetOf(blockId, ct)) add(ct.text.empty() ? tr("Add a comment...") : tr("Edit the comment..."), [this, ct] { beginCommentEdit(ct); });
        menu.addSeparator();
    }
    add(tr("Cu&t"), [this] { cut(); }, any, QKeySequence::Cut);
    add(tr("&Copy"), [this] { copy(); }, any, QKeySequence::Copy);
    add(tr("&Paste"), [this] { paste(); }, true, QKeySequence::Paste);
    add(tr("&Duplicate"), [this] { session_.duplicateSelection(); }, any, QKeySequence("Ctrl+D"));
    menu.addSeparator();
    bool anyInstruction = false, anyContainer = false, allOff = true;
    for (const std::string& id : session_.selection()) {
        const asmcore::blocks::Block* b = session_.document().find(id);
        const asmcore::blocks::BlockDef* d = b ? session_.registry().find(b->type) : nullptr;
        if (!d) continue;
        if (d->valueType.empty()) { anyInstruction = true; if (!b->disabled) allOff = false; }
        if (!d->stacks.empty()) anyContainer = true;
    }
    add(allOff && anyInstruction ? tr("&Enable") : tr("Disa&ble"), [this] { session_.toggleDisabled(); }, anyInstruction, QKeySequence("Ctrl+E"));
    add(tr("&Fold / unfold"), [this] { session_.toggleCollapsed(); }, anyContainer);
    menu.addSeparator();
    add(tr("&Delete"), [this] { session_.deleteSelection(); }, any, QKeySequence::Delete);
    if (blockId.empty()) {
        menu.addSeparator();
        add(tr("Select &all"), [this] { scene_->selectAll(); }, true, QKeySequence::SelectAll);
        add(tr("&Tidy up"), [this] { tidy(); });
        add(tr("Show everything"), [this] { view_->fitAll(); });
    }
    menu.exec(globalPos);
}

// The names that fit a part: the choices of its field, else the symbols of the kind its socket takes (CSRs, services, devices, numbers, labels, procedures).
void BlockEditor::beginFieldEdit(const BlockScene::EditTarget& t) {
    if (fieldEditor_->isOpen() || commentEdit_->isOpen()) return;
    fieldEditing_ = t;
    QStringList items;
    std::set<std::string> seen;
    auto add = [&](const std::string& s) { if (!s.empty() && seen.insert(s).second) items << QString::fromUtf8(s.c_str()); };
    const asmcore::blocks::SymbolTable& syms = session_.symbols();
    if (!t.options.empty()) for (const std::string& o : t.options) add(o);
    for (const std::string& a : t.accepts) {
        if (a == "label.flow" || a == "label.proc" || a == "csr" || a == "service" || a == "dev" || a == "int")
            for (const asmcore::blocks::Symbol* s : syms.ofKind(a)) add(s->name);
    }
    items.sort(Qt::CaseInsensitive);
    // the hint for the value: the range of a number, the form of an operand
    const asmcore::blocks::Block* b = session_.document().find(t.blockId);
    const asmcore::blocks::BlockDef* def = b ? session_.registry().find(b->type) : nullptr;
    const asmcore::blocks::FieldDef* fd = nullptr;
    if (def) for (const asmcore::blocks::FieldDef& f : def->fields) if (f.name == t.part) fd = &f;
    auto hint = [def, fd](const QString& text) -> QString {
        if (!def || !fd) return QString();
        return QString::fromUtf8(asmcore::blocks::checkFieldValue(*def, *fd, text.trimmed().toUtf8().toStdString()).c_str());
    };
    QRect r = view_->mapFromScene(t.rect).boundingRect();
    r.moveTopLeft(view_->viewport()->mapTo(this, r.topLeft()));
    QFont f = font();
    f.setPointSizeF(std::max(7.0, f.pointSizeF() * view_->zoom()));
    const blocks::BlockTheme& th = scene_->theme();
    const bool closed = t.kind == asmcore::blocks::FieldKind::Choice && !t.open && !t.options.empty();
    view_->showBlock(t.rect);
    fieldEditor_->open(r, QString::fromUtf8(t.text.c_str()), items, !closed, f, toQColor(th.canvas).lighter(125), toQColor(th.chipText), toQColor(th.selection), hint);
}

void BlockEditor::beginCommentEdit(const BlockScene::CommentTarget& t) {
    if (commentEdit_->isOpen()) return;
    editing_ = t;
    QRect r = view_->mapFromScene(t.rect).boundingRect();
    r.moveTopLeft(view_->viewport()->mapTo(this, r.topLeft()));
    QFont f = font();
    f.setPointSizeF(std::max(7.0, f.pointSizeF() * view_->zoom()));
    const blocks::BlockTheme& th = scene_->theme();
    commentEdit_->open(QRect(r.x(), r.y(), std::max(r.width(), (int)(190 * view_->zoom())), r.height()), QString::fromUtf8(t.text.c_str()), f, toQColor(th.canvas).lighter(125), toQColor(th.chipText), toQColor(th.selection));
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
