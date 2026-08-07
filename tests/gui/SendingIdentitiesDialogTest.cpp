#include "gui/compose/SendingIdentitiesDialog.h"

#include "app/IdentityApplicationPorts.h"
#include "app/MailApplicationEventsPorts.h"
#include "gui/settings/GuiSettings.h"
#include "jmap/cache/AccountReadRepository.h"
#include "jmap/cache/IdentityReader.h"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QPushButton>
#include <QTimer>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace
{
    class FakeAccountReader final : public javelin::jmap::cache::AccountReader
    {
      public:
        [[nodiscard]] std::variant<std::vector<javelin::jmap::cache::CachedAccount>,
                                   javelin::jmap::cache::DatabaseError>
        listAll() const override
        {
            return accounts();
        }

        [[nodiscard]] std::variant<std::vector<javelin::jmap::cache::CachedAccount>,
                                   javelin::jmap::cache::DatabaseError>
        listOwnedBy(const std::string_view ownerAccountId) const override
        {
            auto values = accounts();
            std::erase_if(values, [ownerAccountId](const auto& account)
                          { return account.ownerAccountId != ownerAccountId; });
            return values;
        }

        [[nodiscard]] std::variant<std::optional<javelin::jmap::cache::CachedAccount>,
                                   javelin::jmap::cache::DatabaseError>
        findById(const std::string_view accountId) const override
        {
            for (const auto& account : accounts())
            {
                if (account.accountId == accountId)
                    return std::optional<javelin::jmap::cache::CachedAccount>{account};
            }
            return std::optional<javelin::jmap::cache::CachedAccount>{};
        }

      private:
        [[nodiscard]] static std::vector<javelin::jmap::cache::CachedAccount> accounts()
        {
            return {{
                .accountId = "account-1",
                .name = "Test Account",
                .isPersonal = true,
                .isReadOnly = false,
                .isPrimary = true,
                .hasMailCapability = true,
                .ownerAccountId = "account-1",
                .hasSubmissionCapability = true,
            }};
        }
    };

    class FakeIdentityReader final : public javelin::jmap::cache::IdentityReader
    {
      public:
        std::unordered_map<std::string, std::vector<javelin::jmap::domain::Identity>> identities;

        [[nodiscard]] std::variant<std::vector<javelin::jmap::domain::Identity>,
                                   javelin::jmap::cache::DatabaseError>
        listByAccount(const std::string_view accountId) const override
        {
            const auto found = identities.find(std::string{accountId});
            return found == identities.end() ? std::vector<javelin::jmap::domain::Identity>{}
                                             : found->second;
        }

        [[nodiscard]] std::variant<std::optional<javelin::jmap::domain::Identity>,
                                   javelin::jmap::cache::DatabaseError>
        find(const std::string_view accountId, const std::string_view identityId) const override
        {
            const auto found = identities.find(std::string{accountId});
            if (found == identities.end())
                return std::optional<javelin::jmap::domain::Identity>{};
            for (const auto& identity : found->second)
            {
                if (identity.id == identityId)
                    return std::optional<javelin::jmap::domain::Identity>{identity};
            }
            return std::optional<javelin::jmap::domain::Identity>{};
        }

        [[nodiscard]] std::variant<std::vector<javelin::jmap::cache::PendingIdentityCreate>,
                                   javelin::jmap::cache::DatabaseError>
        listPendingCreates(std::string_view) const override
        {
            return std::vector<javelin::jmap::cache::PendingIdentityCreate>{};
        }
    };

    class TwoAccountReader final : public javelin::jmap::cache::AccountReader
    {
      public:
        [[nodiscard]] std::variant<std::vector<javelin::jmap::cache::CachedAccount>,
                                   javelin::jmap::cache::DatabaseError>
        listAll() const override
        {
            return accounts();
        }

        [[nodiscard]] std::variant<std::vector<javelin::jmap::cache::CachedAccount>,
                                   javelin::jmap::cache::DatabaseError>
        listOwnedBy(std::string_view) const override
        {
            return accounts();
        }

        [[nodiscard]] std::variant<std::optional<javelin::jmap::cache::CachedAccount>,
                                   javelin::jmap::cache::DatabaseError>
        findById(const std::string_view accountId) const override
        {
            for (const auto& account : accounts())
            {
                if (account.accountId == accountId)
                    return std::optional<javelin::jmap::cache::CachedAccount>{account};
            }
            return std::optional<javelin::jmap::cache::CachedAccount>{};
        }

      private:
        [[nodiscard]] static std::vector<javelin::jmap::cache::CachedAccount> accounts()
        {
            return {
                {.accountId = "account-1",
                 .name = "Account One",
                 .isPersonal = true,
                 .isReadOnly = false,
                 .isPrimary = true,
                 .hasMailCapability = true,
                 .ownerAccountId = "account-1",
                 .hasSubmissionCapability = true},
                {.accountId = "account-2",
                 .name = "Account Two",
                 .isPersonal = true,
                 .isReadOnly = false,
                 .isPrimary = false,
                 .hasMailCapability = true,
                 .ownerAccountId = "account-2",
                 .hasSubmissionCapability = true},
            };
        }
    };

    [[nodiscard]] javelin::jmap::domain::Identity
    identity(std::string id, std::string name, std::string email, std::string signature = {})
    {
        return {
            .id = std::move(id),
            .name = std::move(name),
            .email = std::move(email),
            .replyTo = {},
            .bcc = {},
            .textSignature = signature,
            .htmlSignature = signature.empty() ? std::string{} : "<p>" + signature + "</p>",
            .mayDelete = true,
        };
    }

    class FakeIdentityCommandPort final : public javelin::app::IdentityCommandPort
    {
      public:
        [[nodiscard]] QCoro::Task<javelin::jmap::identity::IdentityListResult>
        requestSenderIdentities(std::string) override
        {
            co_return javelin::jmap::identity::IdentitySnapshot{};
        }

        [[nodiscard]] QCoro::Task<javelin::jmap::identity::IdentitySaveResult>
        saveSenderIdentity(std::string accountId, javelin::jmap::domain::Identity identity) override
        {
            ++saveCalls;
            savedAccountId = std::move(accountId);
            savedIdentity = identity;
            identity.id = "created-identity";
            co_return identity;
        }

        [[nodiscard]] QCoro::Task<javelin::jmap::identity::IdentityDeleteResult>
        deleteSenderIdentity(std::string, std::string) override
        {
            co_return std::monostate{};
        }

        int saveCalls = 0;
        std::string savedAccountId;
        std::optional<javelin::jmap::domain::Identity> savedIdentity;
    };

    class FakeMailEvents final : public javelin::app::MailApplicationEventsPort
    {
      public:
        [[nodiscard]] std::unordered_map<std::string, javelin::app::MailAccountStatus>
        accountStatuses() const override
        {
            return {};
        }
    };
} // namespace

