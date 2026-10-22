#include "blocks/block_shapes.h"

namespace acide {
namespace blocks {

const ShapeRegistry& ShapeRegistry::defaults() {
    static const ShapeRegistry r = [] {
        ShapeRegistry s;
        s.set("label", {Shape::Tag, false});
        s.set("comment", {Shape::Note, false});
        s.set("blank", {Shape::Spacer, false});
        s.set("raw", {Shape::Text, false});
        s.set("directive", {Shape::Statement, false});         // ".equ  A = 1": the fields say it all
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
