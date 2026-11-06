#include "eeprom_tool.h"

#include <map>

#include <QCheckBox>
#include <QCoreApplication>
#include <QDialog>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTextBlock>
#include <QTextCursor>
#include <QPointer>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include "asmcore/disasm.h"
#include "asmcore/eeprom.h"
#include "asmcore/sim.h"
#include "text_util.h"
#include "theme.h"

namespace acide {

namespace {

QString hex4(uint32_t v) { return QString("0x%1").arg(v, 4, 16, QChar('0')).toUpper().replace("0X", "0x"); }
QString hex2(uint32_t v) { return QString("%1").arg(v, 2, 16, QChar('0')).toUpper(); }

// The map file next to an image: PREFIX.eeprom.bin -> PREFIX.eeprom.json
std::string mapPathOf(const std::string& bin) {
    const size_t dot = bin.rfind(".bin");
    return (dot != std::string::npos && dot + 4 == bin.size() ? bin.substr(0, dot) : bin) + ".json";
}

class EepromTool : public QDialog {
    Q_DECLARE_TR_FUNCTIONS(EepromTool)
public:
    EepromTool(AppContext& ctx, QWidget* parent) : QDialog(parent, Qt::Window), ctx_(ctx) {
        setObjectName("eepromTool");
        setWindowTitle(tr("EEPROM"));
        auto* lay = new QVBoxLayout(this);
        tabs_ = new QTabWidget;
        tabs_->setObjectName("eepromTabs");
        tabs_->addTab(buildPage(), tr("Build the image"));
        tabs_->addTab(verifyPage(), tr("Verify an image"));
        tabs_->addTab(disasmPage(), tr("Disassemble an image"));
        lay->addWidget(tabs_);
        for (QPushButton* b : findChildren<QPushButton*>()) { b->setAutoDefault(false); b->setDefault(false); }    // (Enter belongs to the path boxes)
        conns_.add(ctx.snapshotChanged, [this](SnapshotPtr) {
            refreshBuild();
            // an action waiting for the build it asked for (the sources had changed): now with the new program
            const Pending p = pending_;
            pending_ = Pending::None;
            if (p == Pending::Write) writeImage();
            else if (p == Pending::Verify) verify();
        });
        refreshBuild();
        resize(900, 600);
    }
    void showPage(EepromPage p) { tabs_->setCurrentIndex((int)p); }

private:
    // ---- build -------------------------------------------------------------------------------------------------------------------------------------
    QWidget* buildPage() {
        auto* w = new QWidget;
        auto* l = new QVBoxLayout(w);
        buildInfo_ = new QLabel;
        buildInfo_->setObjectName("eepromBuildInfo");
        buildInfo_->setWordWrap(true);
        buildInfo_->setMinimumHeight(buildInfo_->fontMetrics().lineSpacing() * 2 + 4);              // (two lines at least: a squeezed label overlaps them)
        l->addWidget(buildInfo_);
        regions_ = new QTableWidget(0, 5);
        regions_->setObjectName("eepromRegions");
        regions_->setHorizontalHeaderLabels({tr("Region"), tr("Register"), tr("Start"), tr("Bytes"), tr("End")});
        regions_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        regions_->verticalHeader()->hide();
        regions_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        regions_->setFont(editorFont(ctx_.settings));
        l->addWidget(regions_, 1);
        auto* note = new QLabel(tr("The start addresses come from the profile of the project (eeprom_main_code_addr ... eeprom_peri_devmap_addr; -1 = right after the region before it)."));
        note->setWordWrap(true);
        note->setEnabled(false);
        l->addWidget(note);
        auto* row = new QHBoxLayout;
        row->addWidget(new QLabel(tr("Write to:")));
        outPath_ = new QLineEdit;
        outPath_->setObjectName("eepromOutPath");
        outPath_->setPlaceholderText(tr("PREFIX.eeprom.bin"));
        row->addWidget(outPath_, 1);
        auto* browse = new QPushButton(tr("Browse..."));
        row->addWidget(browse);
        l->addLayout(row);
        auto* buttons = new QHBoxLayout;
        auto* rebuild = new QPushButton(tr("Build the program"));
        rebuild->setObjectName("eepromRebuild");
        write_ = new QPushButton(tr("Write the EEPROM image"));
        write_->setObjectName("eepromWrite");
        buttons->addWidget(rebuild);
        buttons->addStretch(1);
        buttons->addWidget(write_);
        l->addLayout(buttons);
        written_ = new QLabel;
        written_->setObjectName("eepromWritten");
        written_->setWordWrap(true);
        written_->setMinimumHeight(written_->fontMetrics().lineSpacing() * 2 + 4);              // (two lines at least: a squeezed label overlaps them)
        written_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        l->addWidget(written_);
        connect(browse, &QPushButton::clicked, this, [this] {
            const QString f = QFileDialog::getSaveFileName(this, tr("Write the EEPROM image"), outPath_->text(), tr("EEPROM images (*.eeprom.bin)"));
            if (!f.isEmpty()) outPath_->setText(f.endsWith(".eeprom.bin") ? f : f + ".eeprom.bin");
        });
        connect(rebuild, &QPushButton::clicked, this, [this] { ctx_.requestBuild.fire(); });
        connect(write_, &QPushButton::clicked, this, [this] { writeImage(); });
        return w;
    }
    // the newest build: where its regions go (or why there is no image)
    void refreshBuild() {
        const SnapshotPtr s = ctx_.snapshot;
        regions_->setRowCount(0);
        image_.clear();
        if (!s) { buildInfo_->setText(tr("Nothing has been built yet: build the program first.")); write_->setEnabled(false); return; }
        if (!s->result.ok) { buildInfo_->setText(tr("The program has errors: fix them, then build again.")); write_->setEnabled(false); return; }
        std::string why;
        if (!asmcore::buildEeprom(s->result, s->profile, image_, map_, why)) {
            buildInfo_->setText(tr("The program does not fit the EEPROM: %1").arg(fromStd(why)));
            write_->setEnabled(false);
            return;
        }
        regions_->setRowCount(asmcore::ER_COUNT);
        for (int i = 0; i < asmcore::ER_COUNT; i++) {
            const asmcore::EepromRegion& g = map_.region[i];
            const QStringList cells = {fromStd(asmcore::eepromRegionName(i)), fromStd(asmcore::eepromRegisterName(i)), hex4(g.addr), QString::number(g.size), g.size ? hex4(g.addr + g.size - 1) : QString("-")};
            for (int c = 0; c < cells.size(); c++) regions_->setItem(i, c, new QTableWidgetItem(cells[c]));
        }
        const bool stale = s->revision != ctx_.workspace.revision();
        buildInfo_->setText(tr("%1 of %2 bytes of the EEPROM used \xC2\xB7 I2C address 0x%3 \xC2\xB7 %4-byte memory addresses%5")
                                .arg(image_.size()).arg(map_.capacity).arg(map_.devAddr, 2, 16, QChar('0')).arg(map_.addrBytes)
                                .arg(stale ? tr("\nThe sources changed since this build: build the program again to write what they say now.") : QString()));
        if (outPath_->text().isEmpty() || outPath_->property("auto").toBool()) {
            outPath_->setText(fromStd(s->prefix + ".eeprom.bin"));
            outPath_->setProperty("auto", true);
        }
        write_->setEnabled(true);
    }
    // The program is out of date (or not built): build it first and come back when it is done (the action waits in pending_). True if it has to wait.
    enum class Pending { None, Write, Verify };
    bool buildFirst(Pending what, QLabel* status) {
        const SnapshotPtr s = ctx_.snapshot;
        if (s && s->revision == ctx_.workspace.revision()) return false;
        pending_ = what;
        status->setStyleSheet(QString());
        status->setText(tr("The sources changed since the last build: building the program first..."));
        ctx_.requestBuild.fire();
        return true;
    }
    void writeImage() {
        if (pending_ == Pending::None && buildFirst(Pending::Write, written_)) return;
        if (image_.empty()) { written_->setText(buildInfo_->text()); return; }
        std::string bin = toStd(outPath_->text().trimmed());
        if (bin.empty()) return;
        if (bin.size() < 11 || bin.compare(bin.size() - 11, 11, ".eeprom.bin") != 0) bin += ".eeprom.bin";
        const std::string base = bin.substr(0, bin.size() - 4);
        auto* fs = ctx_.workspace.fileSystem().get();
        QStringList done;
        bool ok = true;
        auto put = [&](const std::string& p, const std::string& content) { if (fs->write(p, content)) done << fromStd(p); else ok = false; };
        put(bin, std::string(image_.begin(), image_.end()));
        put(base + ".hex", asmcore::intelHex(image_));
        put(base + ".json", map_.json());
        if (ctx_.snapshot) put(base + ".sym", asmcore::eepromSymbols(ctx_.snapshot->result).json());     // the names (keep it apart to pass the image on without them)
        written_->setText((ok ? tr("Written:") : tr("Could not write every file:")) + "\n" + done.join("\n"));
        for (const QString& f : done) ctx_.log.fire(tr("  wrote %1").arg(f));
    }