TEST_CASE("accepting a new sending identity submits an Identity create", "[gui][identity][create]")
{
    javelin::gui::settings::GuiSettings settings{javelin::protocol::SettingsSnapshot{}};
    FakeAccountReader accountReader;
    FakeIdentityReader identityReader;
    FakeIdentityCommandPort commandPort;
    FakeMailEvents mailEvents;
    javelin::gui::compose::SendingIdentitiesDialog dialog{settings, accountReader, identityReader,
                                                          commandPort, mailEvents};

    auto* newButton = dialog.findChild<QPushButton*>(QStringLiteral("identityNewButton"));
    REQUIRE(newButton != nullptr);
    REQUIRE(newButton->isEnabled());

    bool completedModal = false;
    QTimer::singleShot(0,
                       [&completedModal]
                       {
                           auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                           if (modal == nullptr)
                               return;
                           auto* nameEdit =
                               modal->findChild<QLineEdit*>(QStringLiteral("identityNewNameEdit"));
                           auto* emailEdit =
                               modal->findChild<QLineEdit*>(QStringLiteral("identityNewEmailEdit"));
                           auto* buttons = modal->findChild<QDialogButtonBox*>();
                           if (nameEdit == nullptr || emailEdit == nullptr || buttons == nullptr)
                               return;
                           auto* okButton = buttons->button(QDialogButtonBox::Ok);
                           if (okButton == nullptr)
                               return;

                           nameEdit->setText(QStringLiteral("New Sender"));
                           emailEdit->setText(QStringLiteral("new@example.test"));
                           completedModal = true;
                           okButton->click();
                       });

    newButton->click();
    QApplication::processEvents();

    REQUIRE(completedModal);
    REQUIRE(commandPort.saveCalls == 1);
    CHECK(commandPort.savedAccountId == "account-1");
    REQUIRE(commandPort.savedIdentity.has_value());
    CHECK(commandPort.savedIdentity->id.empty());
    CHECK(commandPort.savedIdentity->name == "New Sender");
    CHECK(commandPort.savedIdentity->email == "new@example.test");
}

