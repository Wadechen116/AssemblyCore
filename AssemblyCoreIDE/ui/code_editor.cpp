#include "code_editor.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QContextMenuEvent>
#include <QHelpEvent>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QRegularExpression>
#include <QScrollBar>
#include <QTextBlock>
#include <QToolTip>

#include "editing.h"
#include "text_util.h"

namespace acide {

class LineNumberArea : public QWidget {
public:
    explicit LineNumberArea(CodeEditor* e) : QWidget(e), editor_(e) {}
    QSize sizeHint() const override { return QSize(editor_->lineNumberAreaWidth(), 0); }

protected:
    void paintEvent(QPaintEvent* e) override { editor_->lineNumberAreaPaint(e); }
    void mousePressEvent(QMouseEvent* e) override { editor_->gutterClicked(e->pos().y()); }

private:
    CodeEditor* editor_;
};

using asmcore::lang::Dom;

CodeEditor::CodeEditor(AppContext& ctx, const std::string& path, QWidget* parent)
    : QPlainTextEdit(parent), ctx_(ctx), path_(path), theme_(Theme::fromSettings(ctx.settings)) {
    setLineWrapMode(NoWrap);
    setFont(editorFont(ctx.settings));
    setTabStopDistance(fontMetrics().horizontalAdvance(' ') * 4);
    {
        std::string ext = path.substr(path.find_last_of('.') == std::string::npos ? path.size() : path.find_last_of('.') + 1);
        for (char& ch : ext) ch = (char)std::tolower((unsigned char)ch);
        plain_ = !(ext == "asm" || ext == "inc" || ext == "s");
    }
    hl_ = new SyntaxHighlighter(plain_ ? nullptr : document(), ctx.language, theme_);
    gutter_ = new LineNumberArea(this);
    model_ = new QStandardItemModel(this);
    completer_ = new QCompleter(model_, this);
    completer_->setWidget(this);
    completer_->setCompletionMode(QCompleter::PopupCompletion);
    completer_->setCaseSensitivity(Qt::CaseInsensitive);
    completer_->setCompletionRole(Qt::UserRole);
    connect(completer_, QOverload<const QString&>::of(&QCompleter::activated), this, &CodeEditor::insertCompletion);

    connect(this, &QPlainTextEdit::blockCountChanged, this, [this](int n) {
        updateGutterWidth();
        int delta = n - lastBlockCount_;
        lastBlockCount_ = n;
        if (syncing_ || !delta) return;
        // lines were inserted or removed at the caret: breakpoints below it move with the text
        int line = textCursor().blockNumber() + 1;
        if (delta > 0) line -= delta;
        ctx_.debugger.linesShifted(path_, line, delta);
    });
    connect(this, &QPlainTextEdit::updateRequest, this, [this](const QRect& r, int dy) {
        if (dy) gutter_->scroll(0, dy);
        else gutter_->update(0, r.y(), gutter_->width(), r.height());
        if (r.contains(viewport()->rect())) updateGutterWidth();
    });
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, [this] {
        updateExtra();
        Pos p = posOf(textCursor());
        emit caretLineChanged(p.line);
        ctx_.cursorMoved.fire(path_, p.line);
    });
    // The buffer is loaded BEFORE the editor starts writing back: anything that touches the empty document earlier (a theme change
    // re-colours it and signals contentsChanged) would otherwise overwrite the workspace text with nothing.
    syncing_ = true;
    applyTheme(theme_);
    Document* d = ctx.workspace.open(path);
    setPlainText(d ? toShownText(d->text) : QString());
    document()->setModified(false);
    syncing_ = false;
    connect(document(), &QTextDocument::contentsChanged, this, [this] { onContentsChanged(); });
    lastBlockCount_ = std::max(1, blockCount());
    updateGutterWidth();
    conns_.add(ctx.debugger.breakpointsChanged, [this] { updateExtra(); gutter_->update(); });
    conns_.add(ctx.debugger.updated, [this](const SimUpdate&) { updateExtra(); gutter_->update(); });
    conns_.add(ctx.workspace.documentChanged, [this](const std::string& p) { if (p == path_) syncFromWorkspace(); });
}