    // ---- verify ------------------------------------------------------------------------------------------------------------------------------------
    QWidget* verifyPage() {
        auto* w = new QWidget;
        auto* l = new QVBoxLayout(w);
        l->addWidget(new QLabel(tr("A burned image (e.g. read back from the board) against the newest build of the program: every byte of the six regions, then every instruction as the decoders read it.")));
        auto* row = new QHBoxLayout;
        verifyPath_ = new QLineEdit;
        verifyPath_->setObjectName("eepromVerifyPath");
        verifyPath_->setPlaceholderText(tr("the image (PREFIX.eeprom.bin; its .eeprom.json next to it is checked too)"));
        auto* browse = new QPushButton(tr("Browse..."));
        auto* run = new QPushButton(tr("Verify"));
        run->setObjectName("eepromVerify");
        row->addWidget(verifyPath_, 1);
        row->addWidget(browse);
        row->addWidget(run);
        l->addLayout(row);
        verdict_ = new QLabel;
        verdict_->setObjectName("eepromVerdict");
        verdict_->setWordWrap(true);
        verdict_->setMinimumHeight(verdict_->fontMetrics().lineSpacing() * 2 + 4);              // (two lines at least: a squeezed label overlaps them)
        l->addWidget(verdict_);
        diffs_ = new QTableWidget(0, 6);
        diffs_->setObjectName("eepromDiffs");
        diffs_->setHorizontalHeaderLabels({tr("What"), tr("Where"), tr("EEPROM"), tr("Should be"), tr("In the image"), tr("Source")});
        diffs_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
        diffs_->horizontalHeader()->setStretchLastSection(true);
        diffs_->verticalHeader()->hide();
        diffs_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        diffs_->setSelectionBehavior(QAbstractItemView::SelectRows);
        diffs_->setFont(editorFont(ctx_.settings));
        l->addWidget(diffs_, 1);
        connect(browse, &QPushButton::clicked, this, [this] { browseImage(verifyPath_); });
        connect(run, &QPushButton::clicked, this, [this] { verify(); });
        connect(verifyPath_, &QLineEdit::returnPressed, this, [this] { verify(); });
        connect(diffs_, &QTableWidget::cellDoubleClicked, this, [this](int r, int) {
            auto* it = diffs_->item(r, 1);
            if (!it || it->data(Qt::UserRole).isNull()) return;
            const asmcore::Domain d = it->data(Qt::UserRole + 1).toInt() == 1 ? asmcore::Domain::Peri : asmcore::Domain::Main;
            const DebugLocation loc = ctx_.debugger.locationForPc(d, it->data(Qt::UserRole).toUInt());
            if (loc.valid) { Location go; go.file = loc.file; go.line = loc.line; ctx_.goTo.fire(go); }
        });
        return w;
    }
    void verify() {
        diffs_->setRowCount(0);
        verdict_->setStyleSheet(QString());
        if (pending_ == Pending::None && buildFirst(Pending::Verify, verdict_)) return;
        const SnapshotPtr s = ctx_.snapshot;
        if (!s || !s->result.ok) { verdict_->setText(tr("Build the program first: the image is compared with it.")); return; }
        const std::string path = toStd(verifyPath_->text().trimmed());
        std::string bin, mapText, why;
        auto* fs = ctx_.workspace.fileSystem().get();
        if (path.empty() || !fs->read(path, bin)) { verdict_->setText(tr("Cannot read %1").arg(fromStd(path))); return; }
        asmcore::EepromMap imap;
        const bool haveMap = fs->read(mapPathOf(path), mapText) && asmcore::loadEepromMapJson(mapText, imap, why);
        asmcore::sim::SimConfig cfg;
        cfg.profile = s->profile;
        const std::vector<uint8_t> image(bin.begin(), bin.end());
        const asmcore::sim::EepromVerifyReport rep = asmcore::sim::verifyEepromImage(s->result, cfg, image, haveMap ? &imap : nullptr);
        QString text = rep.ok ? tr("\xE2\x9C\x94 The image is what the program burns: %1 bytes compared, none differs, every instruction decodes to its source.").arg(rep.bytesCompared)
                              : tr("\xE2\x9C\x98 The image is NOT what the program burns: %1 of %2 bytes differ, %3 instruction(s) decode to something else.").arg(rep.diffs.size()).arg(rep.bytesCompared).arg(rep.instructions.size());
        if (!haveMap) text += "\n" + tr("(no map next to the image: the regions are taken where the program places them)");
        for (const std::string& p : rep.problems) text += "\n" + tr("Problem: %1").arg(fromStd(p));
        verdict_->setText(text);
        verdict_->setStyleSheet(rep.ok ? "color: #2e9d4f;" : "color: #d04040;");
        // the instructions first (what matters), then the bytes
        int r = 0;
        diffs_->setRowCount((int)(rep.instructions.size() + std::min<size_t>(rep.diffs.size(), 2000)));
        auto put = [&](int c, const QString& t) { auto* it = new QTableWidgetItem(t); diffs_->setItem(r, c, it); return it; };
        for (const asmcore::sim::DecodedLine& L : rep.instructions) {
            put(0, tr("instruction"));
            auto* where = put(1, QString("%1 %2").arg(L.domain == asmcore::Domain::Main ? "Main" : "Peri", QString("%1").arg(L.pc, 3, 16, QChar('0')).toUpper()));
            where->setData(Qt::UserRole, L.pc);
            where->setData(Qt::UserRole + 1, L.domain == asmcore::Domain::Main ? 0 : 1);
            put(2, hex4(L.eepromAddr));
            put(3, fromStd(L.expected));
            put(4, fromStd(L.text) + (L.valid ? QString() : "    ; " + fromStd(L.problem)));
            put(5, fromStd(L.source));
            for (int c = 0; c < 6; c++) diffs_->item(r, c)->setBackground(QColor(208, 64, 64, 70));
            r++;
        }
        for (size_t i = 0; i < rep.diffs.size() && i < 2000; i++) {
            const asmcore::sim::EepromByteDiff& d = rep.diffs[i];
            put(0, tr("byte"));
            put(1, fromStd(asmcore::eepromRegionName(d.region)));
            put(2, hex4(d.addr));
            put(3, hex2(d.expected));
            put(4, d.missing ? tr("(the image has ended)") : hex2(d.actual));
            put(5, QString());
            r++;
        }
        diffs_->resizeColumnsToContents();
    }

