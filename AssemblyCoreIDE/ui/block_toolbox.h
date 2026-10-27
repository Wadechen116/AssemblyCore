#pragma once
// The toolbox: the drawers of the catalog (patterns, the instruction drawers, structure, definitions, text, the components), each with its samples. Drag a sample
// onto the canvas, or double-click it (or press Enter) to put it after the selected block. The search box above filters the samples as you type.
#include <QTreeWidget>
#include <QWidget>

#include "blocks/block_catalog.h"
#include "blocks/block_theme.h"

class QLineEdit;

namespace acide {

class SampleTree : public QTreeWidget {
    Q_OBJECT
public:
    explicit SampleTree(QWidget* parent = nullptr) : QTreeWidget(parent) {}
    static const char* mimeType() { return "application/x-acblk-sample"; }

protected:
    QMimeData* mimeData(const QList<QTreeWidgetItem*>& items) const override;
    void startDrag(Qt::DropActions) override;
};

class BlockToolbox : public QWidget {
    Q_OBJECT
public:
    BlockToolbox(const blocks::BlockCatalog& catalog, const blocks::BlockTheme& theme, QWidget* parent = nullptr);
    void setTheme(const blocks::BlockTheme& t);
    void setFilter(const QString& text);                          // the search box
    QTreeWidget* tree() const { return tree_; }
    QLineEdit* search() const { return search_; }
    int visibleSamples() const;                                    // samples shown with the current filter

    // the drawers the user can add to (the names as the catalog has them), and the drawer under an item or a header
    std::vector<std::string> drawerNames() const;
    bool catalogHas(const QString& id) const { return catalog_.find(id.toStdString()) != nullptr; }
    void refresh();                                                // the catalog changed (a block was added / removed): build the drawers again, keeping what is open and the filter
    void setSelectionAvailable(bool on) { selectionAvailable_ = on; }

signals:
    void activated(const QString& entryId);                        // double click / Enter on a sample
    void addRequested(const QString& drawer, bool fromSelection);  // the + of a drawer, or "New block" in its menu (fromSelection: made from the blocks selected on the canvas)
    void editRequested(const QString& entryId);                    // a block of the user's own
    void deleteRequested(const QString& entryId);

private:
    void showMenu(const QPoint& at);
    void build();
    void applyFilter();
    const blocks::BlockCatalog& catalog_;
    blocks::BlockTheme theme_;
    SampleTree* tree_;
    QLineEdit* search_;
    bool selectionAvailable_ = false;
};

}  // namespace acide
