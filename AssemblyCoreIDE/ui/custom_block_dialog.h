#pragma once
// The dialog that makes (or edits) a block of the user's own in the toolbox: which drawer, its name, and the assembler text it is made from. The text can be taken
// from the blocks selected on the canvas. The dialog says at once whether the text makes blocks.
#include <QDialog>

#include "blocks/block_catalog.h"

class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;

namespace acide {

class CustomBlockDialog : public QDialog {
    Q_OBJECT
public:
    // drawers: the drawers to choose from (their names as the catalog has them); replacing: the id of the entry being edited ("" for a new one)
    CustomBlockDialog(const blocks::BlockCatalog& catalog, const std::vector<std::string>& drawers, const QString& replacingId, QWidget* parent);
    void setValues(const QString& drawer, const QString& title, const QString& text);
    blocks::CustomBlock values() const;
    void setSelectionText(const QString& text);             // what the "from the selected blocks" button puts in the text (empty: the button is off)
    QLineEdit* titleEdit() const { return title_; }
    QPlainTextEdit* textEdit() const { return text_; }
    QComboBox* drawerBox() const { return drawer_; }
    QString message() const;                                  // what the dialog says about the text now
    bool valid() const;
    const QString& replacingId() const { return replacing_; }

private:
    void validate();
    const blocks::BlockCatalog& catalog_;
    QString replacing_, selection_;
    QComboBox* drawer_;
    QLineEdit* title_;
    QPlainTextEdit* text_;
    QLabel* note_;
    QPushButton *fromSelection_;
    QDialogButtonBox* buttons_;
};

}  // namespace acide
