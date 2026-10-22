#pragma once
// The tab of a .acblk file: one program that can be looked at in two pages -- the blocks, or the assembler text they generate. A pair of
// radio buttons at the top switches the page. Both pages show the same BlockSession, so a block selected on one is the line marked on the
// other. (M2: looking only; editing arrives with the next milestone.)
#include <memory>
#include <string>

#include <QWidget>

#include "app_context.h"
#include "block_scene.h"
#include "block_view.h"
#include "blocks/block_session.h"
#include "comment_edit.h"
#include "document_tab.h"
#include "generated_code_pane.h"

class QLabel;
class QRadioButton;
class QStackedWidget;
class QToolButton;

namespace acide {

class BlockEditor : public QWidget, public DocumentTab {
    Q_OBJECT
public:
    BlockEditor(AppContext& ctx, const std::string& acblkPath, QWidget* parent = nullptr);
    ~BlockEditor() override;

    // DocumentTab
    std::string tabPath() const override { return path_; }
    QString tabTitle() const override;
    std::vector<std::string> workspacePaths() const override { return {path_}; }
    QWidget* widget() override { return this; }

    enum class Page { Blocks, Code };
    Page page() const;
    void setPage(Page p);

    blocks::BlockSession& session() { return session_; }
    BlockScene& scene() { return *scene_; }
    BlockView& view() { return *view_; }
    GeneratedCodePane& codePane() { return *code_; }
    CommentEdit& commentEdit() { return *commentEdit_; }
    QString errorText() const;                       // why the file could not be shown ("" if fine)
    QString infoText() const;                        // the summary line
    void selectBlock(const std::string& id, bool reveal = true);
    void undo() { if (scene_->dragging()) scene_->cancelDrag(); else session_.undo(); }       // (the Edit menu calls these)
    void redo() { session_.redo(); }

private:
    void reload();
    void refreshCode();
    void refreshInfo();
    void applyTheme();
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
    BlockScene::CommentTarget editing_;
    unsigned commentSerial_ = 0;
    QWidget* zoomBar_;
    QLabel *info_, *banner_, *zoomLabel_;
    std::unique_ptr<ConnectionBag> conns_ = std::make_unique<ConnectionBag>();      // released first in the destructor
    bool syncing_ = false;
    std::string lastText_, symbolsFrom_;
};

}  // namespace acide