void CodeEditor::applyTheme(const Theme& t) {
    theme_ = t;
    QPalette p = palette();
    p.setColor(QPalette::Base, t.background);
    p.setColor(QPalette::Text, t.foreground);
    p.setColor(QPalette::Highlight, t.selection);
    p.setColor(QPalette::HighlightedText, t.foreground);
    setPalette(p);
    hl_->setTheme(t);
    updateExtra();
    gutter_->update();
}

void CodeEditor::syncFromWorkspace() {
    Document* d = ctx_.workspace.find(path_);
    if (!d) return;
    QString text = toShownText(d->text);
    if (text == toPlainText()) return;
    syncing_ = true;
    int pos = textCursor().position();
    int sb = verticalScrollBar()->value();
    setPlainText(text);
    QTextCursor c = textCursor();
    c.setPosition(std::min(pos, (int)text.size()));
    setTextCursor(c);
    verticalScrollBar()->setValue(sb);
    syncing_ = false;
}

QString CodeEditor::toShownText(const std::string& stored) {
    QString t = fromStd(stored);                                    // (fromUtf8 drops a BOM by itself, so look at the bytes)
    if (stored.compare(0, 3, "\xEF\xBB\xBF") == 0) bom_ = true;
    if (t.contains("\r\n")) { crlf_ = true; t.replace("\r\n", "\n"); }
    return t;
}

QString CodeEditor::toWorkspaceText(const QString& shown) const {
    QString t = shown;
    if (crlf_) t.replace("\n", "\r\n");
    if (bom_) t.prepend(QChar(0xFEFF));
    return t;
}

void CodeEditor::onContentsChanged() {
    if (syncing_) return;
    syncing_ = true;
    ctx_.workspace.setText(path_, toStd(toWorkspaceText(toPlainText())));
    syncing_ = false;
}

// ---- positions -------------------------------------------------------------------------------------------------------------------
CodeEditor::Pos CodeEditor::posOf(const QTextCursor& c) const {
    Pos p;
    p.line = c.blockNumber() + 1;
    p.byteCol = byteFromChar(c.block().text(), c.positionInBlock());
    return p;
}

Dom CodeEditor::domainOf(const QTextBlock& b) const {
    int s = b.previous().isValid() ? b.previous().userState() : 0;
    return s == 1 ? Dom::Main : s == 2 ? Dom::Peri : Dom::None;
}

void CodeEditor::goTo(int line, int col, int endCol) {
    QTextBlock b = document()->findBlockByNumber(std::max(0, line - 1));
    if (!b.isValid()) return;
    QTextCursor c(b);
    QString text = b.text();
    int cc = charFromByte(text, col);
    c.setPosition(b.position() + std::min(cc, (int)text.size()));
    if (endCol > col) c.setPosition(b.position() + std::min(charFromByte(text, endCol), (int)text.size()), QTextCursor::KeepAnchor);
    setTextCursor(c);
    centerCursor();
    setFocus();
}

// ---- snapshots ---------------------------------------------------------------------------------------------------------------------
void CodeEditor::setSnapshot(SnapshotPtr snap) {
    snap_ = std::move(snap);
    auto known = std::make_shared<std::set<std::string>>();
    if (snap_) for (const asmcore::IndexSymbol& s : snap_->index.symbols()) known->insert(s.name);
    hl_->setKnownSymbols(snap_ ? known : nullptr);
    updateExtra();
    gutter_->update();
}

QList<asmcore::Diagnostic> CodeEditor::diagnosticsAt(int line, int byteCol, bool wholeLine) const {
    QList<asmcore::Diagnostic> out;
    if (!snap_) return out;
    for (const asmcore::Diagnostic& d : snap_->diagnosticsFor(path_)) {
        if (d.loc.line != line) continue;
        if (wholeLine || d.loc.endCol == 0 || (byteCol >= d.loc.col && byteCol <= d.loc.endCol)) out.append(d);
    }
    return out;
}

