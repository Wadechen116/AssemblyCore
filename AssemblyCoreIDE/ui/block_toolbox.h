#pragma once
// The toolbox: the drawers of the catalog (the instruction drawers, structure, definitions, text, the components -- each sample ONE piece with empty sockets --, then
// the ready-made combinations: patterns and the hardware drawers, closed at the start), each with its samples. Drag a sample
// onto the canvas, or double-click it (or press Enter) to put it after the selected block. The search box above filters the samples as you type.
#include <functional>
#include <map>

#include <QImage>
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
    void resizeEvent(QResizeEvent* e) override;
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
    // How a sample is drawn: a picture of its blocks as the canvas draws them (the editor renders them with its scene). Not set, or a null picture: the sample is a line of text.
    // `scale`: the size the blocks are drawn at (1 = as on the canvas). The toolbox asks for the picture at the size it shows it, so the text is sharp.
    using Previewer = std::function<QImage(const blocks::CatalogEntry&, double scale)>;
    void setPreviewer(Previewer p) { previewer_ = std::move(p); refresh(); }
    // The picture of a sample, drawn the first time it is needed (a row being shown) and kept until the drawers are built again. Null: no picture.
    QImage picture(const QString& entryId) const;                 // at the size of the canvas (what a drag carries; its size is what a row is measured from)
    QImage picture(const QString& entryId, double scale) const;   // drawn at `scale` (what a row shows)
    // The size samples are shown at: kScale of the canvas, smaller (down to kMinScale) for a block too wide for the toolbox.
    static constexpr double kScale = 0.95, kMinScale = 0.72;

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
    Previewer previewer_;
    mutable std::map<QString, QImage> pictures_;
    mutable std::map<std::pair<QString, int>, QImage> scaled_;     // (entry, scale in hundredths)
};

}  // namespace acide
