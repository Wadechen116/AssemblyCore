// md2pdf: a Markdown file becomes a PDF (A4), with Qt only (QTextDocument reads the Markdown, QPdfWriter writes the pages).
//   md2pdf INPUT.md OUTPUT.pdf [--title "Title"] [--font-size POINTS]
// Tables, headings, lists, code blocks, images and the inline marks of GitHub Markdown are supported; the text may be Chinese (a CJK font of the
// system is used). Images (![caption](file.png), paths relative to the Markdown file) are scaled to the width of the page. A table that would be cut
// by the end of a page starts on the next page instead (with the caption line or heading right above it), and a heading is never left alone at the
// bottom of a page.
#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>
#include <vector>
#include <cstdio>
#include <cstring>

#include <QAbstractTextDocumentLayout>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QRegularExpression>
#include <QTextImageFormat>
#include <QUrl>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QMarginsF>
#include <QPageLayout>
#include <QPageSize>
#include <QPdfWriter>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextTable>
#include <QTextTableFormat>

namespace {

// The first of these fonts that the system has: one with Chinese characters first.
QString pickFamily(const QStringList& wanted) {
    const QStringList have = QFontDatabase::families();
    for (const QString& w : wanted)
        for (const QString& h : have)
            if (h.compare(w, Qt::CaseInsensitive) == 0) return h;
    return QString();
}

void styleTables(QTextDocument& doc) {
    for (QTextFrame* f : doc.rootFrame()->childFrames()) {
        QTextTable* t = qobject_cast<QTextTable*>(f);
        if (!t) continue;
        QTextTableFormat tf = t->format();
        tf.setBorder(0.6);
        tf.setBorderBrush(QColor("#9aa4b2"));
        tf.setBorderStyle(QTextFrameFormat::BorderStyle_Solid);
        tf.setCellPadding(4);
        tf.setCellSpacing(0);
        tf.setWidth(QTextLength(QTextLength::PercentageLength, 100));
        t->setFormat(tf);
        for (int c = 0; c < t->columns(); c++) {                                          // the header row: shaded and bold
            QTextTableCell cell = t->cellAt(0, c);
            QTextCharFormat cf = cell.format();
            cf.setBackground(QColor("#e8edf3"));
            cf.setFontWeight(QFont::Bold);
            cell.setFormat(cf);
        }
    }
}

// Every image of the Markdown text is loaded from its file (relative to the Markdown file) and given to the document; then each one is scaled so that
// it is as wide as the page, or its natural size when that is smaller.
void loadImages(QTextDocument& doc, const QString& markdown, const QString& dir) {
    QRegularExpression re("!\\[[^\\]]*\\]\\(([^)\\s]+)\\)");
    for (auto it = re.globalMatch(markdown); it.hasNext();) {
        const QString name = it.next().captured(1);
        QImageReader reader(QFileInfo(name).isAbsolute() ? name : dir + "/" + name);
        if (reader.format() == "svg" && reader.size().isValid())                         // a drawing: enough pixels for print (it is as wide as the page)
            reader.setScaledSize(reader.size() * (2400.0 / reader.size().width()));
        QImage img = reader.read();
        if (img.isNull()) { std::fprintf(stderr, "md2pdf: cannot load the image %s\n", qPrintable(name)); continue; }
        doc.addResource(QTextDocument::ImageResource, QUrl(name), img);
    }
}
void fitImages(QTextDocument& doc, qreal pageWidth) {
    for (QTextBlock b = doc.begin(); b.isValid(); b = b.next())
        for (QTextBlock::iterator it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment f = it.fragment();
            if (!f.isValid() || !f.charFormat().isImageFormat()) continue;
            QTextImageFormat imf = f.charFormat().toImageFormat();
            const QImage img = doc.resource(QTextDocument::ImageResource, QUrl(imf.name())).value<QImage>();
            if (img.isNull()) continue;
            const qreal w = pageWidth;                                                   // the figures are drawn for the full width of the page
            imf.setWidth(w);
            imf.setHeight(w * img.height() / img.width());
            QTextCursor c(&doc);
            c.setPosition(f.position());
            c.setPosition(f.position() + f.length(), QTextCursor::KeepAnchor);
            c.setCharFormat(imf);
        }
}

// Page breaks where the plain flow would cut something: before a table that does not fit in what is left of the page (the break goes before the short
// caption line or the heading right above it, so that they stay together), and before a heading whose next block falls on the next page.
// Returns how many breaks were added; called until nothing changes.
int keepTogether(QTextDocument& doc, qreal pageHeight) {
    QAbstractTextDocumentLayout* lay = doc.documentLayout();
    auto pageOf = [&](qreal y) { return (int)(y / pageHeight); };
    auto breakBefore = [&](QTextBlock b, bool caption) {
        // what introduces it moves with it: one caption line right above a table or a listing (a short paragraph that ends with a colon), then headings
        QTextBlock prev = b.previous();
        const QString pt = prev.isValid() ? prev.text().trimmed() : QString();
        if (caption && prev.isValid() && prev.position() > 0 && prev.blockFormat().headingLevel() == 0 && !pt.isEmpty() && pt.size() < 90
            && (pt.endsWith(QChar(':')) || pt.endsWith(QChar(0xFF1A))) && lay->blockBoundingRect(prev).height() < pageHeight / 8)
            b = prev;
        for (prev = b.previous(); prev.isValid() && prev.position() > 0 && prev.blockFormat().headingLevel() > 0; prev = b.previous()) b = prev;
        if (b.blockFormat().pageBreakPolicy() & QTextFormat::PageBreak_AlwaysBefore) return false;
        QTextCursor c(b);
        QTextBlockFormat bf = b.blockFormat();
        bf.setPageBreakPolicy(QTextFormat::PageBreak_AlwaysBefore);
        c.setBlockFormat(bf);
        return true;
    };
    // Everything that is cut, with where it is; the one nearest to the start of the document is repaired first, because a break only moves what
    // comes after it (so a decision that was made stays right).
    std::vector<std::pair<qreal, std::function<bool()>>> cut;
    for (QTextFrame* f : doc.rootFrame()->childFrames()) {
        QTextTable* t = qobject_cast<QTextTable*>(f);
        if (!t) continue;
        const QRectF r = lay->frameBoundingRect(t);
        if (r.height() >= pageHeight * 0.92 || pageOf(r.top()) == pageOf(r.bottom() - 1)) continue;      // longer than a page: it has to be cut somewhere
        cut.push_back({r.top(), [&, t]() {
            QTextBlock above = t->cellAt(0, 0).firstCursorPosition().block().previous();                  // the block right above the table
            if (above.isValid() && above.position() > 0) {
                const QString at = above.text().trimmed();
                const bool intro = above.blockFormat().headingLevel() > 0 || (!at.isEmpty() && at.size() < 90 && (at.endsWith(QChar(':')) || at.endsWith(QChar(0xFF1A))));
                if (intro && breakBefore(above, false)) return true;
                QTextBlock h = above.previous();                                                         // heading, a line or two, table: all three move
                if (!intro && h.isValid() && h.blockFormat().headingLevel() > 0 && lay->blockBoundingRect(above).height() < pageHeight / 12 && breakBefore(h, false)) return true;
            }
            QTextTableFormat tf = t->format();
            if (tf.pageBreakPolicy() & QTextFormat::PageBreak_AlwaysBefore) return false;
            tf.setPageBreakPolicy(QTextFormat::PageBreak_AlwaysBefore);
            t->setFormat(tf);
            return true;
        }});
    }
    auto isCode = [](const QTextBlock& b) { return b.isValid() && b.blockFormat().hasProperty(QTextFormat::BlockCodeFence); };
    for (QTextBlock b = doc.begin(); b.isValid(); b = b.next()) {
        if (isCode(b) && !isCode(b.previous())) {                                         // a listing shorter than a third of a page is not cut
            QTextBlock last = b;
            while (isCode(last.next())) last = last.next();
            const QRectF r0 = lay->blockBoundingRect(b), r1 = lay->blockBoundingRect(last);
            if (r0.isValid() && r1.isValid() && pageOf(r0.top()) != pageOf(r1.bottom() - 1) && r1.bottom() - r0.top() < pageHeight / 3)
                cut.push_back({r0.top(), [&, b]() { return breakBefore(b, true); }});
        }
        if (b.blockFormat().headingLevel() <= 0) continue;
        QTextBlock next = b.next();
        if (!next.isValid()) continue;
        const QRectF rb = lay->blockBoundingRect(b), rn = lay->blockBoundingRect(next);
        if (!rb.isValid() || !rn.isValid() || pageOf(rb.top()) == pageOf(rn.bottom() - 1)) continue;
        bool picture = false;                                                            // a picture cannot be cut: it goes to the next page as a whole
        for (QTextBlock::iterator it = next.begin(); !it.atEnd(); ++it) if (it.fragment().charFormat().isImageFormat()) picture = true;
        const qreal left = pageHeight - std::fmod(rb.bottom(), pageHeight);              // what is left of the page under the heading
        if (picture || left < rb.height() * 1.6) cut.push_back({rb.top(), [&, b]() { return breakBefore(b, false); }});
    }
    std::sort(cut.begin(), cut.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
    for (auto& c : cut) if (c.second()) return 1;
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: md2pdf INPUT.md OUTPUT.pdf [--title TITLE]\n"); return 2; }
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");        // no window is needed
    QGuiApplication app(argc, argv);
    QString title;
    double pointSize = 10;
    for (int i = 3; i + 1 < argc; i++) {
        if (std::strcmp(argv[i], "--title") == 0) title = QString::fromUtf8(argv[i + 1]);
        else if (std::strcmp(argv[i], "--font-size") == 0) pointSize = QString::fromUtf8(argv[i + 1]).toDouble();
    }
    if (pointSize < 6 || pointSize > 30) pointSize = 10;
    QFile in(QString::fromLocal8Bit(argv[1]));
    if (!in.open(QIODevice::ReadOnly)) { std::fprintf(stderr, "md2pdf: cannot read %s\n", argv[1]); return 1; }
    const QString markdown = QString::fromUtf8(in.readAll());

    const QString family = pickFamily({"Microsoft JhengHei", "Noto Sans CJK TC", "Noto Sans TC", "WenQuanYi Zen Hei", "PingFang TC", "Microsoft YaHei", "DejaVu Sans", "Arial"});
    QFont base(family.isEmpty() ? QFontDatabase::systemFont(QFontDatabase::GeneralFont).family() : family);
    base.setPointSizeF(pointSize);
    QTextDocument doc;
    doc.setDefaultFont(base);
    doc.setDocumentMargin(0);
    loadImages(doc, markdown, QFileInfo(QString::fromLocal8Bit(argv[1])).absolutePath());
    doc.setMarkdown(markdown, QTextDocument::MarkdownDialectGitHub);
    styleTables(doc);

    QPdfWriter pdf(QString::fromLocal8Bit(argv[2]));
    pdf.setPageSize(QPageSize(QPageSize::A4));
    pdf.setPageMargins(QMarginsF(18, 16, 18, 16), QPageLayout::Millimeter);
    pdf.setResolution(144);
    if (!title.isEmpty()) pdf.setTitle(title);
    pdf.setCreator("AssemblyCore md2pdf");
    doc.setPageSize(QSizeF(pdf.width(), pdf.height()));
    fitImages(doc, pdf.width());
    int breaks = 0;
    for (int guard = 0; guard < 400 && keepTogether(doc, pdf.height()); guard++) breaks++;        // (each break moves what follows: look again)
    int cut = 0;                                                                          // tables that are still cut (longer than a page)
    for (QTextFrame* f : doc.rootFrame()->childFrames())
        if (qobject_cast<QTextTable*>(f)) {
            const QRectF r = doc.documentLayout()->frameBoundingRect(f);
            if ((int)(r.top() / pdf.height()) != (int)((r.bottom() - 1) / pdf.height())) cut++;
        }
    doc.print(&pdf);
    std::printf("md2pdf: %d page(s), %d page break(s) added to keep tables and headings whole, %d table(s) cut by a page\n", doc.pageCount(), breaks, cut);
    std::printf("md2pdf: %s -> %s (font: %s)\n", argv[1], argv[2], qPrintable(base.family()));
    return 0;
}
