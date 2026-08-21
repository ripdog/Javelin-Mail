#include "gui/compose/ComposeRecipientController.h"

#include "gui/compose/EmailAddressText.h"
#include "gui/widgets/EmailAddressLineEdit.h"
#include "jmap/submission/ComposeTypes.h"

#include <KLocalizedString>

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <optional>
#include <utility>

namespace javelin::gui::compose
{
    struct ComposeRecipientController::RecipientRow
    {
        QWidget* widget = nullptr;
        QComboBox* typeCombo = nullptr;
        QLineEdit* edit = nullptr;
    };

    ComposeRecipientController::~ComposeRecipientController() = default;

    ComposeRecipientController::ComposeRecipientController(QVBoxLayout& layout, QWidget& owner,
                                                           std::function<void()> changed)
        : m_layout(layout), m_owner(owner), m_changed(std::move(changed))
    {
        addRow(RecipientType::To);
    }

    void ComposeRecipientController::setSyncing(const bool syncing)
    {
        m_syncing = syncing;
    }

    void ComposeRecipientController::setEnabled(const bool enabled)
    {
        for (const auto& row : m_rows)
            row.widget->setEnabled(enabled);
    }

    void ComposeRecipientController::reset(const javelin::jmap::submission::DraftSnapshot& snapshot)
    {
        for (const auto& row : m_rows)
            delete row.widget;
        m_rows.clear();

        if (!snapshot.to.empty())
            addRow(RecipientType::To, formatAddresses(snapshot.to));
        if (!snapshot.cc.empty())
            addRow(RecipientType::Cc, formatAddresses(snapshot.cc));
        if (!snapshot.bcc.empty())
            addRow(RecipientType::Bcc, formatAddresses(snapshot.bcc));
        ensureTrailingRow();
    }

    void ComposeRecipientController::setText(const RecipientType type, const QString& value)
    {
        RecipientRow* target = nullptr;
        for (auto& row : m_rows)
        {
            if (row.typeCombo->currentData().toInt() == static_cast<int>(type))
            {
                target = &row;
                break;
            }
        }

        if (target == nullptr && value.trimmed().isEmpty())
            return;

        if (target == nullptr)
        {
            if (!m_rows.empty() && m_rows.back().edit->text().trimmed().isEmpty())
            {
                target = &m_rows.back();
                const auto typeIndex = target->typeCombo->findData(static_cast<int>(type));
                if (typeIndex >= 0)
                    target->typeCombo->setCurrentIndex(typeIndex);
            }
            else
            {
                addRow(type);
                target = &m_rows.back();
            }
        }

        const bool previousSyncing = m_syncing;
        m_syncing = true;
        target->edit->setText(value);
        for (auto& row : m_rows)
        {
            if (&row != target && row.typeCombo->currentData().toInt() == static_cast<int>(type))
                row.edit->clear();
        }
        m_syncing = previousSyncing;
        ensureTrailingRow();
    }

    QString ComposeRecipientController::text(const RecipientType type) const
    {
        QStringList values;
        for (const auto& row : m_rows)
        {
            if (row.typeCombo->currentData().toInt() != static_cast<int>(type))
                continue;
            const auto value = row.edit->text().trimmed();
            if (!value.isEmpty())
                values.push_back(value);
        }
        return values.join(QStringLiteral(", "));
    }

    std::vector<javelin::jmap::domain::EmailAddress>
    ComposeRecipientController::addresses(const RecipientType type) const
    {
        std::vector<javelin::jmap::domain::EmailAddress> result;
        for (const auto& row : m_rows)
        {
            if (row.typeCombo->currentData().toInt() != static_cast<int>(type))
                continue;
            const auto parsed = parseAddressList(row.edit->text(), false);
            if (parsed.has_value())
                result.insert(result.end(), parsed->begin(), parsed->end());
        }
        return result;
    }

    void ComposeRecipientController::updateLabelWidths(QLabel& fromLabel, QLabel& subjectLabel)
    {
        if (m_rows.empty())
            return;
        m_headerLabelWidth =
            std::max({fromLabel.sizeHint().width(), subjectLabel.sizeHint().width(),
                      m_rows.front().typeCombo->sizeHint().width()});
        fromLabel.setFixedWidth(m_headerLabelWidth);
        subjectLabel.setFixedWidth(m_headerLabelWidth);
        for (const auto& row : m_rows)
            row.typeCombo->setFixedWidth(m_headerLabelWidth);
    }

    void ComposeRecipientController::addRow(const RecipientType type, const QString& value)
    {
        auto* rowWidget = new QWidget(&m_owner);
        auto* rowLayout = new QHBoxLayout(rowWidget);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(6);

        auto* typeCombo = new QComboBox(rowWidget);
        typeCombo->setAccessibleName(
            i18nc("@label accessible email recipient type", "Recipient type"));
        typeCombo->addItem(i18nc("@label email recipients", "To"),
                           static_cast<int>(RecipientType::To));
        typeCombo->addItem(i18nc("@label email carbon-copy recipients", "Cc"),
                           static_cast<int>(RecipientType::Cc));
        typeCombo->addItem(i18nc("@label email blind-carbon-copy recipients", "Bcc"),
                           static_cast<int>(RecipientType::Bcc));
        const auto typeIndex = typeCombo->findData(static_cast<int>(type));
        if (typeIndex >= 0)
            typeCombo->setCurrentIndex(typeIndex);

        auto* edit = new widgets::EmailAddressLineEdit(true, rowWidget);
        edit->setPlaceholderText(QStringLiteral("alice@example.com, Bob <bob@example.com>"));
        edit->setText(value);
        const auto updateAccessibleName = [typeCombo, edit]
        {
            edit->setAccessibleName(i18nc("@label accessible email recipients", "%1 recipients",
                                          typeCombo->currentText()));
        };
        updateAccessibleName();
        rowLayout->addWidget(typeCombo);
        rowLayout->addWidget(edit, 1);
        m_layout.addWidget(rowWidget);
        m_rows.push_back({.widget = rowWidget, .typeCombo = typeCombo, .edit = edit});

        if (m_headerLabelWidth > 0)
            typeCombo->setFixedWidth(m_headerLabelWidth);

        QObject::connect(typeCombo, qOverload<int>(&QComboBox::currentIndexChanged), rowWidget,
                         [this, updateAccessibleName](const int)
                         {
                             updateAccessibleName();
                             if (!m_syncing && m_changed)
                                 m_changed();
                         });
        QObject::connect(edit, &QLineEdit::textChanged, rowWidget,
                         [this](const QString&)
                         {
                             if (m_syncing)
                                 return;
                             ensureTrailingRow();
                             if (m_changed)
                                 m_changed();
                         });
    }

    void ComposeRecipientController::ensureTrailingRow()
    {
        if (m_rows.empty() || !m_rows.back().edit->text().trimmed().isEmpty())
            addRow(RecipientType::To);
    }
} // namespace javelin::gui::compose