void CodeEditor::updateExtra() {
    QList<QTextEdit::ExtraSelection> sel;
    diagSelections_ = 0;
    // current line
    {
        QTextEdit::ExtraSelection s;
        s.format.setBackground(theme_.currentLine);
        s.format.setProperty(QTextFormat::FullWidthSelection, true);
        s.cursor = textCursor();
        s.cursor.clearSelection();
        sel.append(s);
    }
    // bracket match
    {
        QTextCursor c = textCursor();
        QString t = c.block().text();
        int p = c.positionInBlock();
        auto tryAt = [&](int i) {
            if (i < 0 || i >= t.size()) return false;
            QChar ch = t[i];
            const QString open = "[(", close = "])";
            int o = open.indexOf(ch), cl = close.indexOf(ch);
            if (o < 0 && cl < 0) return false;
            int dir = o >= 0 ? 1 : -1;
            QChar mate = o >= 0 ? close[o] : open[cl];
            int depth = 0;
            for (int j = i; j >= 0 && j < t.size(); j += dir) {
                if (t[j] == ch) depth++;
                else if (t[j] == mate && --depth == 0) {
                    for (int k : {i, j}) {
                        QTextEdit::ExtraSelection s;
                        s.format.setBackground(theme_.bracketMatch);
                        s.cursor = QTextCursor(c.block());
                        s.cursor.setPosition(c.block().position() + k);
                        s.cursor.setPosition(c.block().position() + k + 1, QTextCursor::KeepAnchor);
                        sel.append(s);
                    }
                    return true;
                }
            }
            return false;
        };
        if (!tryAt(p)) tryAt(p - 1);
    }
    // the lines the sequencers are about to execute
    execSelections_ = 0;
    if (ctx_.debugger.active())
        for (asmcore::Domain dom : {asmcore::Domain::Main, asmcore::Domain::Peri}) {
            DebugLocation loc = ctx_.debugger.location(dom);
            if (!loc.valid || loc.file != path_) continue;
            QTextBlock b = document()->findBlockByNumber(loc.line - 1);
            if (!b.isValid()) continue;
            QTextEdit::ExtraSelection s;
            s.format.setBackground(dom == asmcore::Domain::Main ? theme_.execMain : theme_.execPeri);
            s.format.setProperty(QTextFormat::FullWidthSelection, true);
            s.cursor = QTextCursor(b);
            sel.append(s);
            execSelections_++;
        }
    // diagnostics
    if (snap_) {
        for (const asmcore::Diagnostic& d : snap_->diagnosticsFor(path_)) {
            if (d.loc.line < 1) continue;
            QTextBlock b = document()->findBlockByNumber(d.loc.line - 1);
            if (!b.isValid()) continue;
            QString text = b.text();
            int c0 = d.loc.endCol > 0 ? charFromByte(text, d.loc.col) : 0;
            int c1 = d.loc.endCol > 0 ? charFromByte(text, d.loc.endCol) : (int)text.size();
            if (c1 <= c0) c1 = std::min((int)text.size(), c0 + 1);
            QTextEdit::ExtraSelection s;
            s.format.setUnderlineStyle(QTextCharFormat::WaveUnderline);
            s.format.setUnderlineColor(d.sev == asmcore::Diagnostic::Error ? theme_.error : theme_.warning);
            s.cursor = QTextCursor(b);
            s.cursor.setPosition(b.position() + c0);
            s.cursor.setPosition(b.position() + c1, QTextCursor::KeepAnchor);
            sel.append(s);
            diagSelections_++;
        }
    }
    setExtraSelections(sel);
}

