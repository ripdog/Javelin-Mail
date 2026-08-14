#include "gui/contacts/ContactDetailsView.h"

#include "gui/IconUtils.h"

#include <KLocalizedString>

#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPalette>
#include <QPixmap>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include <ranges>
#include <unordered_map>
#include <utility>
#include <variant>

namespace javelin::gui::contacts
{
    ContactDetailsView::ContactDetailsView(
        QLabel& name, QLabel& location, QToolButton& star, QLabel& photo, QLabel& editorPhoto,
        QWidget& cardContainer, QVBoxLayout& cardLayout,
        javelin::jmap::cache::ContactReader& repository,
        const std::vector<javelin::jmap::cache::ContactAccount>& accounts,
        std::function<QString()> currentAccountId,
        std::function<void(QString, QString, QString)> composeMail,
        std::function<void(QString, QString)> searchMailFrom)
        : m_name(name), m_location(location), m_star(star), m_photo(photo),
          m_editorPhoto(editorPhoto), m_cardContainer(cardContainer), m_cardLayout(cardLayout),
          m_repository(repository), m_accounts(accounts),
          m_currentAccountId(std::move(currentAccountId)), m_composeMail(std::move(composeMail)),
          m_searchMailFrom(std::move(searchMailFrom))
    {
    }

    void ContactDetailsView::showIdentity(const javelin::jmap::contacts::ContactSummary& contact,
                                          const QString& locationText, const bool starredEnabled)
    {
        m_name.setText(QString::fromStdString(contact.displayName));
        m_location.setText(locationText);
        m_star.setChecked(contact.isImportant);
        m_star.setEnabled(starredEnabled);
    }

