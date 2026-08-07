#include "gui/compose/SendingIdentitiesDialog.h"

#include "app/IdentityApplicationPorts.h"
#include "app/MailApplicationEventsPorts.h"
#include "gui/compose/ComposeBodyConverter.h"
#include "gui/compose/IdentityPresentation.h"
#include "gui/compose/JavelinComposerEdit.h"
#include "gui/settings/GuiSettings.h"
#include "gui/widgets/EmailAddressLineEdit.h"
#include "jmap/cache/AccountReadRepository.h"
#include "jmap/cache/IdentityReader.h"

#include <QCoroTask>

#include <KActionCollection>
#include <KLocalizedString>
#include <KPIMTextEdit/RichTextComposerControler>

#include <QAbstractButton>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTextCharFormat>
#include <QTextFormat>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace javelin::gui::compose
{
    namespace
    {
        constexpr int itemKindRole = Qt::UserRole;
        constexpr int accountIdRole = Qt::UserRole + 1;
        constexpr int identityIdRole = Qt::UserRole + 2;
        constexpr int creationIdRole = Qt::UserRole + 3;

        enum class ItemKind
        {
            Account,
            Identity,
            PendingCreate,
        };

        [[nodiscard]] QString displayAddress(const javelin::jmap::domain::EmailAddress& address)
        {
            if (address.name.has_value() && !address.name->empty())
            {
                return QStringLiteral("%1 <%2>").arg(QString::fromStdString(*address.name),
                                                     QString::fromStdString(address.email));
            }
            return QString::fromStdString(address.email);
        }

        [[nodiscard]] QString
        formatAddresses(const std::vector<javelin::jmap::domain::EmailAddress>& addresses)
        {
            QStringList result;
            result.reserve(static_cast<qsizetype>(addresses.size()));
            for (const auto& address : addresses)
                result.push_back(displayAddress(address));
            return result.join(QStringLiteral(", "));
        }

        [[nodiscard]] std::optional<javelin::jmap::domain::EmailAddress>
        parseAddressToken(const QString& token)
        {
            const auto trimmed = token.trimmed();
            if (trimmed.isEmpty())
                return std::nullopt;
            const auto openBracket = trimmed.lastIndexOf(QLatin1Char('<'));
            const auto closeBracket = trimmed.lastIndexOf(QLatin1Char('>'));
            if (openBracket >= 0 && closeBracket > openBracket)
            {
                const auto name = trimmed.left(openBracket).trimmed().remove(QLatin1Char('"'));
                const auto email =
                    trimmed.mid(openBracket + 1, closeBracket - openBracket - 1).trimmed();
                if (!email.contains(QLatin1Char('@')))
                    return std::nullopt;
                return javelin::jmap::domain::EmailAddress{
                    .name = name.isEmpty() ? std::nullopt
                                           : std::optional<std::string>{name.toStdString()},
                    .email = email.toStdString(),
                };
            }
            if (!trimmed.contains(QLatin1Char('@')))
                return std::nullopt;
            return javelin::jmap::domain::EmailAddress{
                .name = std::nullopt,
                .email = trimmed.toStdString(),
            };
        }

        [[nodiscard]] std::optional<std::vector<javelin::jmap::domain::EmailAddress>>
        parseAddresses(const QString& value)
        {
            std::vector<javelin::jmap::domain::EmailAddress> result;
            QString current;
            bool insideAngleBrackets = false;
            for (const auto character : value)
            {
                if (character == QLatin1Char('<'))
                    insideAngleBrackets = true;
                else if (character == QLatin1Char('>'))
                    insideAngleBrackets = false;
                if (!insideAngleBrackets &&
                    (character == QLatin1Char(',') || character == QLatin1Char(';')))
                {
                    if (!current.trimmed().isEmpty())
                    {
                        const auto parsed = parseAddressToken(current);
                        if (!parsed.has_value())
                            return std::nullopt;
                        result.push_back(*parsed);
                    }
                    current.clear();
                    continue;
                }
                current.append(character);
            }
            if (!current.trimmed().isEmpty())
            {
                const auto parsed = parseAddressToken(current);
                if (!parsed.has_value())
                    return std::nullopt;
                result.push_back(*parsed);
            }
            return result;
        }

        [[nodiscard]] QString signatureHtmlWithoutImages(QString html)
        {
            static const QRegularExpression imageTag{QStringLiteral("<img\\b[^>]*>"),
                                                     QRegularExpression::CaseInsensitiveOption};
            html.remove(imageTag);
            return html;
        }

        [[nodiscard]] QString pendingStatusLabel(const std::string_view status)
        {
            if (status == "unknown")
                return i18n("Outcome uncertain");
            if (status == "in_flight")
                return i18n("Saving…");
            return i18n("Pending");
        }

        [[nodiscard]] QString signatureLabel(const javelin::jmap::domain::Identity& identity)
        {
            const auto preview = identitySignaturePreview(identity);
            return preview.isEmpty() ? i18n("No signature") : preview;
        }
    } // namespace

    SendingIdentitiesDialog::SendingIdentitiesDialog(
        javelin::gui::settings::GuiSettings& settings,
        javelin::jmap::cache::AccountReader& accountReader,
        javelin::jmap::cache::IdentityReader& identityReader,
        javelin::app::IdentityCommandPort& commandPort,
        javelin::app::MailApplicationEventsPort& mailEvents, QWidget* parent)
        : QDialog(parent), m_settings(settings), m_accountReader(accountReader),
          m_identityReader(identityReader), m_commandPort(commandPort), m_mailEvents(mailEvents)
    {
        setupUi();
        createFormattingActions();
        connect(&m_mailEvents, &javelin::app::MailApplicationEventsPort::cacheInvalidated, this,
                [this](const javelin::app::MailCacheInvalidation& invalidation)
                {
                    if (!invalidation.change.identitiesChanged || m_busy || m_editorDirty)
                        return;
                    reloadTree(selectedAccountId(), selectedIdentityId());
                });
        reloadTree();
    }

    void SendingIdentitiesDialog::setupUi()
    {
        setWindowTitle(i18n("Sending Identities and Signatures"));
        resize(1120, 720);

        auto* rootLayout = new QVBoxLayout(this);
        auto* splitter = new QSplitter(Qt::Horizontal, this);
        rootLayout->addWidget(splitter, 1);

        auto* listPane = new QWidget(splitter);
        auto* listLayout = new QVBoxLayout(listPane);
        listLayout->setContentsMargins(0, 0, 0, 0);
        m_tree = new QTreeWidget(listPane);
        m_tree->setColumnCount(2);
        m_tree->setHeaderLabels({i18n("Identity"), i18n("Signature")});
        m_tree->setRootIsDecorated(true);
        m_tree->setAlternatingRowColors(true);
        m_tree->header()->setStretchLastSection(true);
        m_tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        listLayout->addWidget(m_tree, 1);

        auto* listButtons = new QHBoxLayout;
        m_newButton =
            new QPushButton(QIcon::fromTheme(QStringLiteral("list-add")), i18n("New…"), listPane);
        m_newButton->setObjectName(QStringLiteral("identityNewButton"));
        m_duplicateButton = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-copy")),
                                            i18n("Duplicate"), listPane);
        m_deleteButton = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-delete")),
                                         i18n("Delete"), listPane);
        m_refreshButton = new QPushButton(QIcon::fromTheme(QStringLiteral("view-refresh")),
                                          i18n("Refresh"), listPane);
        listButtons->addWidget(m_newButton);
        listButtons->addWidget(m_duplicateButton);
        listButtons->addWidget(m_deleteButton);
        listButtons->addStretch(1);
        listButtons->addWidget(m_refreshButton);
        listLayout->addLayout(listButtons);

        auto* editorPane = new QWidget(splitter);
        auto* editorLayout = new QVBoxLayout(editorPane);
        editorLayout->setContentsMargins(0, 0, 0, 0);
        auto* form = new QFormLayout;
        m_nameEdit = new QLineEdit(editorPane);
        m_emailEdit = new QLineEdit(editorPane);
        m_replyToEdit = new javelin::gui::widgets::EmailAddressLineEdit(true, editorPane);
        m_bccEdit = new javelin::gui::widgets::EmailAddressLineEdit(true, editorPane);
        form->addRow(i18n("Display name:"), m_nameEdit);
        form->addRow(i18n("Email address:"), m_emailEdit);
        form->addRow(i18n("Reply-To:"), m_replyToEdit);
        form->addRow(i18n("Automatic Bcc:"), m_bccEdit);
        editorLayout->addLayout(form);

        m_richTextCheck = new QCheckBox(i18n("Rich Text"), editorPane);
        editorLayout->addWidget(m_richTextCheck);
        m_formatToolbar = new QToolBar(editorPane);
        m_formatToolbar->setIconSize(QSize{18, 18});
        editorLayout->addWidget(m_formatToolbar);
        m_signatureEdit = new JavelinComposerEdit(editorPane);
        m_signatureEdit->setAcceptDrops(false);
        m_signatureEdit->setAcceptRichText(true);
        m_signatureEdit->document()->setDocumentMargin(8);
        editorLayout->addWidget(m_signatureEdit, 1);

        auto* editorButtons = new QHBoxLayout;
        editorButtons->addStretch(1);
        m_revertButton = new QPushButton(i18n("Revert"), editorPane);
        m_saveButton = new QPushButton(QIcon::fromTheme(QStringLiteral("document-save")),
                                       i18n("Save"), editorPane);
        m_saveButton->setDefault(true);
        editorButtons->addWidget(m_revertButton);
        editorButtons->addWidget(m_saveButton);
        editorLayout->addLayout(editorButtons);

        splitter->addWidget(listPane);
        splitter->addWidget(editorPane);
        splitter->setStretchFactor(0, 1);
        splitter->setStretchFactor(1, 2);
        splitter->setSizes({380, 740});

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        rootLayout->addWidget(buttons);

        connect(m_tree, &QTreeWidget::currentItemChanged, this,
                [this](QTreeWidgetItem* current, QTreeWidgetItem* previous)
                {
                    if (m_editorDirty && previous != nullptr && current != previous)
                    {
                        const auto answer = QMessageBox::question(
                            this, i18n("Discard Changes?"),
                            i18n("Discard the unsaved identity changes?"),
                            QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
                        if (answer != QMessageBox::Discard)
                        {
                            const QSignalBlocker blocker{m_tree};
                            m_tree->setCurrentItem(previous);
                            return;
                        }
                    }
                    loadCurrentSelection();
                });
        connect(m_newButton, &QPushButton::clicked, this,
                &SendingIdentitiesDialog::beginNewIdentity);
        connect(m_duplicateButton, &QPushButton::clicked, this,
                &SendingIdentitiesDialog::duplicateCurrentIdentity);
        connect(m_deleteButton, &QPushButton::clicked, this,
                &SendingIdentitiesDialog::deleteCurrentIdentity);
        connect(m_refreshButton, &QPushButton::clicked, this,
                &SendingIdentitiesDialog::refreshCurrentAccount);
        connect(m_saveButton, &QPushButton::clicked, this,
                &SendingIdentitiesDialog::saveCurrentIdentity);
        connect(m_revertButton, &QPushButton::clicked, this,
                &SendingIdentitiesDialog::revertCurrentIdentity);
        connect(m_richTextCheck, &QCheckBox::toggled, this,
                &SendingIdentitiesDialog::switchEditorMode);

        const auto markDirty = [this]
        {
            if (m_switchingMode || !m_editorIdentity.has_value())
                return;
            m_editorDirty = true;
            updateActions();
        };
        for (auto* edit : {m_nameEdit, m_emailEdit, m_replyToEdit, m_bccEdit})
            connect(edit, &QLineEdit::textChanged, this, markDirty);
        connect(m_signatureEdit, &QTextEdit::textChanged, this, markDirty);
        connect(m_signatureEdit, &JavelinComposerEdit::attachmentPathsRequested, this,
                [this](const QStringList&)
                {
                    QMessageBox::information(
                        this, i18n("Unsupported Signature Content"),
                        i18n("Files and inline images cannot be embedded in JMAP signatures."));
                });
        connect(m_signatureEdit, &JavelinComposerEdit::inlineImageRequested, this,
                [this](const QImage&)
                {
                    QMessageBox::information(
                        this, i18n("Unsupported Signature Content"),
                        i18n("Files and inline images cannot be embedded in JMAP signatures."));
                });
    }

    void SendingIdentitiesDialog::createFormattingActions()
    {
        m_actionCollection =
            new KActionCollection(this, QStringLiteral("javelin-signature-editor"));
        m_actionCollection->addAssociatedWidget(this);
        m_signatureEdit->createActions(m_actionCollection);
        const auto add = [this](const QString& name)
        {
            if (auto* action = m_actionCollection->action(name))
                m_formatToolbar->addAction(action);
        };
        add(QStringLiteral("format_heading_level"));
        add(QStringLiteral("format_list_style"));
        add(QStringLiteral("format_font_family"));
        add(QStringLiteral("format_font_size"));
        m_formatToolbar->addSeparator();
        add(QStringLiteral("format_text_bold"));
        add(QStringLiteral("format_text_italic"));
        add(QStringLiteral("format_text_underline"));
        add(QStringLiteral("format_text_strikeout"));
        auto* codeAction = new QAction(QIcon::fromTheme(QStringLiteral("format-text-code")),
                                       i18nc("@action text formatting", "Code"), this);
        codeAction->setCheckable(true);
        m_actionCollection->addAction(QStringLiteral("javelin_signature_format_code"), codeAction);
        m_formatToolbar->addAction(codeAction);
        connect(codeAction, &QAction::triggered, this,
                [this, codeAction]
                {
                    QTextCharFormat format;
                    if (codeAction->isChecked())
                        format.setFontFamilies(QStringList{QStringLiteral("monospace")});
                    else
                        format.clearProperty(QTextFormat::FontFamilies);
                    m_signatureEdit->mergeCurrentCharFormat(format);
                });
        connect(m_signatureEdit, &QTextEdit::currentCharFormatChanged, this,
                [codeAction](const QTextCharFormat& format)
                {
                    const QSignalBlocker blocker{codeAction};
                    codeAction->setChecked(
                        format.fontFamilies().toStringList().contains(QStringLiteral("monospace")));
                });
        add(QStringLiteral("format_text_foreground_color"));
        add(QStringLiteral("format_text_background_color"));
        m_formatToolbar->addSeparator();
        add(QStringLiteral("format_align_left"));
        add(QStringLiteral("format_align_center"));
        add(QStringLiteral("format_align_right"));
        add(QStringLiteral("format_align_justify"));
        add(QStringLiteral("format_list_indent_more"));
        add(QStringLiteral("format_list_indent_less"));
        m_formatToolbar->addSeparator();
        add(QStringLiteral("manage_link"));
        add(QStringLiteral("insert_horizontal_rule"));
        add(QStringLiteral("insert_html"));
        add(QStringLiteral("insert_table"));
        add(QStringLiteral("format_list_checkbox"));
        add(QStringLiteral("format_reset"));
        add(QStringLiteral("format_painter"));
        add(QStringLiteral("direction_ltr"));
        add(QStringLiteral("direction_rtl"));
        updateActions();
    }

    void SendingIdentitiesDialog::reloadTree(std::optional<std::string> selectedAccount,
                                             std::optional<std::string> selectedIdentity)
    {
        if (!selectedAccount.has_value())
            selectedAccount = selectedAccountId();
        if (!selectedIdentity.has_value())
            selectedIdentity = selectedIdentityId();

        const QSignalBlocker blocker{m_tree};
        m_tree->clear();
        m_accounts.clear();
        QTreeWidgetItem* selectedItem = nullptr;
        QTreeWidgetItem* firstIdentityItem = nullptr;

        const auto accountsResult = m_accountReader.listAll();
        const auto* accounts =
            std::get_if<std::vector<javelin::jmap::cache::CachedAccount>>(&accountsResult);
        if (accounts == nullptr)
        {
            QMessageBox::critical(
                this, i18n("Unable to Load Identities"),
                std::get<javelin::jmap::cache::DatabaseError>(accountsResult).message);
            clearEditor();
            return;
        }

        for (const auto& account : *accounts)
        {
            if (!account.hasSubmissionCapability)
                continue;
            const auto guiAccount =
                m_settings.accountForCachedId(QString::fromStdString(account.accountId));
            auto displayName =
                guiAccount.displayName.isEmpty() ? guiAccount.loginEmail : guiAccount.displayName;
            if (displayName.isEmpty())
                displayName = account.name.empty() ? QString::fromStdString(account.accountId)
                                                   : QString::fromStdString(account.name);
            m_accounts.push_back({.accountId = account.accountId, .displayName = displayName});

            auto* accountItem = new QTreeWidgetItem(m_tree, {displayName, {}});
            accountItem->setData(0, itemKindRole, static_cast<int>(ItemKind::Account));
            accountItem->setData(0, accountIdRole, QString::fromStdString(account.accountId));
            accountItem->setFlags(accountItem->flags() & ~Qt::ItemIsSelectable);
            QFont accountFont = accountItem->font(0);
            accountFont.setBold(true);
            accountItem->setFont(0, accountFont);

            const auto identitiesResult = m_identityReader.listByAccount(account.accountId);
            const auto pendingResult = m_identityReader.listPendingCreates(account.accountId);
            const auto* identities =
                std::get_if<std::vector<javelin::jmap::domain::Identity>>(&identitiesResult);
            const auto* pending =
                std::get_if<std::vector<javelin::jmap::cache::PendingIdentityCreate>>(
                    &pendingResult);
            if (identities == nullptr || pending == nullptr)
            {
                auto* errorItem = new QTreeWidgetItem(accountItem, {i18n("Unable to load"), {}});
                errorItem->setDisabled(true);
                continue;
            }

            QHash<QString, int> identicalTotals;
            for (const auto& identity : *identities)
            {
                const auto key =
                    identityAddressLabel(identity) + QLatin1Char('\n') + signatureLabel(identity);
                ++identicalTotals[key];
            }
            QHash<QString, int> identicalSeen;
            for (const auto& identity : *identities)
            {
                if (isWildcardSenderIdentity(identity))
                    continue;
                auto signature = signatureLabel(identity);
                const auto key = identityAddressLabel(identity) + QLatin1Char('\n') + signature;
                if (identicalTotals.value(key) > 1)
                {
                    signature += i18n(" — Variant %1", ++identicalSeen[key]);
                }
                auto* item = new QTreeWidgetItem(
                    accountItem, {identityAddressLabel(identity), std::move(signature)});
                item->setData(0, itemKindRole, static_cast<int>(ItemKind::Identity));
                item->setData(0, accountIdRole, QString::fromStdString(account.accountId));
                item->setData(0, identityIdRole, QString::fromStdString(identity.id));
                if (firstIdentityItem == nullptr)
                    firstIdentityItem = item;
                if (selectedAccount == account.accountId && selectedIdentity == identity.id)
                    selectedItem = item;
            }

            for (const auto& projection : *pending)
            {
                const auto status = pendingStatusLabel(projection.status);
                auto* item = new QTreeWidgetItem(
                    accountItem,
                    {identityAddressLabel(projection.identity),
                     QStringLiteral("%1 — %2").arg(status, signatureLabel(projection.identity))});
                item->setData(0, itemKindRole, static_cast<int>(ItemKind::PendingCreate));
                item->setData(0, accountIdRole, QString::fromStdString(account.accountId));
                item->setData(0, creationIdRole, QString::fromStdString(projection.creationId));
                item->setIcon(0, QIcon::fromTheme(projection.status == "unknown"
                                                      ? QStringLiteral("dialog-warning")
                                                      : QStringLiteral("document-save")));
                if (firstIdentityItem == nullptr)
                    firstIdentityItem = item;
            }
            accountItem->setExpanded(true);
        }

        m_tree->setCurrentItem(selectedItem != nullptr ? selectedItem : firstIdentityItem);
        if (m_tree->currentItem() == nullptr)
            clearEditor();
        else
            loadCurrentSelection();
        updateActions();
    }

    void SendingIdentitiesDialog::loadCurrentSelection()
    {
        const auto* item = m_tree->currentItem();
        if (item == nullptr)
        {
            clearEditor();
            return;
        }
        const auto kind = static_cast<ItemKind>(item->data(0, itemKindRole).toInt());
        const auto accountId = item->data(0, accountIdRole).toString().toStdString();
        if (kind == ItemKind::Identity)
        {
            const auto identityId = item->data(0, identityIdRole).toString().toStdString();
            const auto found = m_identityReader.find(accountId, identityId);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&found))
            {
                QMessageBox::critical(this, i18n("Unable to Load Identity"), error->message);
                clearEditor();
                return;
            }
            const auto& identity = std::get<std::optional<javelin::jmap::domain::Identity>>(found);
            if (!identity.has_value())
            {
                reloadTree();
                return;
            }
            presentIdentity(accountId, *identity, false, *identity);
            return;
        }
        if (kind == ItemKind::PendingCreate)
        {
            const auto creationId = item->data(0, creationIdRole).toString().toStdString();
            const auto pending = m_identityReader.listPendingCreates(accountId);
            if (const auto* values =
                    std::get_if<std::vector<javelin::jmap::cache::PendingIdentityCreate>>(&pending))
            {
                const auto found = std::ranges::find(
                    *values, creationId, &javelin::jmap::cache::PendingIdentityCreate::creationId);
                if (found != values->end())
                {
                    presentIdentity(accountId, found->identity, true, std::nullopt);
                    return;
                }
            }
        }
        clearEditor();
    }

    void SendingIdentitiesDialog::presentIdentity(
        std::string accountId, javelin::jmap::domain::Identity identity, const bool pending,
        std::optional<javelin::jmap::domain::Identity> revertIdentity)
    {
        m_switchingMode = true;
        m_editorAccountId = std::move(accountId);
        m_editorIdentity = identity;
        m_revertIdentity = std::move(revertIdentity);
        m_editorPending = pending;

        const QSignalBlocker nameBlocker{m_nameEdit};
        const QSignalBlocker emailBlocker{m_emailEdit};
        const QSignalBlocker replyBlocker{m_replyToEdit};
        const QSignalBlocker bccBlocker{m_bccEdit};
        const QSignalBlocker richBlocker{m_richTextCheck};
        const QSignalBlocker signatureBlocker{m_signatureEdit};
        m_nameEdit->setText(QString::fromStdString(identity.name));
        m_emailEdit->setText(QString::fromStdString(identity.email));
        m_replyToEdit->setText(formatAddresses(identity.replyTo));
        m_bccEdit->setText(formatAddresses(identity.bcc));
        const bool richText =
            identity.htmlSignature.has_value() && !identity.htmlSignature->empty();
        m_richTextCheck->setChecked(richText);
        if (richText)
        {
            m_signatureEdit->activateRichText();
            m_signatureEdit->setHtml(
                htmlForQtDocument(QString::fromStdString(*identity.htmlSignature)));
        }
        else
        {
            m_signatureEdit->setPlainText(
                QString::fromStdString(identity.textSignature.value_or(std::string{})));
            m_signatureEdit->forcePlainTextMarkup(false);
            m_signatureEdit->switchToPlainText();
        }
        m_formatToolbar->setVisible(richText);
        m_editorDirty = false;
        m_switchingMode = false;
        updateActions();
    }

    void SendingIdentitiesDialog::clearEditor()
    {
        m_switchingMode = true;
        m_editorAccountId.clear();
        m_editorIdentity.reset();
        m_revertIdentity.reset();
        m_editorPending = false;
        m_editorDirty = false;
        m_nameEdit->clear();
        m_emailEdit->clear();
        m_replyToEdit->clear();
        m_bccEdit->clear();
        m_signatureEdit->clear();
        m_switchingMode = false;
        updateActions();
    }

    void SendingIdentitiesDialog::beginNewIdentity()
    {
        if (m_accounts.empty())
            return;
        QDialog dialog{this};
        dialog.setWindowTitle(i18n("New Sending Identity"));
        auto* layout = new QVBoxLayout(&dialog);
        auto* form = new QFormLayout;
        auto* accountCombo = new QComboBox(&dialog);
        for (const auto& account : m_accounts)
        {
            accountCombo->addItem(account.displayName, QString::fromStdString(account.accountId));
        }
        if (const auto current = selectedAccountId(); current.has_value())
        {
            const auto index = accountCombo->findData(QString::fromStdString(*current));
            if (index >= 0)
                accountCombo->setCurrentIndex(index);
        }
        auto* nameEdit = new QLineEdit(&dialog);
        nameEdit->setObjectName(QStringLiteral("identityNewNameEdit"));
        auto* emailEdit = new QLineEdit(&dialog);
        emailEdit->setObjectName(QStringLiteral("identityNewEmailEdit"));
        const auto accountId = accountCombo->currentData().toString().toStdString();
        const auto existing = m_identityReader.listByAccount(accountId);
        if (const auto* identities =
                std::get_if<std::vector<javelin::jmap::domain::Identity>>(&existing);
            identities != nullptr && !identities->empty())
        {
            emailEdit->setText(QString::fromStdString(identities->front().email));
        }
        form->addRow(i18n("Account:"), accountCombo);
        form->addRow(i18n("Display name:"), nameEdit);
        form->addRow(i18n("Email address:"), emailEdit);
        layout->addLayout(form);
        auto* buttons =
            new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        layout->addWidget(buttons);
        if (dialog.exec() != QDialog::Accepted)
            return;
        if (!emailEdit->text().trimmed().contains(QLatin1Char('@')))
        {
            QMessageBox::warning(this, i18n("Invalid Email Address"),
                                 i18n("Enter a valid sender email address."));
            return;
        }
        const auto selectedAccount = accountCombo->currentData().toString().toStdString();
        javelin::jmap::domain::Identity identity{
            .id = {},
            .name = nameEdit->text().trimmed().toStdString(),
            .email = emailEdit->text().trimmed().toStdString(),
            .replyTo = {},
            .bcc = {},
            .textSignature = std::string{},
            .htmlSignature = std::string{},
            .mayDelete = true,
        };
        {
            const QSignalBlocker blocker{m_tree};
            m_tree->clearSelection();
            m_tree->setCurrentItem(nullptr);
        }
        presentIdentity(selectedAccount, identity, false, identity);
        m_editorDirty = true;
        updateActions();
        saveCurrentIdentity();
    }

    void SendingIdentitiesDialog::duplicateCurrentIdentity()
    {
        if (!m_editorIdentity.has_value() || m_editorPending || m_editorIdentity->id.empty())
            return;
        auto duplicate = *m_editorIdentity;
        duplicate.id.clear();
        duplicate.mayDelete = true;
        {
            const QSignalBlocker blocker{m_tree};
            m_tree->clearSelection();
            m_tree->setCurrentItem(nullptr);
        }
        presentIdentity(m_editorAccountId, duplicate, false, duplicate);
        m_editorDirty = true;
        updateActions();
    }

    void SendingIdentitiesDialog::saveCurrentIdentity()
    {
        if (!m_editorIdentity.has_value() || m_editorPending || m_busy)
            return;
        auto identity = *m_editorIdentity;
        identity.name = m_nameEdit->text().trimmed().toStdString();
        identity.email = m_emailEdit->text().trimmed().toStdString();
        if (!QString::fromStdString(identity.email).contains(QLatin1Char('@')))
        {
            QMessageBox::warning(this, i18n("Invalid Email Address"),
                                 i18n("Enter a valid sender email address."));
            return;
        }
        const auto replyTo = parseAddresses(m_replyToEdit->text());
        const auto bcc = parseAddresses(m_bccEdit->text());
        if (!replyTo.has_value() || !bcc.has_value())
        {
            QMessageBox::warning(this, i18n("Invalid Address"),
                                 i18n("Enter valid Reply-To and automatic-Bcc addresses."));
            return;
        }
        identity.replyTo = *replyTo;
        identity.bcc = *bcc;
        if (m_richTextCheck->isChecked())
        {
            identity.htmlSignature =
                signatureHtmlWithoutImages(m_signatureEdit->toCleanHtml()).toStdString();
            identity.textSignature = m_signatureEdit->toCleanPlainText().toStdString();
        }
        else
        {
            const auto plainText = m_signatureEdit->toCleanPlainText();
            identity.textSignature = plainText.toStdString();
            identity.htmlSignature = htmlFromPlainText(plainText).toStdString();
        }

        const auto accountId = m_editorAccountId;
        const auto revertIdentity = m_revertIdentity;
        setBusy(true);
        auto task = m_commandPort.saveSenderIdentity(accountId, identity);
        QCoro::connect(
            std::move(task), this,
            [this, accountId, identity,
             revertIdentity](javelin::jmap::identity::IdentitySaveResult result)
            {
                setBusy(false);
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                {
                    QMessageBox::critical(this, i18n("Unable to Save Identity"), error->message);
                    reloadTree();
                    presentIdentity(accountId, identity, false, revertIdentity);
                    m_editorDirty = true;
                    updateActions();
                    return;
                }
                const auto& saved = std::get<javelin::jmap::domain::Identity>(result);
                reloadTree(accountId, saved.id);
            });
    }

    void SendingIdentitiesDialog::revertCurrentIdentity()
    {
        if (!m_editorIdentity.has_value())
            return;
        if (!m_editorIdentity->id.empty())
        {
            const auto found = m_identityReader.find(m_editorAccountId, m_editorIdentity->id);
            if (const auto* identity =
                    std::get_if<std::optional<javelin::jmap::domain::Identity>>(&found);
                identity != nullptr && identity->has_value())
            {
                presentIdentity(m_editorAccountId, **identity, false, **identity);
                return;
            }
        }
        if (m_revertIdentity.has_value())
            presentIdentity(m_editorAccountId, *m_revertIdentity, false, m_revertIdentity);
    }

    void SendingIdentitiesDialog::deleteCurrentIdentity()
    {
        if (!m_editorIdentity.has_value() || m_editorPending || m_editorIdentity->id.empty() ||
            !m_editorIdentity->mayDelete || m_busy)
            return;
        if (QMessageBox::question(
                this, i18n("Delete Sending Identity?"),
                i18n("Delete %1 from the server? Existing drafts using this identity will need "
                     "another sender before they can be sent.",
                     identityAddressLabel(*m_editorIdentity)),
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
            return;
        const auto accountId = m_editorAccountId;
        const auto identityId = m_editorIdentity->id;
        setBusy(true);
        auto task = m_commandPort.deleteSenderIdentity(accountId, identityId);
        QCoro::connect(
            std::move(task), this,
            [this, accountId, identityId](javelin::jmap::identity::IdentityDeleteResult result)
            {
                setBusy(false);
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                {
                    QMessageBox::critical(this, i18n("Unable to Delete Identity"), error->message);
                    reloadTree(accountId, identityId);
                    return;
                }
                reloadTree(accountId, std::nullopt);
            });
    }

    void SendingIdentitiesDialog::refreshCurrentAccount()
    {
        const auto accountId = selectedAccountId().or_else(
            [this]() -> std::optional<std::string>
            {
                if (!m_editorAccountId.empty())
                    return m_editorAccountId;
                if (!m_accounts.empty())
                    return m_accounts.front().accountId;
                return std::nullopt;
            });
        if (!accountId.has_value() || m_busy)
            return;
        const auto identityId = selectedIdentityId();
        setBusy(true);
        auto task = m_commandPort.requestSenderIdentities(*accountId);
        QCoro::connect(std::move(task), this,
                       [this, accountId = *accountId,
                        identityId](javelin::jmap::identity::IdentityListResult result)
                       {
                           setBusy(false);
                           if (const auto* error =
                                   std::get_if<javelin::jmap::OperationError>(&result))
                           {
                               QMessageBox::critical(this, i18n("Unable to Refresh Identities"),
                                                     error->message);
                               return;
                           }
                           reloadTree(accountId, identityId);
                       });
    }

    void SendingIdentitiesDialog::switchEditorMode(const bool richText)
    {
        if (m_switchingMode || !m_editorIdentity.has_value())
            return;

        bool useMarkup = false;
        if (!richText && m_signatureEdit->composerControler()->isFormattingUsed())
        {
            QMessageBox warning{QMessageBox::Warning, i18n("Convert Signature to Plain Text"),
                                i18n("This signature contains formatting. How should it be "
                                     "converted to plain text?"),
                                QMessageBox::NoButton, this};
            QAbstractButton* loseFormatting = warning.addButton(
                i18nc("@action:button", "Lose Formatting"), QMessageBox::DestructiveRole);
            QPushButton* addMarkup = warning.addButton(
                i18nc("@action:button", "Add Markup Plain Text"), QMessageBox::AcceptRole);
            QAbstractButton* cancel = warning.addButton(QMessageBox::Cancel);
            warning.setDefaultButton(addMarkup);
            warning.exec();
            if (warning.clickedButton() == cancel || warning.clickedButton() == nullptr)
            {
                const QSignalBlocker blocker{m_richTextCheck};
                m_richTextCheck->setChecked(true);
                return;
            }
            useMarkup = warning.clickedButton() == addMarkup;
            Q_UNUSED(loseFormatting);
        }

        m_switchingMode = true;
        if (richText)
        {
            const auto plainText = m_signatureEdit->toCleanPlainText();
            m_signatureEdit->activateRichText();
            m_signatureEdit->setHtml(htmlFromPlainText(plainText));
        }
        else
        {
            m_signatureEdit->forcePlainTextMarkup(useMarkup);
            m_signatureEdit->switchToPlainText();
        }
        m_formatToolbar->setVisible(richText);
        m_switchingMode = false;
        m_editorDirty = true;
        updateActions();
    }

    void SendingIdentitiesDialog::setBusy(const bool busy)
    {
        m_busy = busy;
        m_tree->setEnabled(!busy);
        updateActions();
    }

    void SendingIdentitiesDialog::updateActions()
    {
        const bool hasIdentity = m_editorIdentity.has_value();
        const bool confirmed = hasIdentity && !m_editorPending && !m_editorIdentity->id.empty();
        const bool editable = hasIdentity && !m_editorPending && !m_busy;
        m_newButton->setEnabled(!m_busy && !m_accounts.empty());
        m_duplicateButton->setEnabled(!m_busy && confirmed);
        m_deleteButton->setEnabled(!m_busy && confirmed && m_editorIdentity->mayDelete);
        m_refreshButton->setEnabled(!m_busy && (selectedAccountId().has_value() ||
                                                !m_editorAccountId.empty() || !m_accounts.empty()));
        m_saveButton->setEnabled(editable && m_editorDirty);
        m_revertButton->setEnabled(editable && m_editorDirty);
        m_nameEdit->setEnabled(editable);
        m_emailEdit->setEnabled(editable && m_editorIdentity->id.empty());
        m_replyToEdit->setEnabled(editable);
        m_bccEdit->setEnabled(editable);
        m_richTextCheck->setEnabled(editable);
        m_signatureEdit->setEnabled(editable);
        m_signatureEdit->setEnableActions(editable && m_richTextCheck->isChecked());
        m_formatToolbar->setEnabled(editable);
    }

    std::optional<SendingIdentitiesDialog::AccountEntry>
    SendingIdentitiesDialog::accountEntry(const std::string_view accountId) const
    {
        const auto found = std::ranges::find(m_accounts, accountId, &AccountEntry::accountId);
        return found == m_accounts.end() ? std::nullopt : std::optional<AccountEntry>{*found};
    }

    std::optional<std::string> SendingIdentitiesDialog::selectedAccountId() const
    {
        const auto* item = m_tree->currentItem();
        if (item == nullptr)
            return std::nullopt;
        const auto value = item->data(0, accountIdRole).toString();
        return value.isEmpty() ? std::nullopt : std::optional<std::string>{value.toStdString()};
    }

    std::optional<std::string> SendingIdentitiesDialog::selectedIdentityId() const
    {
        const auto* item = m_tree->currentItem();
        if (item == nullptr ||
            static_cast<ItemKind>(item->data(0, itemKindRole).toInt()) != ItemKind::Identity)
            return std::nullopt;
        const auto value = item->data(0, identityIdRole).toString();
        return value.isEmpty() ? std::nullopt : std::optional<std::string>{value.toStdString()};
    }

    void SendingIdentitiesDialog::selectIdentity(std::string accountId, std::string identityId)
    {
        reloadTree(std::move(accountId), std::move(identityId));
    }
} // namespace javelin::gui::compose
