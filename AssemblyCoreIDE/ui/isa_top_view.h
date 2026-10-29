#pragma once
// The ISA Top diagram, live: the boxes of services/isa_layout drawn on a canvas, their rows showing what the simulator holds now, the wires of the path an
// instruction takes lit while it runs (Main's colour amber, Peri's cyan), a row that changed flashing. A click on a box opens the panel it stands for (the
// Memory panel for the scratchpad ...) or shows the line a decoder is at. While nothing is debugged the diagram is there with "-" for every value.
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <QElapsedTimer>
#include <QScrollArea>
#include <QTimer>

#include "app_context.h"
#include "isa_layout.h"

namespace acide {

class IsaCanvas;

class IsaTopView : public QScrollArea {
public:
    explicit IsaTopView(AppContext& ctx, QWidget* parent = nullptr);
    ~IsaTopView() override;

    const IsaLayout& layout() const { return layout_; }
    void setLayout(const IsaLayout& l);                       // another diagram (tests, a layout file)
    void fit();                                               // the whole diagram in the view (width and height)
    void fitWidth();                                          // the whole width (the default: readable, the rest scrolls)
    void setZoom(double z);                                   // 1 = canvas pixels
    double zoom() const { return zoom_; }

    // what is going on, for tests
    std::vector<std::string> litWires() const;                // glowing now
    std::vector<std::string> pulsingBoxes() const;            // a decoder that just executed
    std::vector<std::string> flashingRows() const;            // "box/label" of a row whose value just changed
    std::string valueOf(const std::string& box, const std::string& label) const;
    QRect boxRect(const std::string& id) const;               // in the coordinates of canvas()
    QWidget* canvas() const;
    void feed(const SimUpdate& u);                            // what Debugger::updated delivers
    void refreshValues();                                     // read every row again from the simulator's current state

protected:
    void resizeEvent(QResizeEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;

private:
    friend class IsaCanvas;
    void rescale();
    void tick();
    bool animating() const;
    std::string instrAt(int domain, uint32_t pc);
    void activate(const IsaBox& b);

    AppContext& ctx_;
    IsaLayout layout_;
    IsaCanvas* canvas_ = nullptr;
    QElapsedTimer clock_;
    QTimer timer_;
    double zoom_ = 1;
    bool fit_ = true, fitAll_ = false;
    bool live_ = false;
    std::map<std::string, std::pair<qint64, bool>> lit_;      // wire -> (when, peri?)
    std::map<std::string, std::pair<qint64, bool>> pulse_;    // box -> (when, peri?)
    std::vector<std::vector<std::string>> values_;            // [box][row]
    std::vector<std::vector<qint64>> changedAt_;
    std::map<std::pair<int, uint32_t>, std::string> instrs_;
    const void* instrsFor_ = nullptr;
    ConnectionBag conns_;
};

// The page the Simulator tab holds: the view with a small header (fit / 100 %).
QWidget* createIsaTopPanel(AppContext& ctx);

}  // namespace acide