    void ContactDetailsView::populateCards(const javelin::jmap::contacts::ContactSummary& contact)
    {
        while (auto* item = m_cardLayout.takeAt(0))
        {
            delete item->widget();
            delete item;
        }

        const auto parsed = javelin::jmap::contacts::contactEditorData(contact.document);
        const auto* editorData = std::get_if<javelin::jmap::contacts::ContactEditorData>(&parsed);
        if (editorData == nullptr)
        {
            m_cardLayout.addWidget(
                new QLabel(i18n("This contact could not be displayed."), &m_cardContainer));
            return;
        }

        const auto addCard = [this](const QString& title, const QString& value,
                                    const std::optional<QString>& email = std::nullopt,
                                    const QString& name = QString{})
        {
            auto* card = new QWidget(&m_cardContainer);
            card->setObjectName(QStringLiteral("contactCard"));
            auto* layout = new QVBoxLayout(card);
            auto* header = new QHBoxLayout();
            auto* heading = new QLabel(title, card);
            heading->setObjectName(QStringLiteral("contactCardTitle"));
            header->addWidget(heading);
            header->addStretch(1);
            if (email.has_value())
            {
                auto* compose = new QToolButton(card);
                compose->setIcon(javelin::gui::themedSvgIcon(
                    QStringLiteral(":/icons/thunderbird-icons/new-mail.svg"),
                    compose->palette().color(QPalette::Active, QPalette::ButtonText)));
                compose->setAccessibleName(i18n("Compose mail"));
                compose->setToolTip(i18n("Compose mail"));
                QObject::connect(compose, &QToolButton::clicked, card, [this, email = *email, name]
                                 { m_composeMail(m_currentAccountId(), name, email); });
                header->addWidget(compose);
                auto* search = new QToolButton(card);
                search->setIcon(QIcon::fromTheme(QStringLiteral("edit-find")));
                search->setToolTip(i18n("Find mail from this address"));
                search->setAccessibleName(search->toolTip());
                QObject::connect(search, &QToolButton::clicked, card, [this, email = *email]
                                 { m_searchMailFrom(m_currentAccountId(), email); });
                header->addWidget(search);
            }
            auto* copy = new QToolButton(card);
            copy->setText(i18nc("@action:button", "Copy"));
            copy->setToolTip(i18n("Copy to clipboard"));
            QObject::connect(copy, &QToolButton::clicked, card,
                             [value] { QApplication::clipboard()->setText(value); });
            header->addWidget(copy);
            layout->addLayout(header);
            auto* content = new QLabel(value, card);
            content->setTextInteractionFlags(Qt::TextSelectableByMouse);
            content->setWordWrap(true);
            layout->addWidget(content);
            m_cardLayout.addWidget(card);
        };

        if (editorData->kind == "group" && !editorData->members.empty())
        {
            std::unordered_map<std::string, QString> memberNames;
            for (const auto& account : m_accounts)
            {
                const auto listed = m_repository.listContacts(account.accountId);
                const auto* contacts =
                    std::get_if<std::vector<javelin::jmap::contacts::ContactSummary>>(&listed);
                if (contacts == nullptr)
                    continue;
                for (const auto& member : *contacts)
                {
                    if (member.kind != "group" &&
                        std::ranges::contains(editorData->members, member.uid) &&
                        !memberNames.contains(member.uid))
                        memberNames.emplace(member.uid, QString::fromStdString(member.displayName));
                }
            }
            QStringList members;
            for (const auto& uid : editorData->members)
            {
                if (const auto found = memberNames.find(uid); found != memberNames.end())
                    members.push_back(found->second);
                else
                    members.push_back(
                        i18n("%1 (currently unavailable)", QString::fromStdString(uid)));
            }
            addCard(i18nc("@title contact information card", "Members"),
                    members.join(QLatin1Char('\n')));
        }

        if (editorData->kind != "group" &&
            (!editorData->organization.empty() || !editorData->title.empty()))
        {
            QStringList identity;
            if (!editorData->organization.empty())
                identity << QString::fromStdString(editorData->organization);
            if (!editorData->title.empty())
                identity << QString::fromStdString(editorData->title);
            addCard(i18nc("@title contact information card", "Work"),
                    identity.join(QLatin1Char('\n')));
        }

        const auto fieldTitle =
            [](const QString& fallback, const javelin::jmap::contacts::ContactEditorField& field)
        {
            if (field.label.has_value() && !field.label->empty())
                return QString::fromStdString(*field.label);
            if (const auto work = field.contexts.find("work");
                work != field.contexts.end() && work->second)
                return i18nc("@title contact field in work context", "%1 · Work", fallback);
            if (const auto home = field.contexts.find("private");
                home != field.contexts.end() && home->second)
                return i18nc("@title contact field in home context", "%1 · Home", fallback);
            return fallback;
        };

        for (const auto& email : editorData->emails)
            addCard(fieldTitle(i18nc("@title contact information card", "Email"), email),
                    QString::fromStdString(email.value), QString::fromStdString(email.value),
                    QString::fromStdString(editorData->fullName));
        for (const auto& phone : editorData->phones)
            addCard(fieldTitle(i18nc("@title contact information card", "Phone"), phone),
                    QString::fromStdString(phone.value));
        for (const auto& address : editorData->addresses)
            addCard(fieldTitle(i18nc("@title contact information card", "Address"), address),
                    QString::fromStdString(address.value));
        if (editorData->kind != "group" && !editorData->birthday.empty())
            addCard(i18nc("@title contact information card", "Birthday"),
                    QString::fromStdString(editorData->birthday));
        if (!editorData->notes.empty())
            addCard(i18nc("@title contact information card", "Notes"),
                    QString::fromStdString(editorData->notes));
        m_cardLayout.addStretch(1);
    }

    void ContactDetailsView::clearPhoto()
    {
        m_photo.clear();
        m_photo.setVisible(false);
        m_editorPhoto.clear();
        m_editorPhoto.setVisible(false);
    }

    void ContactDetailsView::showPhoto(const QByteArray& payload)
    {
        QPixmap pixmap;
        if (!pixmap.loadFromData(payload))
            return;
        m_photo.setPixmap(
            pixmap.scaled(m_photo.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
        m_photo.setVisible(true);
        m_editorPhoto.setPixmap(
            pixmap.scaled(m_editorPhoto.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
        m_editorPhoto.setVisible(true);
    }
} // namespace javelin::gui::contacts