// ---- gutter ------------------------------------------------------------------------------------------------------------------------
int CodeEditor::lineNumberAreaWidth() const {
    int digits = 1;
    for (int n = std::max(1, blockCount()); n >= 10; n /= 10) digits++;
    return 26 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * std::max(3, digits);
}
void CodeEditor::updateGutterWidth() { setViewportMargins(lineNumberAreaWidth(), 0, 0, 0); }
void CodeEditor::resizeEvent(QResizeEvent* e) {
    QPlainTextEdit::resizeEvent(e);
    QRect cr = contentsRect();
    gutter_->setGeometry(QRect(cr.left(), cr.top(), lineNumberAreaWidth(), cr.height()));
}
void CodeEditor::lineNumberAreaPaint(QPaintEvent* ev) {
    QPainter p(gutter_);
    p.fillRect(ev->rect(), theme_.lineNumberBg);
    QTextBlock b = firstVisibleBlock();
    int n = b.blockNumber();
    int top = (int)blockBoundingGeometry(b).translated(contentOffset()).top();
    int bottom = top + (int)blockBoundingRect(b).height();
    const int cur = textCursor().blockNumber();
    while (b.isValid() && top <= ev->rect().bottom()) {
        if (b.isVisible() && bottom >= ev->rect().top()) {
            bool err = false, warn = false;
            if (snap_) for (const asmcore::Diagnostic& d : snap_->diagnosticsFor(path_)) if (d.loc.line == n + 1) (d.sev == asmcore::Diagnostic::Error ? err : warn) = true;
            if (err || warn) {
                p.setPen(Qt::NoPen);
                p.setBrush(err ? theme_.error : theme_.warning);
                p.drawEllipse(QPoint(19, top + fontMetrics().height() / 2), 3, 3);
            }
            const int cy = top + fontMetrics().height() / 2;
            if (ctx_.debugger.hasLineBreakpoint(path_, n + 1)) {
                bool enabled = true, resolved = true;
                for (const LineBreakpoint& lb : ctx_.debugger.lineBreakpoints()) if (lb.file == path_ && lb.line == n + 1) { enabled = lb.enabled; resolved = lb.resolved; }
                p.setRenderHint(QPainter::Antialiasing);
                p.setPen(QPen(theme_.breakpoint, 1.5));
                p.setBrush(enabled && resolved ? QBrush(theme_.breakpoint) : Qt::NoBrush);
                p.drawEllipse(QPoint(8, cy), 5, 5);
            }
            if (ctx_.debugger.active())
                for (asmcore::Domain dom : {asmcore::Domain::Main, asmcore::Domain::Peri}) {
                    DebugLocation loc = ctx_.debugger.location(dom);
                    if (!loc.valid || loc.file != path_ || loc.line != n + 1) continue;
                    p.setRenderHint(QPainter::Antialiasing);
                    p.setPen(Qt::NoPen);
                    p.setBrush(dom == asmcore::Domain::Main ? QColor("#f5c518") : QColor("#18c5c5"));
                    QPolygon tri;
                    tri << QPoint(2, cy - 5) << QPoint(12, cy) << QPoint(2, cy + 5);
                    p.drawPolygon(tri);
                }
            p.setPen(n == cur ? theme_.foreground : theme_.lineNumberFg);
            p.drawText(0, top, gutter_->width() - 6, fontMetrics().height(), Qt::AlignRight, QString::number(n + 1));
        }
        b = b.next();
        top = bottom;
        bottom = top + (int)blockBoundingRect(b).height();
        n++;
    }
}

void CodeEditor::gutterClicked(int y) {
    QTextBlock b = firstVisibleBlock();
    int top = (int)blockBoundingGeometry(b).translated(contentOffset()).top();
    while (b.isValid()) {
        int bottom = top + (int)blockBoundingRect(b).height();
        if (y >= top && y < bottom) {
            if (!plain_) {
                int used = ctx_.debugger.toggleLineBreakpoint(path_, b.blockNumber() + 1);
                if (used == 0) ctx_.status.fire(tr("no code on or after this line: no breakpoint set"));
                else if (used != b.blockNumber() + 1) ctx_.status.fire(tr("that line has no code: breakpoint moved to line %1").arg(used));
            }
            return;
        }
        top = bottom;
        b = b.next();
    }
}

int CodeEditor::lineY(int line) const {
    QTextBlock b = document()->findBlockByNumber(line - 1);
    if (!b.isValid()) return -1;
    int top = (int)blockBoundingGeometry(b).translated(contentOffset()).top();
    return top + fontMetrics().height() / 2;
}

