#include "gui/search/AdvancedSearchDialog.h"
#include "gui/widgets/EmailAddressLineEdit.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include <optional>
#include <string>

namespace javelin::gui::search
{
    namespace
    {

        [[nodiscard]] std::optional<std::string> optionalText(const QLineEdit& edit)
        {
            const auto value = edit.text().trimmed();
            return value.isEmpty() ? std::optional<std::string>{std::nullopt}
                                   : std::optional<std::string>{value.toStdString()};
        }

    } // namespace

    AdvancedSearchDialog::AdvancedSearchDialog(QWidget* parent) : QDialog(parent)
    {
        setWindowTitle(QStringLiteral("Advanced Search"));

        m_textEdit = new QLineEdit(this);
        m_withEdit = new widgets::EmailAddressLineEdit(true, this);
        m_fromEdit = new widgets::EmailAddressLineEdit(false, this);
        m_toEdit = new widgets::EmailAddressLineEdit(true, this);
        m_ccEdit = new widgets::EmailAddressLineEdit(true, this);
        m_bccEdit = new widgets::EmailAddressLineEdit(true, this);
        m_subjectEdit = new QLineEdit(this);
        m_bodyEdit = new QLineEdit(this);

        auto* formLayout = new QFormLayout;
        formLayout->addRow(QStringLiteral("Anywhere"), m_textEdit);
        formLayout->addRow(QStringLiteral("With"), m_withEdit);
        formLayout->addRow(QStringLiteral("From"), m_fromEdit);
        formLayout->addRow(QStringLiteral("To"), m_toEdit);
        formLayout->addRow(QStringLiteral("Cc"), m_ccEdit);
        formLayout->addRow(QStringLiteral("Bcc"), m_bccEdit);
        formLayout->addRow(QStringLiteral("Subject"), m_subjectEdit);
        formLayout->addRow(QStringLiteral("Body"), m_bodyEdit);

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, this);
        buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Search"));
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

        auto* layout = new QVBoxLayout(this);
        layout->addLayout(formLayout);
        layout->addWidget(buttons);

        resize(420, sizeHint().height());
    }

    javelin::jmap::search::EmailSearchCriteria AdvancedSearchDialog::criteria() const
    {
        return javelin::jmap::search::EmailSearchCriteria{
            .text = optionalText(*m_textEdit),
            .with = optionalText(*m_withEdit),
            .from = optionalText(*m_fromEdit),
            .to = optionalText(*m_toEdit),
            .cc = optionalText(*m_ccEdit),
            .bcc = optionalText(*m_bccEdit),
            .subject = optionalText(*m_subjectEdit),
            .body = optionalText(*m_bodyEdit),
        };
    }

} // namespace javelin::gui::search