    // ---- disassemble -------------------------------------------------------------------------------------------------------------------------------
    QWidget* disasmPage() {
        auto* w = new QWidget;
        auto* l = new QVBoxLayout(w);
        l->addWidget(new QLabel(tr("A burned image back to assembler source: the code, descriptor tables and const pool / DeviceMap of each domain are read from the regions of its map.")));
        auto* row = new QHBoxLayout;
        disasmPath_ = new QLineEdit;
        disasmPath_->setObjectName("eepromDisasmPath");
        disasmPath_->setPlaceholderText(tr("the image (PREFIX.eeprom.bin with its .eeprom.json)"));
        auto* browse = new QPushButton(tr("Browse..."));
        auto* run = new QPushButton(tr("Disassemble"));
        run->setObjectName("eepromDisassemble");
        annotate_ = new QCheckBox(tr("addresses and bytes as comments"));
        annotate_->setObjectName("eepromAnnotate");
        useNames_ = new QCheckBox(tr("names from the symbol file"));
        useNames_->setObjectName("eepromUseNames");
        useNames_->setChecked(true);
        useNames_->setToolTip(tr("PREFIX.eeprom.sym next to the image: labels, CSRs, services, devices (and the const pool symbols as comments)"));
        strict_ = new QCheckBox(tr("strict (a warning fails it too)"));
        strict_->setObjectName("eepromStrict");
        row->addWidget(disasmPath_, 1);
        row->addWidget(browse);
        row->addWidget(run);
        l->addLayout(row);
        auto* opts = new QHBoxLayout;
        opts->addWidget(useNames_);
        opts->addWidget(annotate_);
        opts->addWidget(strict_);
        opts->addStretch(1);
        l->addLayout(opts);
        disasmInfo_ = new QLabel;
        disasmInfo_->setObjectName("eepromDisasmInfo");
        disasmInfo_->setWordWrap(true);
        disasmInfo_->setMinimumHeight(disasmInfo_->fontMetrics().lineSpacing() * 2 + 4);              // (two lines at least: a squeezed label overlaps them)
        l->addWidget(disasmInfo_);
        source_ = new QPlainTextEdit;
        source_->setObjectName("eepromSource");
        source_->setReadOnly(true);
        source_->setFont(editorFont(ctx_.settings));
        source_->setLineWrapMode(QPlainTextEdit::NoWrap);
        l->addWidget(source_, 3);
        // what the checks found: the decoder (bytes that are no legal encoding), the specification (the assembler's rules), the identity (the same image again)
        checks_ = new QTableWidget(0, 4);
        checks_->setObjectName("eepromChecks");
        checks_->setHorizontalHeaderLabels({tr("Check"), tr("Code"), tr("Line"), tr("What")});
        checks_->horizontalHeader()->setStretchLastSection(true);
        checks_->verticalHeader()->hide();
        checks_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        checks_->setSelectionBehavior(QAbstractItemView::SelectRows);
        l->addWidget(checks_, 1);
        connect(checks_, &QTableWidget::cellClicked, this, [this](int r, int) {        // to the line of the disassembly it is about
            auto* it = checks_->item(r, 2);
            const int line = it ? it->data(Qt::UserRole).toInt() : 0;
            if (line <= 0) return;
            QTextCursor c(source_->document()->findBlockByNumber(line - 1));
            source_->setTextCursor(c);
            source_->centerCursor();
            source_->setFocus();
        });
        auto* buttons = new QHBoxLayout;
        save_ = new QPushButton(tr("Save as .asm and open it..."));
        save_->setObjectName("eepromSaveAsm");
        save_->setEnabled(false);
        buttons->addStretch(1);
        buttons->addWidget(save_);
        l->addLayout(buttons);
        connect(browse, &QPushButton::clicked, this, [this] { browseImage(disasmPath_); });
        connect(run, &QPushButton::clicked, this, [this] { disassemble(); });
        connect(disasmPath_, &QLineEdit::returnPressed, this, [this] { disassemble(); });
        for (QCheckBox* c : {annotate_, useNames_, strict_}) connect(c, &QCheckBox::toggled, this, [this] { if (!source_->toPlainText().isEmpty()) disassemble(); });
        connect(save_, &QPushButton::clicked, this, [this] {
            QString f = QFileDialog::getSaveFileName(this, tr("Save the disassembly"), fromStd(mapPathOf(toStd(disasmPath_->text().trimmed()))).replace(".eeprom.json", ".disasm.asm"), tr("Assembler source (*.asm)"));
            if (f.isEmpty()) return;
            if (!f.endsWith(".asm")) f += ".asm";
            saveDisassembly(toStd(f));
        });
        return w;
    }
public:
    // (also for the tests: the file dialog is not part of it)
    bool saveDisassembly(const std::string& path) {
        if (!ctx_.workspace.fileSystem()->write(path, toStd(source_->toPlainText()))) { disasmInfo_->setText(tr("Cannot write %1").arg(fromStd(path))); return false; }
        ctx_.log.fire(tr("  wrote %1").arg(fromStd(path)));
        Location go;
        go.file = path;
        go.line = 1;
        ctx_.goTo.fire(go);
        return true;
    }
private:
    void disassemble() {
        source_->clear();
        checks_->setRowCount(0);
        save_->setEnabled(false);
        disasmInfo_->setStyleSheet(QString());
        const std::string path = toStd(disasmPath_->text().trimmed());
        std::string bin, mapText, symText, why;
        auto* fs = ctx_.workspace.fileSystem().get();
        if (path.empty() || !fs->read(path, bin)) { disasmInfo_->setText(tr("Cannot read %1").arg(fromStd(path))); return; }
        asmcore::EepromMap map;
        if (!fs->read(mapPathOf(path), mapText) || !asmcore::loadEepromMapJson(mapText, map, why)) {
            disasmInfo_->setText(tr("Its map %1 cannot be read%2: without it the regions are not known (asmc --eeprom and the build write it next to the image).")
                                     .arg(fromStd(mapPathOf(path)), why.empty() ? QString() : " (" + fromStd(why) + ")"));
            return;
        }
        const std::string symPath = mapPathOf(path).substr(0, mapPathOf(path).size() - 5) + ".sym";
        asmcore::EepromSymbols syms;
        bool haveSyms = false;
        QString namesNote;
        if (useNames_->isChecked() && fs->read(symPath, symText)) {
            haveSyms = asmcore::loadEepromSymbolsJson(symText, syms, why);
            namesNote = haveSyms ? tr("Names from %1.").arg(QFileInfo(fromStd(symPath)).fileName()) : tr("The symbol file cannot be read (%1): numbers and L_xxxx labels.").arg(fromStd(why));
        } else {
            namesNote = useNames_->isChecked() ? tr("No symbol file (%1): numbers and L_xxxx labels.").arg(QFileInfo(fromStd(symPath)).fileName()) : tr("Without names: numbers and L_xxxx labels.");
        }
        const asmcore::Profile pf = ctx_.snapshot ? ctx_.snapshot->profile : asmcore::Profile();
        const std::vector<uint8_t> image(bin.begin(), bin.end());
        asmcore::EepromDisasmCheck c;
        if (!asmcore::checkEepromDisassembly(image, map, pf, haveSyms ? &syms : nullptr, annotate_->isChecked(), c, why)) { disasmInfo_->setText(fromStd(why)); return; }
        const std::string head = "; disassembled from " + path + "\n";
        source_->setPlainText(fromStd(head + c.source));
        // the findings: decode, specification, identity
        const bool strict = strict_->isChecked();
        int r = 0;
        auto row = [&](const QString& check, const QString& code, int line, const QString& what, bool bad) {
            checks_->setRowCount(r + 1);
            checks_->setItem(r, 0, new QTableWidgetItem(check));
            checks_->setItem(r, 1, new QTableWidgetItem(code));
            auto* li = new QTableWidgetItem(line > 0 ? QString::number(line) : QString());
            li->setData(Qt::UserRole, line);
            checks_->setItem(r, 2, li);
            checks_->setItem(r, 3, new QTableWidgetItem(what));
            if (bad) for (int k = 0; k < 4; k++) checks_->item(r, k)->setBackground(QColor(208, 64, 64, 70));
            r++;
        };
        const int lineOffset = 1;                                                   // (the "disassembled from" line in front of the text)
        for (const asmcore::DisasmIssue& i : c.decodeIssues)                      // (a decoder finding names the pc it is about; it has no line of the text)
            row(i.fatal ? tr("decode error") : tr("decode remark"), fromStd(i.code), 0, tr("pc %1: %2").arg(i.pc).arg(fromStd(i.msg)), i.fatal || strict);
        for (const asmcore::Diagnostic& d : c.specDiags) {
            const bool err = d.sev == asmcore::Diagnostic::Error;
            row(err ? tr("spec error") : d.sev == asmcore::Diagnostic::Warning ? tr("spec warning") : tr("spec note"), fromStd(d.code), d.loc.line > 0 ? d.loc.line + lineOffset : 0, fromStd(d.msg),
                err || (strict && d.sev == asmcore::Diagnostic::Warning));
        }
        if (c.assembles && !c.identical)
            row(tr("identity"), QString(), 0, tr("It assembles to ANOTHER image: %1 byte(s) differ, the first in %2 at %3").arg(c.differentBytes).arg(fromStd(asmcore::eepromRegionName(c.differentRegion))).arg(hex4(c.firstDifference)), true);
        checks_->resizeColumnsToContents();
        const bool ok = c.ok(strict);
        QString verdict = ok ? tr("\xE2\x9C\x94 A program of the specification: every byte decodes, the assembler finds no error%1, and it assembles to the identical image.").arg(strict ? tr(" and no warning") : QString())
                             : tr("\xE2\x9C\x98 NOT a correct program: %1 decode error(s), %2 spec error(s), %3 warning(s)%4.").arg(c.fatalDecode()).arg(c.errors()).arg(c.warnings())
                                   .arg(!c.assembles ? tr(", it does not assemble") : !c.identical ? tr(", it assembles to another image") : QString());
        verdict += "\n" + tr("Main: %1 bytes of code at %2 \xC2\xB7 Peri: %3 bytes of code at %4 \xC2\xB7 %5").arg(map.codeSize[0]).arg(hex4(map.region[asmcore::ER_MAIN_CODE].addr))
                               .arg(map.codeSize[1]).arg(hex4(map.region[asmcore::ER_PERI_CODE].addr)).arg(namesNote);
        disasmInfo_->setText(verdict);
        disasmInfo_->setStyleSheet(ok ? "color: #2e9d4f;" : "color: #d04040;");
        save_->setEnabled(true);
    }

