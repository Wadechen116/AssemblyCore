// md2pdf: a Markdown file becomes a PDF (A4), with Qt only (QTextDocument reads the Markdown, QPdfWriter writes the pages).
//   md2pdf INPUT.md OUTPUT.pdf [--title "Title"]
// Tables, headings, lists, code blocks and the inline marks of GitHub Markdown are supported; the text may be Chinese (a CJK font of the system is used).
#include <cstdio>
#include <cstring>

#include <QFile>
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

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: md2pdf INPUT.md OUTPUT.pdf [--title TITLE]\n"); return 2; }
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");        // no window is needed
    QGuiApplication app(argc, argv);
    QString title;
    for (int i = 3; i + 1 < argc; i++) if (std::strcmp(argv[i], "--title") == 0) title = QString::fromUtf8(argv[i + 1]);
    QFile in(QString::fromLocal8Bit(argv[1]));
    if (!in.open(QIODevice::ReadOnly)) { std::fprintf(stderr, "md2pdf: cannot read %s\n", argv[1]); return 1; }
    const QString markdown = QString::fromUtf8(in.readAll());

    const QString family = pickFamily({"Microsoft JhengHei", "Noto Sans CJK TC", "Noto Sans TC", "WenQuanYi Zen Hei", "PingFang TC", "Microsoft YaHei", "DejaVu Sans", "Arial"});
    QFont base(family.isEmpty() ? QFontDatabase::systemFont(QFontDatabase::GeneralFont).family() : family);
    base.setPointSizeF(10);
    QTextDocument doc;
    doc.setDefaultFont(base);
    doc.setDocumentMargin(0);
    doc.setMarkdown(markdown, QTextDocument::MarkdownDialectGitHub);
    styleTables(doc);

    QPdfWriter pdf(QString::fromLocal8Bit(argv[2]));
    pdf.setPageSize(QPageSize(QPageSize::A4));
    pdf.setPageMargins(QMarginsF(18, 16, 18, 16), QPageLayout::Millimeter);
    pdf.setResolution(144);
    if (!title.isEmpty()) pdf.setTitle(title);
    pdf.setCreator("AssemblyCore md2pdf");
    doc.setPageSize(QSizeF(pdf.width(), pdf.height()));
    doc.print(&pdf);
    std::printf("md2pdf: %s -> %s (font: %s)\n", argv[1], argv[2], qPrintable(base.family()));
    return 0;
}
