#pragma once
// The tab of a .acblk file: one program that can be looked at in two pages -- the blocks, or the assembler text they generate. A pair of
// radio buttons at the top switches the page. Both pages show the same BlockSession, so a block selected on one is the line marked on the
// other. (M2: looking only; editing arrives with the next milestone.)
#include <map>
#include <memory>
#include <string>

#include <QWidget>

#include "app_context.h"
#include "block_scene.h"
#include "block_view.h"
#include "blocks/block_session.h"
#include "block_toolbox.h"
#include "blocks/block_catalog.h"
#include "comment_edit.h"
#include "custom_block_dialog.h"
#include "typeblock_box.h"
#include "field_editor.h"
#include "document_tab.h"
#include "generated_code_pane.h"

class QLabel;
class QMenu;
class QSplitter;
class QRadioButton;
class QStackedWidget;
class QToolButton;

namespace acide {

// Where copied blocks go. The editor copies to the system clipboard; a test (or a platform whose clipboard is busy) can give it another.
class IBlockClipboard {
public:
    virtual ~IBlockClipboard() = default;
    virtual void set(const std::string& text, const std::string& blocks) = 0;       // the assembler text, and the blocks themselves (JSON)
    virtual void get(std::string& text, std::string& blocks) = 0;                   // either may be empty
};

class BlockEditor : public QWidget, public DocumentTab {
    Q_OBJECT
public:
    BlockEditor(AppContext& ctx, const std::string& acblkPath, QWidget* parent = nullptr);
    ~BlockEditor() override;

    // DocumentTab
    std::string tabPath() const override { return path_; }
    QString tabTitle() const override;
    void prepareToClose() override { flushSync(); }
    // A big program (more than 300 blocks) is written to the workspace a moment after the last edit (serializing it on every edit would be felt); this does it now.
    // Everything that reads or writes files calls it first (saving, building, closing).
    void flushSync();
    std::vector<std::string> workspacePaths() const override { return {path_, generatedPath()}; }
    // the assembler file generated from these blocks (the program's `output`, else NAME.generated.asm)
    std::string generatedPath() const;
    QWidget* widget() override { return this; }

    enum class Page { Blocks, Code };
    Page page() const;
    void setPage(Page p);

    blocks::BlockSession& session() { return session_; }
    BlockScene& scene() { return *scene_; }
    BlockView& view() { return *view_; }
    GeneratedCodePane& codePane() { return *code_; }
    CommentEdit& commentEdit() { return *commentEdit_; }
    FieldEditor& fieldEditor() { return *fieldEditor_; }
    BlockToolbox& toolbox() { return *toolbox_; }
    TypeBlockBox& typeblock() { return *typeblock_; }
    const blocks::BlockCatalog& catalog() const { return catalog_; }
    // Puts a block of the toolbox (or a typed line of assembler) on the canvas: after the selected block when it fits, else on the canvas in the middle of the view.
    size_t insertEntry(const QString& entryId);
    size_t insertAssembler(const QString& text);
    void openTypeblock(const QString& initial = QString());
    // The user's own blocks in the toolbox: a dialog to make (or edit) one -- from the blocks selected on the canvas if asked -- and to delete one. Kept in the settings.
    void newCustomBlock(const QString& drawer, bool fromSelection, const QString& replacingId = QString());
    void deleteCustomBlock(const QString& entryId);
    CustomBlockDialog* customDialog() const { return customDialog_; }
    QString errorText() const;                       // why the file could not be shown ("" if fine)
    QString infoText() const;
    QMenu* choiceMenu() const { return choiceMenu_; }         // the menu of a closed choice while it is open (for tests)
    void jumpTo(const QString& kind);                         // "floating" / "raw" / "problems": select the next such block and show it (the counts of the status line are links)
    void selectBlock(const std::string& id, bool reveal = true);
    // ---- what Edit > Cut / Copy / Paste ... and the context menu do to the selection
    void setClipboard(std::shared_ptr<IBlockClipboard> c) { clipboard_ = std::move(c); }
    void copy();
    void cut();
    void paste();
    void duplicateSelection() { session_.duplicateSelection(); }
    void deleteSelection() { session_.deleteSelection(); }
    void toggleDisabled() { session_.toggleDisabled(); }
    void toggleCollapsed() { session_.toggleCollapsed(); }
    void selectAll() { scene_->selectAll(); }
    void tidy();                                                          // every stack gets a place; nothing overlaps; the program is unchanged
    void undo() { if (scene_->dragging()) scene_->cancelDrag(); else session_.undo(); }       // (the Edit menu calls these)
    void redo() { session_.redo(); }

protected:
    bool eventFilter(QObject* o, QEvent* e) override;         // a click on the canvas closes an open editor, keeping what was typed

private:
    void reload();
    void refreshCode();
    void refreshInfo();
    void applyTheme();
    void showContextMenu(const QPointF& scenePos, const QPoint& globalPos, const std::string& blockId);
    void beginFieldEdit(const BlockScene::EditTarget& t);                 // a double click on a part of a block: type into it over the block
    void beginCommentEdit(const BlockScene::CommentTarget& t);            // a double click on the end of a block: edit its comment over it
    void refreshSymbols();                           // the names the hardware description defines (for the "not defined" checks)
    void onBlockClicked(const std::string& id, Qt::KeyboardModifiers m);

    AppContext& ctx_;
    std::string path_;
    blocks::BlockSession session_;
    QRadioButton *blocksRadio_, *codeRadio_;
    QStackedWidget* pages_;
    BlockScene* scene_ = nullptr;
    BlockView* view_ = nullptr;
    GeneratedCodePane* code_ = nullptr;
    CommentEdit* commentEdit_ = nullptr;
    FieldEditor* fieldEditor_ = nullptr;
    BlockToolbox* toolbox_ = nullptr;
    TypeBlockBox* typeblock_ = nullptr;
    QSplitter* blocksPage_ = nullptr;
    CustomBlockDialog* customDialog_ = nullptr;
    blocks::BlockCatalog catalog_;
    std::vector<asmcore::blocks::Block> paletteBlocks_;
    QString paletteEntry_;
    BlockScene::EditTarget fieldEditing_;
    unsigned editSerial_ = 0;
    std::shared_ptr<IBlockClipboard> clipboard_;
    BlockScene::CommentTarget editing_;
    unsigned commentSerial_ = 0;
    QWidget* zoomBar_;
    QLabel *info_, *banner_, *zoomLabel_;
    std::unique_ptr<ConnectionBag> conns_ = std::make_unique<ConnectionBag>();      // released first in the destructor
    std::vector<std::string> blocksFor(const QString& kind) const;
    std::map<QString, unsigned> jumpAt_;
    void refreshHardwareDrawers(const std::string& achwPath);
    void showChoiceMenu(const BlockScene::EditTarget& t);
    void beginTextEdit(const BlockScene::EditTarget& t);
    void applyFieldValue(const BlockScene::EditTarget& t, const QString& text);
    QMenu* choiceMenu_ = nullptr;
    void applyBuildProblems();                               // the assembler's messages about the generated file become badges on the blocks that produced the lines
    SnapshotPtr snap_;
    void syncToWorkspace();
    void doSync();
    QTimer* syncTimer_ = nullptr;                                  // after an edit: the program and the generated file go into the workspace buffers (unsaved)
    bool syncing_ = false, loading_ = true, attached_ = false, codeStale_ = true;
    std::string lastText_, syncedText_, symbolsFrom_;
};

}  // namespace acide