    void browseImage(QLineEdit* into) {
        const std::string dir = ctx_.workspace.hasProject() ? ctx_.workspace.project().absolute(ctx_.workspace.project().outputDir) : std::string();
        const QString f = QFileDialog::getOpenFileName(this, tr("An EEPROM image"), into->text().isEmpty() ? fromStd(dir) : into->text(), tr("EEPROM images (*.eeprom.bin);;All files (*)"));
        if (!f.isEmpty()) into->setText(f);
    }

    AppContext& ctx_;
    QTabWidget* tabs_;
    QLabel *buildInfo_, *written_, *verdict_, *disasmInfo_;
    QTableWidget *regions_, *diffs_;
    QLineEdit *outPath_, *verifyPath_, *disasmPath_;
    QPushButton *write_, *save_;
    QCheckBox* annotate_;
    QPlainTextEdit* source_;
    std::vector<uint8_t> image_;
    asmcore::EepromMap map_;
    Pending pending_ = Pending::None;
    QCheckBox *useNames_, *strict_;
    QTableWidget* checks_;
    ConnectionBag conns_;
};

std::map<AppContext*, QPointer<EepromTool>>& tools() { static std::map<AppContext*, QPointer<EepromTool>> m; return m; }

}  // namespace

QWidget* showEepromTool(AppContext& ctx, QWidget* parent, EepromPage page) {
    QPointer<EepromTool>& t = tools()[&ctx];
    if (!t) t = new EepromTool(ctx, parent ? parent->window() : nullptr);
    t->showPage(page);
    t->show();
    t->raise();
    t->activateWindow();
    return t;
}

bool saveEepromDisassembly(AppContext& ctx, const std::string& path) {
    auto it = tools().find(&ctx);
    return it != tools().end() && it->second && it->second->saveDisassembly(path);
}

QWidget* eepromToolWindow(AppContext& ctx) {
    auto it = tools().find(&ctx);
    return it == tools().end() ? nullptr : it->second.data();
}

}  // namespace acide
