#include "custom_block_dialog.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace acide {

CustomBlockDialog::CustomBlockDialog(const blocks::BlockCatalog& catalog, const std::vector<std::string>& drawers, const QString& replacingId, QWidget* parent)
    : QDialog(parent), catalog_(catalog), replacing_(replacingId) {
    setObjectName("customBlockDialog");
    setWindowTitle(replacingId.isEmpty() ? tr("New block") : tr("Edit the block"));
    auto* l = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    drawer_ = new QComboBox(this);
    drawer_->setObjectName("customDrawer");
    for (const std::string& d : drawers) drawer_->addItem(QCoreApplication::translate("BlockDrawer", d.c_str()), QString::fromStdString(d));
    title_ = new QLineEdit(this);
    title_->setObjectName("customTitle");
    title_->setPlaceholderText(tr("The name shown in the toolbox"));
    form->addRow(tr("Drawer"), drawer_);
    form->addRow(tr("Name"), title_);
    l->addLayout(form);
    l->addWidget(new QLabel(tr("Made from this assembler text (several lines make several blocks; in a drawer of components: an operand such as MAIN_SCRATCHPAD[4..7] or LIMIT + 1):"), this));
    text_ = new QPlainTextEdit(this);
    text_->setObjectName("customText");
    QFont mono = font();
    mono.setFamily("monospace");
    mono.setStyleHint(QFont::Monospace);
    text_->setFont(mono);
    text_->setMinimumHeight(110);
    l->addWidget(text_);
    fromSelection_ = new QPushButton(tr("Use the selected blocks"), this);
    fromSelection_->setObjectName("customFromSelection");
    fromSelection_->setEnabled(false);
    l->addWidget(fromSelection_, 0, Qt::AlignLeft);
    note_ = new QLabel(this);
    note_->setObjectName("customNote");
    note_->setWordWrap(true);
    l->addWidget(note_);
    buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    l->addWidget(buttons_);
    connect(buttons_, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(title_, &QLineEdit::textChanged, this, [this] { validate(); });
    connect(text_, &QPlainTextEdit::textChanged, this, [this] { validate(); });
    connect(drawer_, &QComboBox::currentIndexChanged, this, [this] { validate(); });
    connect(fromSelection_, &QPushButton::clicked, this, [this] { text_->setPlainText(selection_); });
    resize(520, 420);
    validate();
}

void CustomBlockDialog::setValues(const QString& drawer, const QString& title, const QString& text) {
    const int i = drawer_->findData(drawer);
    if (i >= 0) drawer_->setCurrentIndex(i);
    title_->setText(title);
    text_->setPlainText(text);
    validate();
}

blocks::CustomBlock CustomBlockDialog::values() const {
    return blocks::CustomBlock{drawer_->currentData().toString().toStdString(), title_->text().trimmed().toUtf8().toStdString(), text_->toPlainText().toUtf8().toStdString()};
}

void CustomBlockDialog::setSelectionText(const QString& text) {
    selection_ = text;
    fromSelection_->setEnabled(!text.trimmed().isEmpty());
}

bool CustomBlockDialog::valid() const { return buttons_->button(QDialogButtonBox::Ok)->isEnabled(); }
QString CustomBlockDialog::message() const { return note_->text(); }

void CustomBlockDialog::validate() {
    const blocks::BlockCatalog::Check c = catalog_.checkCustom(values(), replacing_.toStdString());
    buttons_->button(QDialogButtonBox::Ok)->setEnabled(c.ok);
    QString m = QString::fromStdString(c.message);
    if (!m.isEmpty() && !c.ok) m = QCoreApplication::translate("CustomBlockCheck", c.message.c_str());
    else if (c.ok && c.raw == 0) m = tr("OK: %n block(s) are made", nullptr, c.blocks);
    else if (c.ok) m = tr("%n line(s) are not understood as instructions and stay as assembler text", nullptr, c.raw);
    note_->setStyleSheet(c.ok ? (c.raw ? "color:#e37400;" : "color:#2e7d32;") : "color:#c62828;");
    note_->setText(m);
}

}  // namespace acide
