#pragma once
// Shared by blocks.cpp, blocks_import.cpp and blocks_edit.cpp; not part of the public interface.
#include <string>
#include <vector>

#include "asmcore/blocks.h"
#include "json_lite.h"

namespace asmcore {
namespace blocks {
namespace detail {

json::Value blockToJson(const Block& b);
bool blockFromJson(const json::Value& v, Block& b, const std::string& where, std::string& err);
// give a block and everything inside it fresh ids from `doc`
void reid(Block& b, Document& doc);
// all lines of `text` as blocks, section headers dropped (several sections are put together); ids from `target`
std::vector<Block> importFlat(Document& target, const std::string& text, ImportStats* stats);

}  // namespace detail
}  // namespace blocks
}  // namespace asmcore