// ---- tooltips ----------------------------------------------------------------------------------------------------------------------
bool CodeEditor::event(QEvent* e) {
    if (e->type() == QEvent::ToolTip && !plain_) {
        QHelpEvent* he = static_cast<QHelpEvent*>(e);
        QTextCursor c = cursorForPosition(viewport()->mapFrom(this, he->pos()));
        Pos p = posOf(c);
        QStringList lines;
        for (const asmcore::Diagnostic& d : diagnosticsAt(p.line, p.byteCol, false))
            lines << QString("%1 %2: %3").arg(d.sev == asmcore::Diagnostic::Error ? "error" : "warning", fromStd(d.code), fromStd(d.msg));
        std::string hv = hoverText(*ctx_.language, snap_ ? &snap_->index : nullptr, snap_ ? &snap_->result : nullptr, path_, p.line, toStd(c.block().text()), p.byteCol, domainOf(c.block()));
        if (!hv.empty()) lines << fromStd(hv);
        if (lines.isEmpty()) QToolTip::hideText();
        else QToolTip::showText(he->globalPos(), lines.join("\n\n"), this);
        return true;
    }
    return QPlainTextEdit::event(e);
}

// ---- completion --------------------------------------------------------------------------------------------------------------------
QStringList CodeEditor::completionLabels() const {
    QStringList l;
    for (int i = 0; i < model_->rowCount(); i++) l << model_->item(i)->data(Qt::UserRole).toString();
    return l;
}

QString CodeEditor::wordUnderCursor(const QTextCursor& c, int* start) const {
    QString t = c.block().text();
    int e = c.positionInBlock(), b = e;
    auto wordChar = [](QChar ch) { return ch.isLetterOrNumber() || ch == '_' || ch == '.' || ch == '@' || ch == '$' || ch == '?'; };
    while (b > 0 && wordChar(t[b - 1])) b--;
    if (start) *start = b;
    return t.mid(b, e - b);
}

void CodeEditor::triggerCompletion() {
    if (plain_) return;
    QTextCursor c = textCursor();
    QString line = c.block().text();
    int byteCol = byteFromChar(line, c.positionInBlock());
    std::vector<CompletionItem> items = complete(*ctx_.language, snap_ ? &snap_->index : nullptr, toStd(line), byteCol, domainOf(c.block()));
    model_->clear();
    for (const CompletionItem& it : items) {
        auto* row = new QStandardItem(fromStd(it.label) + (it.detail.empty() ? QString() : "    " + fromStd(it.detail)));
        row->setData(fromStd(it.insert), Qt::UserRole);
        model_->appendRow(row);
    }
    prefix_ = wordUnderCursor(c);
    if (items.empty()) { completer_->popup()->hide(); return; }
    completer_->setCompletionPrefix(prefix_);
    completer_->popup()->setCurrentIndex(completer_->completionModel()->index(0, 0));
    if (completer_->completionCount() == 0) { completer_->popup()->hide(); return; }
    QRect r = cursorRect();
    r.setWidth(completer_->popup()->sizeHintForColumn(0) + completer_->popup()->verticalScrollBar()->sizeHint().width());
    completer_->complete(r);
}

void CodeEditor::insertCompletion(const QString& insert) {
    QTextCursor c = textCursor();
    int start = 0;
    wordUnderCursor(c, &start);
    c.setPosition(c.block().position() + start);
    c.setPosition(textCursor().position(), QTextCursor::KeepAnchor);
    c.insertText(insert);
    setTextCursor(c);
}

