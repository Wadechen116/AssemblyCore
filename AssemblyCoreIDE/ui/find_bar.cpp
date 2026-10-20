#include "find_bar.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QRegularExpression>
#include <QTextDocument>

namespace acide {

FindBar::FindBar(QWidget* parent) : QWidget(parent) {
    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(4, 2, 4, 2);
    find_ = new QLineEdit;
    find_->setPlaceholderText(tr("Find"));
    find_->setObjectName("findField");
    replace_ = new QLineEdit;
    replace_->setPlaceholderText(tr("Replace"));
    replace_->setObjectName("replaceField");
    case_ = new QCheckBox(tr("Aa"));
    case_->setToolTip(tr("Match case"));
    regex_ = new QCheckBox(tr(".*"));
    regex_->setToolTip(tr("Regular expression"));
    auto* next = new QPushButton(tr("Next"));
    auto* prev = new QPushButton(tr("Prev"));
    auto* rep = new QPushButton(tr("Replace"));
    auto* all = new QPushButton(tr("All"));
    auto* close = new QPushButton(QString::fromUtf8("\xC3\x97"));
    result_ = new QLabel;
    for (QWidget* w : std::initializer_list<QWidget*>{find_, replace_, case_, regex_, prev, next, rep, all, result_, close}) lay->addWidget(w);
    lay->setStretch(0, 2);
    lay->setStretch(1, 1);
    connect(find_, &QLineEdit::returnPressed, this, [this] { findNext(false); });
    connect(next, &QPushButton::clicked, this, [this] { findNext(false); });
    connect(prev, &QPushButton::clicked, this, [this] { findNext(true); });
    connect(rep, &QPushButton::clicked, this, [this] {
        if (!editor_) return;
        QTextCursor c = editor_->textCursor();
        if (c.hasSelection()) {
            QString sel = c.selectedText();
            bool match = regex_->isChecked() ? QRegularExpression(find_->text(), case_->isChecked() ? QRegularExpression::NoPatternOption : QRegularExpression::CaseInsensitiveOption).match(sel).hasMatch()
                                             : sel.compare(find_->text(), case_->isChecked() ? Qt::CaseSensitive : Qt::CaseInsensitive) == 0;
            if (match) c.insertText(replace_->text());
        }
        findNext(false);
    });
    connect(all, &QPushButton::clicked, this, [this] { replaceAll(); });
    connect(close, &QPushButton::clicked, this, [this] { hide(); if (editor_) editor_->setFocus(); });
    hide();
}

void FindBar::open(const QString& initial) {
    show();
    if (!initial.isEmpty() && !initial.contains('\n')) find_->setText(initial);
    find_->setFocus();
    find_->selectAll();
}

QTextDocument::FindFlags FindBar::flags(bool backwards) const {
    QTextDocument::FindFlags f;
    if (backwards) f |= QTextDocument::FindBackward;
    if (case_->isChecked()) f |= QTextDocument::FindCaseSensitively;
    return f;
}

bool FindBar::findNext(bool backwards) {
    if (!editor_ || find_->text().isEmpty()) return false;
    bool found;
    if (regex_->isChecked()) {
        QRegularExpression re(find_->text(), case_->isChecked() ? QRegularExpression::NoPatternOption : QRegularExpression::CaseInsensitiveOption);
        if (!re.isValid()) { result_->setText(tr("bad pattern")); return false; }
        found = editor_->find(re, flags(backwards));
        if (!found) {
            QTextCursor c = editor_->textCursor();
            c.movePosition(backwards ? QTextCursor::End : QTextCursor::Start);
            editor_->setTextCursor(c);
            found = editor_->find(re, flags(backwards));
        }
    } else {
        found = editor_->find(find_->text(), flags(backwards));
        if (!found) {                                           // wrap around
            QTextCursor c = editor_->textCursor();
            c.movePosition(backwards ? QTextCursor::End : QTextCursor::Start);
            editor_->setTextCursor(c);
            found = editor_->find(find_->text(), flags(backwards));
        }
    }
    result_->setText(found ? QString() : tr("not found"));
    return found;
}

int FindBar::replaceAll() {
    if (!editor_ || find_->text().isEmpty()) return 0;
    int n = 0;
    QTextCursor c = editor_->textCursor();
    c.beginEditBlock();
    QTextCursor pos(editor_->document());
    for (;;) {
        QTextCursor f = regex_->isChecked()
            ? editor_->document()->find(QRegularExpression(find_->text(), case_->isChecked() ? QRegularExpression::NoPatternOption : QRegularExpression::CaseInsensitiveOption), pos, flags(false))
            : editor_->document()->find(find_->text(), pos, flags(false));
        if (f.isNull()) break;
        f.insertText(replace_->text());
        pos = f;
        n++;
    }
    c.endEditBlock();
    result_->setText(tr("%1 replaced").arg(n));
    return n;
}

}  // namespace acide
