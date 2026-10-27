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

signals:
    void activated(const QString& entryId);                        // double click / Enter on a sample

private:
    void build();
    void applyFilter();
    const blocks::BlockCatalog& catalog_;
    blocks::BlockTheme theme_;
    SampleTree* tree_;
    QLineEdit* search_;
};

}  // namespace acide