TEST_CASE("new identity account changes update the default sender address",
          "[gui][identity][create][account]")
{
    javelin::gui::settings::GuiSettings settings{javelin::protocol::SettingsSnapshot{}};
    TwoAccountReader accountReader;
    FakeIdentityReader identityReader;
    identityReader.identities.emplace(
        "account-1", std::vector{identity("identity-1", "Sender One", "one@example.test")});
    identityReader.identities.emplace(
        "account-2", std::vector{identity("identity-2", "Sender Two", "two@example.test")});
    FakeIdentityCommandPort commandPort;
    FakeMailEvents mailEvents;
    javelin::gui::compose::SendingIdentitiesDialog dialog{settings, accountReader, identityReader,
                                                          commandPort, mailEvents};

    auto* newButton = dialog.findChild<QPushButton*>(QStringLiteral("identityNewButton"));
    REQUIRE(newButton != nullptr);

    bool initialAddressCorrect = false;
    bool switchedAddressCorrect = false;
    QTimer::singleShot(
        0,
        [&initialAddressCorrect, &switchedAddressCorrect]
        {
            auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (modal == nullptr)
                return;
            auto* accountCombo =
                modal->findChild<QComboBox*>(QStringLiteral("identityNewAccountCombo"));
            auto* emailEdit = modal->findChild<QLineEdit*>(QStringLiteral("identityNewEmailEdit"));
            auto* buttons = modal->findChild<QDialogButtonBox*>();
            if (accountCombo == nullptr || emailEdit == nullptr || buttons == nullptr)
                return;

            initialAddressCorrect = emailEdit->text() == QStringLiteral("one@example.test");
            const auto secondIndex = accountCombo->findData(QStringLiteral("account-2"));
            if (secondIndex >= 0)
                accountCombo->setCurrentIndex(secondIndex);
            switchedAddressCorrect = emailEdit->text() == QStringLiteral("two@example.test");
            buttons->button(QDialogButtonBox::Cancel)->click();
        });

    newButton->click();
    QApplication::processEvents();

    CHECK(initialAddressCorrect);
    CHECK(switchedAddressCorrect);
    CHECK(commandPort.saveCalls == 0);
}

TEST_CASE("duplicating a sending identity immediately submits a create",
          "[gui][identity][duplicate]")
{
    javelin::gui::settings::GuiSettings settings{javelin::protocol::SettingsSnapshot{}};
    FakeAccountReader accountReader;
    FakeIdentityReader identityReader;
    identityReader.identities.emplace(
        "account-1",
        std::vector{identity("identity-1", "Existing Sender", "sender@example.test", "Regards")});
    FakeIdentityCommandPort commandPort;
    FakeMailEvents mailEvents;
    javelin::gui::compose::SendingIdentitiesDialog dialog{settings, accountReader, identityReader,
                                                          commandPort, mailEvents};

    auto* duplicateButton =
        dialog.findChild<QPushButton*>(QStringLiteral("identityDuplicateButton"));
    REQUIRE(duplicateButton != nullptr);
    REQUIRE(duplicateButton->isEnabled());

    duplicateButton->click();
    QApplication::processEvents();

    REQUIRE(commandPort.saveCalls == 1);
    CHECK(commandPort.savedAccountId == "account-1");
    REQUIRE(commandPort.savedIdentity.has_value());
    CHECK(commandPort.savedIdentity->id.empty());
    CHECK(commandPort.savedIdentity->name == "Existing Sender");
    CHECK(commandPort.savedIdentity->email == "sender@example.test");
    CHECK(commandPort.savedIdentity->textSignature == std::optional<std::string>{"Regards"});
}
