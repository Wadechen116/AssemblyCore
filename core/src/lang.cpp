// Language service, part 1: vocabulary and tokenizer. See include/asmcore/lang.h.
#include "asmcore/lang.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "isa.h"
#include "util.h"

namespace asmcore {
namespace lang {

const char* tokName(Tok t) {
    switch (t) {
        case Tok::Whitespace: return "whitespace";
        case Tok::Comment: return "comment";
        case Tok::Label: return "label";
        case Tok::Directive: return "directive";
        case Tok::Mnemonic: return "mnemonic";
        case Tok::SubOp: return "subop";
        case Tok::Region: return "region";
        case Tok::Condition: return "condition";
        case Tok::ArgName: return "argname";
        case Tok::Number: return "number";
        case Tok::String: return "string";
        case Tok::Symbol: return "symbol";
        case Tok::Operator: return "operator";
        case Tok::Punct: return "punct";
        case Tok::Unknown: return "unknown";
    }
    return "unknown";
}

void Language::add(const Entry& e) {
    Entry x = e;
    x.name = upper(x.name);
    entries_.push_back(x);
}

const Entry* Language::find(Tok kind, const std::string& name, Dom d) const {
    std::string n = upper(name);
    if (kind == Tok::Region) {                          // the aliases map to the canonical entry
        const isa::RegionInfo* r = isa::regionByName(n);
        if (r) n = r->name;
    }
    for (const Entry& e : entries_) {
        if (e.kind != kind || e.name != n) continue;
        if (d == Dom::Main && !(e.domains & kMain)) continue;
        if (d == Dom::Peri && !(e.domains & kPeri)) continue;
        return &e;
    }
    return nullptr;
}

std::vector<const Entry*> Language::all(Tok kind, Dom d) const {
    std::vector<const Entry*> out;
    for (const Entry& e : entries_) {
        if (e.kind != kind) continue;
        if (d == Dom::Main && !(e.domains & kMain)) continue;
        if (d == Dom::Peri && !(e.domains & kPeri)) continue;
        out.push_back(&e);
    }
    return out;
}

Language Language::defaults(const Profile& pf) {
    Language L;
    auto add = [&](const char* name, Tok k, int dom, const char* sig, const char* doc, const char* spec, const char* ins = "", const char* group = "") {
        Entry e;
        e.name = name; e.kind = k; e.domains = dom; e.signature = sig; e.doc = doc; e.spec = spec; e.insert = ins; e.group = group;
        L.add(e);
    };
    // ---- instructions
    add("MOV", Tok::Mnemonic, kMain, "MOV src -> dst", "Copy bytes between the Main data regions. Both sides must have the same length.", "5");
    add("ALU", Tok::Mnemonic, kMain, "ALU op [signed|unsigned]", "Operate on ALU_OPA / ALU_OPB; the result is in ALU_OUT / ALU_STATUS.", "6");
    add("EXEC_CSR", Tok::Mnemonic, kBoth, "EXEC_CSR op csr, value", "Read or write a CSR byte, or move a buffer to or from consecutive CSR addresses (Main).", "4");
    add("BR.COND", Tok::Mnemonic, kMain, "BR.COND cond, label [, safe]", "Branch to label when the condition holds.", "7.2");
    add("JMP", Tok::Mnemonic, kBoth, "JMP label [, safe]", "Unconditional branch. 'safe' makes it an event checkpoint.", "7.2");
    add("BR.ERET", Tok::Mnemonic, kBoth, "BR.ERET", "Return from an event handler to the saved resume PC (handler code only).", "16.5");
    add("SAFE", Tok::SubOp, kBoth, "safe", "Marks an ordinary branch as an event checkpoint (event_safe = 1); events are dispatched only at such branches.", "16.3", "safe", "branchattr");
    add("CASCADE_BEGIN", Tok::Mnemonic, kMain, "CASCADE_BEGIN k, block0", "Start a cascade: jump to the block chosen by the value k.", "7.3");
    add("CASCADE_END", Tok::Mnemonic, kMain, "CASCADE_END i, exit", "End block i of a cascade and continue at exit.", "7.3");
    add("CALL", Tok::Mnemonic, kBoth, "CALL proc", "Call a procedure (inline or software return, see --call-model).", "6 (ASM_SYNTAX)");
    add("RET", Tok::Mnemonic, kBoth, "RET", "Return from a procedure.", "6 (ASM_SYNTAX)");
    add("EXEC_PUB", Tok::Mnemonic, kPeri, "EXEC_PUB service=S, src=N, len=L", "Publish PERI_RX_BUF[src..src+len-1] to the Main result slot as service S.", "11.3");
    add("EXEC_XFER", Tok::Mnemonic, kPeri, "EXEC_XFER READ|WRITE dev=D, ...", "Start an I2C transfer on the device D of the DeviceMap.", "9");
    for (const isa::NamedCode& c : isa::kPeriConds) L.add([&] {
        Entry e; e.name = std::string("BR.") + c.name; e.kind = Tok::Mnemonic; e.domains = kPeri;
        e.signature = e.name + " label"; e.doc = std::string("Peri branch on ") + c.name + "."; e.spec = "11.2"; return e; }());
    // ---- directives
    add(".MAIN", Tok::Directive, kBoth, ".main", "The following statements belong to the Main sequencer.", "");
    add(".PERI", Tok::Directive, kBoth, ".peri", "The following statements belong to the Peri sequencer.", "");
    add(".INCLUDE", Tok::Directive, kBoth, ".include \"file\"", "Include another source file.", "");
    add(".EQU", Tok::Directive, kBoth, ".equ NAME = expr", "Define a constant.", "");
    add("EQU", Tok::Directive, kBoth, "NAME EQU expr", "Define a constant (8051 style).", "");
    add(".CSR", Tok::Directive, kBoth, ".csr NAME = addr", "Name a 12-bit CSR address.", "4");
    add(".SERVICE", Tok::Directive, kBoth, ".service NAME = id", "Name a service id (1..15) shared by Main and Peri.", "12");
    add(".CONST", Tok::Directive, kMain, ".const NAME db|dw v, ...", "Constants placed in MAIN_CONST_POOL.", "5");
    add(".DEVMAP", Tok::Directive, kBoth, ".devmap i, I2C, addr7 [, name]", "Entry i of the DeviceMap.", "9");
    add(".VECTOR", Tok::Directive, kBoth, ".vector EVENT_ID, label", "Entry of the event handler for this id in the current domain. The handler must end in BR.ERET (spec v31 16).", "16");
    add(".PROC", Tok::Directive, kBoth, ".proc NAME", "Begin a procedure.", "");
    add(".ENDP", Tok::Directive, kBoth, ".endp", "End a procedure.", "");
    add(".WCET", Tok::Directive, kMain, ".wcet NAME, from, to", "Register a worst-case-time query between two labels.", "");
    add(".ASSUME", Tok::Directive, kBoth, ".assume REGION[a..b] defined", "Tell the data-flow check these bytes are initialised elsewhere.", "");
    add(".END", Tok::Directive, kBoth, ".end", "Stop reading this file.", "");
    // ---- regions
    for (const isa::RegionInfo& r : isa::kRegions) {
        Entry e;
        e.name = r.name; e.kind = Tok::Region; e.domains = r.name == std::string("PERI_RX_BUF") ? kPeri : kMain;
        e.signature = std::string(r.name) + (r.size ? "[a..b]" : "[@const]");
        e.doc = std::string(r.readable ? "readable" : "not readable") + ", " + (r.writable ? "writable" : "not writable") +
                (r.size ? ", " + std::to_string(r.size) + " bytes" : "");
        e.spec = "5.1";
        L.add(e);
    }
    { Entry e; e.name = "PERI_RX_BUF"; e.kind = Tok::Region; e.domains = kPeri; e.signature = "PERI_RX_BUF[a..b]"; e.doc = "Bytes read by EXEC_XFER; source of EXEC_PUB."; e.spec = "9"; L.add(e); }
    // ---- conditions
    for (const isa::NamedCode& c : isa::kMainConds) {
        if (c.code == 0xE && !pf.profileCond0) continue;
        add(c.name, Tok::Condition, kMain, c.name, "Main branch condition.", "7.2");
    }
    // ---- sub-operations
    const char* aluOps[] = {"ADD", "SUB", "MUL", "INC", "DEC", "AND", "OR", "XOR", "NOT", "SHL", "SHR", "CMP.EQ", "CMP.NE", "CMP.LT", "CMP.GE", "EQ", "NE", "LT", "GE"};
    for (const char* n : aluOps) add(n, Tok::SubOp, kMain, n, "ALU operation.", "6.3", "", "alu");
    add("SIGNED", Tok::SubOp, kMain, "signed", "Treat the operands as signed.", "6.3", "signed", "alumod");
    add("UNSIGNED", Tok::SubOp, kMain, "unsigned", "Treat the operands as unsigned.", "6.3", "unsigned", "alumod");
    for (const char* n : {"WRITE8", "SET8", "CLR8"}) add(n, Tok::SubOp, kBoth, n, "One-byte CSR operation.", "4", "", "csr");
    for (const char* n : {"READ8", "READ_BUF"}) add(n, Tok::SubOp, kMain, n, "CSR read into MAIN_CSR_DATA_BUF (Main).", "4", "", "csr");
    add("WRITE_SP", Tok::SubOp, kMain, "WRITE_SP csr, len=N, ptr=P", "Write MAIN_SCRATCHPAD[ptr..] to consecutive CSR bytes (Main).", "4.3", "", "csr");
    add("READ", Tok::SubOp, kPeri, "READ", "I2C read.", "9", "", "xfer");
    add("WRITE", Tok::SubOp, kPeri, "WRITE", "I2C write.", "9", "", "xfer");
    // ---- named arguments
    for (const char* n : {"LEN", "PTR", "DEV", "ADDR", "ADDRLEN", "DATA", "EXT", "SERVICE", "SRC", "COND_SEL", "TARGET"}) {
        std::string lower = n;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return (char)std::tolower(ch); });
        add(n, Tok::ArgName, kBoth, (lower + "=").c_str(), "Named operand.", "", (lower + "=").c_str());
    }
    return L;
}

