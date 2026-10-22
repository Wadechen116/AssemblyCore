#pragma once
// Shared by blocks.cpp, blocks_import.cpp and blocks_edit.cpp; not part of the public interface.
#include <string>
#include <vector>

#include "asmcore/blocks.h"
#include "asmcore/lang.h"
#include "json_lite.h"

namespace asmcore {
namespace blocks {
namespace detail {

json::Value blockToJson(const Block& b);
bool blockFromJson(const json::Value& v, Block& b, const std::string& where, std::string& err);
// give a block and everything inside it fresh ids from `doc`
void reid(Block& b, Document& doc);
// the text of a template for one block ({field}, {?flag:text}, [optional group]); components and mirrors are taken into account
std::string renderTemplate(const std::string& tmpl, const Block& b, const Registry& reg);
// the value blocks, the sockets of the instructions (blocks_values.cpp)
void addComponentDefs(Registry& r, const lang::Language& L);
// structure problems of a component block (numbers out of range, regions ...), appended to `out`
void componentProblems(const Block& b, const BlockDef& d, const Registry& reg, std::vector<BlockProblem>& out);
// all component problems of a block and everything plugged into it (used to see what a new plug breaks)
std::vector<BlockProblem> subtreeProblems(const Block& host, const Registry& reg);
// problems of one component plugged into one socket (a constant too wide for CONST, a float where an integer is needed ...)
void plugProblems(const Block& parent, const BlockDef& pd, const InputDef& in, const Block& child, const Registry& reg, std::vector<BlockProblem>& out);
// all lines of `text` as blocks, section headers dropped (several sections are put together); ids from `target`
std::vector<Block> importFlat(Document& target, const std::string& text, ImportStats* stats);

}  // namespace detail
}  // namespace blocks
}  // namespace asmcore