// ---- keys --------------------------------------------------------------------------------------------------------------------------
void CodeEditor::keyPressEvent(QKeyEvent* e) {
    if (completer_->popup()->isVisible()) {
        switch (e->key()) {
            case Qt::Key_Enter: case Qt::Key_Return: case Qt::Key_Tab: case Qt::Key_Backtab: case Qt::Key_Escape:
                e->ignore();
                return;                                             // the completer takes these
            default: break;
        }
    }
    const bool ctrl = e->modifiers() & Qt::ControlModifier, shift = e->modifiers() & Qt::ShiftModifier;
    if (ctrl && e->key() == Qt::Key_Space) { triggerCompletion(); return; }
    if (e->key() == Qt::Key_F12) { if (shift) findReferences(); else goToDefinition(); return; }
    if (e->key() == Qt::Key_F2) { renameSymbol(); return; }
    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
        QString line = textCursor().block().text();
        QString indent = line.left(line.indexOf(QRegularExpression("[^ \t]")) < 0 ? line.size() : line.indexOf(QRegularExpression("[^ \t]")));
        if (indent.isEmpty() && line.trimmed().endsWith(':')) indent = "    ";        // after a label, the statement is indented
        QPlainTextEdit::keyPressEvent(e);
        textCursor().insertText(indent);
        return;
    }
    if (e->key() == Qt::Key_Tab && !ctrl) {
        QTextCursor c = textCursor();
        if (c.hasSelection()) {
            int a = document()->findBlock(c.selectionStart()).blockNumber(), b = document()->findBlock(c.selectionEnd()).blockNumber();
            c.beginEditBlock();
            for (int n = a; n <= b; n++) { QTextCursor l(document()->findBlockByNumber(n)); l.insertText("    "); }
            c.endEditBlock();
        } else {
            int col = c.positionInBlock();
            c.insertText(QString(4 - col % 4, ' '));
        }
        return;
    }
    if (e->key() == Qt::Key_Backtab) {
        QTextCursor c = textCursor();
        int a = document()->findBlock(c.selectionStart()).blockNumber(), b = document()->findBlock(c.selectionEnd()).blockNumber();
        c.beginEditBlock();
        for (int n = a; n <= b; n++) {
            QTextBlock blk = document()->findBlockByNumber(n);
            int k = 0;
            while (k < 4 && k < blk.text().size() && blk.text()[k] == ' ') k++;
            QTextCursor l(blk);
            l.setPosition(blk.position() + k, QTextCursor::KeepAnchor);
            l.removeSelectedText();
        }
        c.endEditBlock();
        return;
    }
    QPlainTextEdit::keyPressEvent(e);
    if (e->text().isEmpty() || ctrl) return;
    QChar ch = e->text()[0];
    bool typing = ch.isLetterOrNumber() || ch == '_' || ch == '.' || ch == ',' || ch == ' ' || ch == '=' || ch == '>';
    QString w = wordUnderCursor(textCursor());
    if (typing && (ch == ',' || ch == ' ' || ch == '=' || ch == '>' || !w.isEmpty())) {
        if (ch == ' ' && textCursor().positionInBlock() > 0) {
            // after the mnemonic a space opens the first operand list; in other places it does not
            QString left = textCursor().block().text().left(textCursor().positionInBlock()).trimmed();
            if (left.contains(' ') || left.isEmpty()) { completer_->popup()->hide(); return; }
        }
        triggerCompletion();
    } else {
        completer_->popup()->hide();
    }
}

// ---- navigation --------------------------------------------------------------------------------------------------------------------
void CodeEditor::goToDefinition() {
    if (!snap_) return;
    Pos p = posOf(textCursor());
    const asmcore::IndexSymbol* s = snap_->index.at(path_, p.line, p.byteCol);
    if (!s) { ctx_.status.fire(QString("no symbol under the cursor")); return; }
    Location l;
    l.file = s->def.file; l.line = s->def.line; l.col = s->def.col; l.endCol = s->def.endCol;
    ctx_.goTo.fire(l);
}

void CodeEditor::findReferences() {
    if (!snap_) return;
    Pos p = posOf(textCursor());
    const asmcore::IndexSymbol* s = snap_->index.at(path_, p.line, p.byteCol);
    if (!s) { ctx_.status.fire(QString("no symbol under the cursor")); return; }
    std::vector<Location> v;
    for (const asmcore::SrcLoc& l : snap_->index.occurrences(*s)) v.push_back(Location{l.file, l.line, l.col, l.endCol});
    ctx_.showReferences.fire(QString("References to %1 (%2)").arg(fromStd(s->name)).arg(v.size()), v);
}

void CodeEditor::renameSymbol() {
    if (!snap_) return;
    Pos p = posOf(textCursor());
    const asmcore::IndexSymbol* s = snap_->index.at(path_, p.line, p.byteCol);
    if (!s) { ctx_.status.fire(QString("no symbol under the cursor")); return; }
    bool ok = false;
    QString nn = QInputDialog::getText(this, tr("Rename"), tr("New name for %1 (%2 uses):").arg(fromStd(s->name)).arg(s->refs.size()), QLineEdit::Normal, fromStd(s->name), &ok);
    if (!ok || nn.isEmpty() || toStd(nn) == s->name) return;
    std::map<std::string, std::string> texts;
    for (const asmcore::SrcLoc& l : snap_->index.occurrences(*s)) {
        Document* d = ctx_.workspace.open(l.file);
        if (d) texts[l.file] = d->text;
    }
    std::string problem;
    auto edits = renameEdits(snap_->index, *s, toStd(nn), texts, problem);
    if (edits.empty()) { QMessageBox::warning(this, tr("Rename"), fromStd(problem)); return; }
    for (auto& kv : edits) ctx_.workspace.setText(kv.first, kv.second);
    ctx_.status.fire(tr("Renamed in %1 file(s)").arg(edits.size()));
}