namespace {

bool isNumberStart(const std::string& s, size_t i) { return std::isdigit((unsigned char)s[i]) != 0; }

// 0x7F, 7Fh, 0b101, 101b, decimal: scan a word starting with a digit; returns its end.
size_t scanNumber(const std::string& s, size_t i) {
    size_t j = i;
    while (j < s.size() && std::isalnum((unsigned char)s[j])) j++;
    return j;
}

}  // namespace

std::vector<Token> Language::tokenizeLine(const std::string& line, Dom& dom, const std::set<std::string>* known) const {
    std::vector<Token> out;
    auto push = [&](size_t b, size_t e, Tok k) { if (e > b) out.push_back(Token{(int)b, (int)(e - b), k}); };
    const size_t n = line.size();
    size_t i = 0;
    bool stmtStart = true;           // the next word starts a statement (after optional labels)
    std::string mnemonic;            // upper case, once seen
    bool afterEquSet = false;
    while (i < n) {
        char ch = line[i];
        if (ch == ' ' || ch == '\t') { size_t j = i; while (j < n && (line[j] == ' ' || line[j] == '\t')) j++; push(i, j, Tok::Whitespace); i = j; continue; }
        if (ch == ';') { push(i, n, Tok::Comment); break; }
        if (ch == '"' || ch == '\'') {
            size_t j = i + 1;
            while (j < n && line[j] != ch) j++;
            if (j < n) j++;
            push(i, j, ch == '"' ? Tok::String : Tok::Number);     // 'A' is a number
            stmtStart = false;
            i = j;
            continue;
        }
        if (ch == '-' && i + 1 < n && line[i + 1] == '>') { push(i, i + 2, Tok::Operator); i += 2; continue; }
        if (ch == '.' && i + 1 < n && line[i + 1] == '.') { push(i, i + 2, Tok::Operator); i += 2; continue; }       // the range '..'
        if (isNumberStart(line, i)) { size_t j = scanNumber(line, i); push(i, j, Tok::Number); stmtStart = false; i = j; continue; }
        if (isIdentStart(ch)) {
            size_t j = i;
            while (j < n && isIdentChar(line[j])) j++;
            std::string word = line.substr(i, j - i), up = upper(word);
            size_t k = j;
            while (k < n && (line[k] == ' ' || line[k] == '\t')) k++;
            bool colon = k < n && line[k] == ':' && !(k + 1 < n && line[k + 1] == ':');
            Tok kind = Tok::Unknown;
            if (stmtStart && colon) {
                kind = Tok::Label;                                   // the ':' itself is punctuation
            } else if (stmtStart) {
                stmtStart = false;
                if (!word.empty() && word[0] == '.') {
                    kind = find(Tok::Directive, up) ? Tok::Directive : Tok::Unknown;
                    mnemonic = up;
                    if (up == ".MAIN") dom = Dom::Main;
                    else if (up == ".PERI") dom = Dom::Peri;
                } else if (find(Tok::Mnemonic, up, Dom::None)) {
                    kind = Tok::Mnemonic;
                    mnemonic = up;
                } else {
                    // "NAME EQU expr": the name comes first
                    size_t m = k, mEnd = k;
                    while (mEnd < n && isIdentChar(line[mEnd])) mEnd++;
                    if (mEnd > m && upper(line.substr(m, mEnd - m)) == "EQU") { kind = Tok::Symbol; afterEquSet = true; mnemonic = "EQU"; }
                    else kind = Tok::Unknown;
                }
            } else if (afterEquSet && up == "EQU") {
                kind = Tok::Directive;
                afterEquSet = false;
            } else {
                // operand word
                bool named = k < n && line[k] == '=' && !(k + 1 < n && line[k + 1] == '=');
                if (named && find(Tok::ArgName, up)) kind = Tok::ArgName;
                else if (find(Tok::Region, up) || isa::regionByName(up)) kind = Tok::Region;
                else if (find(Tok::Condition, up, dom) || find(Tok::SubOp, up, dom)) kind = find(Tok::Condition, up, dom) ? Tok::Condition : Tok::SubOp;
                else if (up == "CONST" || up == "DB" || up == "DW" || up == "I2C" || up == "DEFINED") kind = Tok::SubOp;
                else if (!known || known->count(word)) kind = Tok::Symbol;
                else kind = Tok::Unknown;
            }
            push(i, j, kind);
            if (kind == Tok::Label) {                                // label word, then the colon, then maybe another label
                size_t c = k;
                push(j, c, Tok::Whitespace);
                push(c, c + 1, Tok::Punct);
                i = c + 1;
                stmtStart = true;
                continue;
            }
            i = j;
            continue;
        }
        // operators and punctuation
        static const char* ops = "+-*/%<>&|^~=!";
        push(i, i + 1, std::strchr(ops, ch) ? Tok::Operator : Tok::Punct);
        // << and >> as one token
        if ((ch == '<' || ch == '>') && i + 1 < n && line[i + 1] == ch) { out.back().len = 2; i += 2; continue; }
        i++;
        stmtStart = false;
    }
    return out;
}

