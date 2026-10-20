#pragma once
// Find / replace within the current editor.
#include <QCheckBox>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QWidget>

namespace acide {

class FindBar : public QWidget {
    Q_OBJECT
public:
    explicit FindBar(QWidget* parent = nullptr);
    void setEditor(QPlainTextEdit* e) { editor_ = e; }
    void open(const QString& initial);                 // show and focus the search field
    bool findNext(bool backwards = false);
    int replaceAll();

private:
    QTextDocument::FindFlags flags(bool backwards) const;
    QPlainTextEdit* editor_ = nullptr;
    QLineEdit *find_, *replace_;
    QCheckBox *case_, *regex_;
    QLabel* result_;
};

}  // namespace acide