void CodeEditor::toggleComment() {
    QTextCursor c = textCursor();
    int a = document()->findBlock(c.selectionStart()).blockNumber(), b = document()->findBlock(c.selectionEnd()).blockNumber();
    if (c.hasSelection() && c.selectionEnd() == document()->findBlockByNumber(b).position() && b > a) b--;     // a selection ending at a line start
    bool allCommented = true;
    for (int n = a; n <= b; n++) {
        QString t = document()->findBlockByNumber(n).text().trimmed();
        if (!t.isEmpty() && !t.startsWith(';')) allCommented = false;
    }
    c.beginEditBlock();
    for (int n = a; n <= b; n++) {
        QTextBlock blk = document()->findBlockByNumber(n);
        QString t = blk.text();
        if (t.trimmed().isEmpty()) continue;
        int indent = t.indexOf(QRegularExpression("[^ \t]"));
        QTextCursor l(blk);
        l.setPosition(blk.position() + indent);
        if (allCommented) {
            int len = (t.mid(indent + 1, 1) == " ") ? 2 : 1;
            l.setPosition(blk.position() + indent + len, QTextCursor::KeepAnchor);
            l.removeSelectedText();
        } else {
            l.insertText("; ");
        }
    }
    c.endEditBlock();
}

void CodeEditor::duplicateLine() {
    QTextCursor c = textCursor();
    QString t = c.block().text();
    c.beginEditBlock();
    c.movePosition(QTextCursor::EndOfBlock);
    c.insertText("\n" + t);
    c.endEditBlock();
    setTextCursor(c);
}

void CodeEditor::moveLines(int delta) {
    QTextCursor c = textCursor();
    int a = document()->findBlock(c.selectionStart()).blockNumber(), b = document()->findBlock(c.selectionEnd()).blockNumber();
    int other = delta < 0 ? a - 1 : b + 1;
    if (other < 0 || other >= document()->blockCount()) return;
    c.beginEditBlock();
    QString moved;
    for (int n = a; n <= b; n++) moved += document()->findBlockByNumber(n).text() + (n < b ? "\n" : "");
    QString swap = document()->findBlockByNumber(other).text();
    QTextCursor all(document());
    int from = delta < 0 ? other : a, to = delta < 0 ? b : other;
    all.setPosition(document()->findBlockByNumber(from).position());
    QTextBlock last = document()->findBlockByNumber(to);
    all.setPosition(last.position() + last.length() - 1, QTextCursor::KeepAnchor);
    all.insertText(delta < 0 ? moved + "\n" + swap : swap + "\n" + moved);
    c.endEditBlock();
    QTextCursor n(document()->findBlockByNumber(a + delta));
    setTextCursor(n);
}

void CodeEditor::gotoLineDialog() {
    bool ok = false;
    int n = QInputDialog::getInt(this, tr("Go to line"), tr("Line:"), textCursor().blockNumber() + 1, 1, std::max(1, blockCount()), 1, &ok);
    if (ok) goTo(n, 0, 0);
}

void CodeEditor::contextMenuEvent(QContextMenuEvent* e) {
    QMenu* m = createStandardContextMenu();
    m->addSeparator();
    m->addAction(tr("Go to Definition\tF12"), this, &CodeEditor::goToDefinition);
    m->addAction(tr("Find All References\tShift+F12"), this, &CodeEditor::findReferences);
    m->addAction(tr("Rename Symbol\tF2"), this, &CodeEditor::renameSymbol);
    m->exec(e->globalPos());
    delete m;
}

void CodeEditor::mouseReleaseEvent(QMouseEvent* e) {
    QPlainTextEdit::mouseReleaseEvent(e);
    if (e->button() == Qt::LeftButton && (e->modifiers() & Qt::ControlModifier)) goToDefinition();
}

}  // namespace acide
