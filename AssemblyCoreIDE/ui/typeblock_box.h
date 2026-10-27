#pragma once
// Typeblock: type on the canvas and a block appears. A line of text and a short list under it: the blocks whose name matches, and -- when what is typed is a
// line of assembler -- "insert as assembler" first. Enter takes the highlighted one, Up / Down choose, Esc closes. Nothing here knows where the block goes: the owner
// is told what was chosen.
#include <QFrame>

#include "blocks/block_catalog.h"

class QLineEdit;
class QListWidget;

namespace acide {

class TypeBlockBox : public QFrame {
    Q_OBJECT
public:
    TypeBlockBox(const blocks::BlockCatalog& catalog, QWidget* parent);
    void open(const QRect& within, const QString& initialText);     // opens at the bottom middle of `within` (coordinates of the parent)
    bool isOpen() const { return open_; }
    QLineEdit* edit() const { return edit_; }
    QListWidget* list() const { return list_; }
    int resultCount() const;

signals:
    void chosenEntry(const QString& entryId);                        // a block of the catalog
    void chosenAssembler(const QString& text);                       // a typed line of assembler
    void closed();

protected:
    bool eventFilter(QObject* o, QEvent* e) override;

private:
    void refresh();
    void finish(bool take);
    const blocks::BlockCatalog& catalog_;
    QLineEdit* edit_;
    QListWidget* list_;
    bool open_ = false;
    QRect within_;
};

}  // namespace acide