Language::Cursor Language::cursorAt(const std::string& line, int col, Dom dom) const {
    Cursor c;
    c.dom = dom;
    if (col < 0) col = 0;
    if ((size_t)col > line.size()) col = (int)line.size();
    std::string head = stripComment(line.substr(0, (size_t)col));
    size_t b = head.size();
    while (b > 0 && isIdentChar(head[b - 1])) b--;
    c.wordPrefix = head.substr(b);
    Dom d = dom;
    std::vector<Token> toks = tokenizeLine(head, d, nullptr);
    bool sawMnemonic = false;
    for (const Token& t : toks) {
        if (t.kind == Tok::Whitespace || t.kind == Tok::Comment) continue;
        if (t.kind == Tok::Label) continue;
        if (t.col + t.len >= (int)head.size() && !c.wordPrefix.empty() && t.col == (int)b) break;    // the word under the cursor
        if (!sawMnemonic && (t.kind == Tok::Mnemonic || t.kind == Tok::Directive)) {
            c.mnemonic = upper(head.substr((size_t)t.col, (size_t)t.len));
            sawMnemonic = true;
            c.operand = 0;
            continue;
        }
        if (!sawMnemonic) continue;
        const char first = head[(size_t)t.col];
        if (first == ',' || (t.kind == Tok::Operator && head.compare((size_t)t.col, 2, "->") == 0)) { c.operand++; c.word = 0; c.afterEquals = false; }
        else if (t.kind == Tok::Operator && first == '=') c.afterEquals = true;
        else if (t.kind != Tok::Punct && t.kind != Tok::Operator) { c.word++; c.afterEquals = false; }
    }
    c.atStatementStart = !sawMnemonic;
    if (c.atStatementStart) c.operand = -1;
    return c;
}

}  // namespace lang
}  // namespace asmcore
