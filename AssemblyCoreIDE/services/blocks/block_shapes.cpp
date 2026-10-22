#include "blocks/block_shapes.h"

namespace acide {
namespace blocks {

PlugShape plugShapeOf(const std::string& t) {
    if (t.empty()) return PlugShape::None;
    if (t == "int") return PlugShape::Round;
    if (t.compare(0, 7, "region:") == 0) return PlugShape::Point;
    if (t == "op.alu" || t == "sign" || t.compare(0, 5, "cond.") == 0) return PlugShape::Square;
    if (t.compare(0, 6, "label.") == 0) return PlugShape::Flag;
    return PlugShape::Diamond;                                    // csr, service, dev, ...
}

PlugShape plugShapeOfSocket(const std::vector<std::string>& accepts) {
    PlugShape s = PlugShape::None;
    for (const std::string& a : accepts) {
        const PlugShape p = plugShapeOf(a);
        if (s == PlugShape::None) s = p;
        else if (s != p) return PlugShape::Any;
    }
    return s;
}

const ShapeRegistry& ShapeRegistry::defaults() {
    static const ShapeRegistry r = [] {
        ShapeRegistry s;
        s.set("label", {Shape::Tag, false});
        s.set("comment", {Shape::Note, false});
        s.set("blank", {Shape::Spacer, false});
        s.set("raw", {Shape::Text, false});
        s.set("directive", {Shape::Statement, false});         // ".equ  A = 1": the fields say it all
        for (const char* t : {"num", "name", "expr_bin", "expr_un", "expr_paren", "label_ref", "proc_ref", "alu_op", "sign_mode", "cond_main_c", "cond_peri_c", "csr_ref", "service_ref", "dev_ref"})
            s.set(t, {Shape::Value, false});                   // components show only their parts
        return s;
    }();
    return r;
}

ShapeInfo ShapeRegistry::infoFor(const asmcore::blocks::BlockDef& def) const {
    auto it = byType_.find(def.type);
    if (it != byType_.end()) return it->second;
    ShapeInfo i;
    if (!def.valueType.empty()) i.shape = Shape::Value;
    else if (!def.stacks.empty()) i.shape = Shape::Container;
    return i;
}

}  // namespace blocks
}  // namespace acide
