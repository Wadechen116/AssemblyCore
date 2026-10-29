#pragma once
// The source editor: a QPlainTextEdit with line numbers, language-aware colouring, diagnostics (wavy underlines, gutter marks, tooltips),
// bracket matching, completion and symbol navigation. It edits the Workspace buffer of its file and nothing else; builds reach it as
// snapshots.
#include <memory>
#include <set>

#include <QCompleter>
#include <QPlainTextEdit>
#include <QStandardItemModel>

#include "app_context.h"
#include "syntax_highlighter.h"

namespace acide {

class CodeEditor : public QPlainTextEdit {
    Q_OBJECT
public:
    CodeEditor(AppContext& ctx, const std::string& path, QWidget* parent = nullptr);

    const std::string& path() const { return path_; }
    void setSnapshot(SnapshotPtr snap);                    // diagnostics, known symbols, navigation
    void applyTheme(const Theme& theme);
    void goTo(int line, int col, int endCol);              // 1-based line; bytes
    // The whole text becomes `text`, as ONE undo step (Ctrl+Z gives the old text back); the caret keeps its line. False for a generated file.
    bool replaceAllText(const QString& text);
    void syncFromWorkspace();                              // the buffer changed elsewhere (reload, rename)
    void goToDefinition();
    void findReferences();
    void renameSymbol();
    void toggleComment();                                  // ';' on the selected lines
    void duplicateLine();
    void moveLines(int delta);                             // -1 up, +1 down
    void gotoLineDialog();
    // A file generated from a block program: read-only, with a banner that says where it comes from and a button that opens the blocks. "" = an ordinary file.
    void setGeneratedFrom(const std::string& acblkPath);
    const std::string& generatedFrom() const { return generatedFrom_; }
    QWidget* generatedBanner() const { return banner_; }
    bool refuseIfGenerated();                              // says why an edit is not possible and returns true for a generated file
    void gutterClicked(int y);                             // a click in the line-number area: toggle a breakpoint
    void gutterPressed(const QPoint& pos);                 // ... or, on the flag of a sequencer's cursor, make that sequencer the one Step acts on
    // The cursors of the two sequencers in this file (the line each is about to execute), and which one Step acts on; for tests
    struct CursorMark { int domain = 0; int line = 0; bool focus = false; };   // domain: 0 Main, 1 Peri
    std::vector<CursorMark> cursorMarks() const;

    // used by the line number area
    int lineNumberAreaWidth() const;
    void lineNumberAreaPaint(QPaintEvent* e);

    // for tests
    int diagnosticSelectionCount() const { return diagSelections_; }
    int executionSelectionCount() const { return execSelections_; }      // lines highlighted as "about to execute"
    QWidget* gutterWidget() const { return gutter_; }
    int lineY(int line) const;                             // y (in the gutter) of the middle of a 1-based line, for tests
    QStringList completionLabels() const;
    void triggerCompletion();

signals:
    void caretLineChanged(int line);
    void openBlocksRequested(const QString& acblkPath);

protected:
    void resizeEvent(QResizeEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void paintEvent(QPaintEvent* e) override;
    bool event(QEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;

private:
    struct Pos { int line = 1; int byteCol = 0; };
    Pos posOf(const QTextCursor& c) const;
    asmcore::lang::Dom domainOf(const QTextBlock& b) const;
    void updateExtra();
    void updateGutterWidth();
    int bannerHeight() const;
    void onContentsChanged();
    void insertCompletion(const QString& insert);
    QList<asmcore::Diagnostic> diagnosticsAt(int line, int byteCol, bool wholeLineIfNoColumn) const;
    QString wordUnderCursor(const QTextCursor& c, int* start = nullptr) const;

    AppContext& ctx_;
    std::string path_;
    SnapshotPtr snap_;
    Theme theme_;
    SyntaxHighlighter* hl_;
    QWidget* gutter_;
    QWidget* banner_ = nullptr;
    std::string generatedFrom_;
    QCompleter* completer_;
    QStandardItemModel* model_;
    QString prefix_;
    // The editor shows '\n' line ends and no BOM; what the file had is put back when the text goes to the workspace.
    QString toWorkspaceText(const QString& shown) const;
    QString toShownText(const std::string& stored);
    bool crlf_ = false, bom_ = false;
    bool syncing_ = false;
    bool plain_ = false;                                   // not assembly (an environment file...): no colouring, completion or navigation
    int lastBlockCount_ = 1;
    int execSelections_ = 0;
    int diagSelections_ = 0;
    ConnectionBag conns_;
    friend class LineNumberArea;
};

}  // namespace acide
