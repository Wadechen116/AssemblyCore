#pragma once
// The ISA Top diagram as DATA: boxes, the rows of values they show, the wires between them, and which wires an instruction lights up. The picture is drawn from
// this (ui/isa_top_view); when the ISA Top design changes the text changes, not the code. The default layout is built in; a project or the user can give another
// file (same format). No Qt.
//
// Format (UTF-8 text; '#' starts a comment; words separated by spaces, "quoted text" may hold spaces):
//   canvas  W H
//   box     ID X Y W H KIND "Title" [domain=main|peri] [target=PANEL] [help="text"]
//             KIND: code decoder data alu flags timer shared device (the colour)
//             target: the panel a click opens (memory csrs signals state watch events), or "code" = the line the sequencer is at (decoder boxes)
//   row     BOX "Label" SOURCE...
//             SOURCE: pc main|peri | instr main|peri | count main|peri | fault main|peri | cycle
//                     region NAME A..B     (bytes of scratch csrbuf rx slot meta opa opb out status const, as hex)
//                     signal NAME          (a number)        flag NAME   (a lamp: on / off)
//                     csrn N               (the N-th CSR the program names: NAME=0xVV)
//                     text "static text"
//   wire    ID "Label" X,Y X,Y ...         (a polyline; the arrow is at the last point)
//   path    main|peri MNEMONIC = WIRE...   (the wires lit while an instruction of that mnemonic runs; MNEMONIC is the start of the instruction text)
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "sim_session.h"

namespace acide {

struct IsaSource {
    enum Kind { Text, Pc, Instr, Count, Fault, Cycle, Region, Signal, Flag, Csrn } kind = Text;
    int domain = 0;                       // 0 Main, 1 Peri
    std::string name, text;
    int from = 0, to = 0;                 // Region: bytes; Csrn: the index in from
};

struct IsaRow {
    std::string label;
    IsaSource src;
};

struct IsaBox {
    std::string id, title, kind, target, help;
    double x = 0, y = 0, w = 0, h = 0;
    int domain = -1;                      // -1 none, 0 main, 1 peri
    std::vector<IsaRow> rows;
};

struct IsaWire {
    std::string id, label;
    std::vector<std::pair<double, double>> points;
};

struct IsaLayout {
    double width = 1200, height = 720;
    std::vector<IsaBox> boxes;
    std::vector<IsaWire> wires;
    std::vector<std::vector<std::pair<std::string, std::vector<std::string>>>> paths;   // [domain] -> (mnemonic, wire ids)
    std::vector<std::string> errors;      // what could not be read ("line 12: ..."); the rest of the layout is still there

    IsaLayout() : paths(2) {}
    const IsaBox* box(const std::string& id) const;
    const IsaWire* wire(const std::string& id) const;
    // The wires lit by an instruction text ("MOV CONST(..) -> ..." of domain 0 / 1): the longest mnemonic that is the start of the text wins.
    std::vector<std::string> wiresFor(int domain, const std::string& instructionText) const;
    std::string mnemonicOf(int domain, const std::string& instructionText) const;
};

IsaLayout parseIsaLayout(const std::string& text);
const std::string& defaultIsaLayoutText();

// What a row shows now. `instrAt` gives the text of the instruction at a pc ("" if unknown). Without a view (nothing is being debugged) every value is "-".
std::string isaValue(const IsaSource& s, const SimView* view, const std::function<std::string(int domain, uint32_t pc)>& instrAt);

}  // namespace acide
