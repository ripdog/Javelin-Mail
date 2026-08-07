#include "gui/compose/SendingIdentitiesDialog.h"

#include "app/IdentityApplicationPorts.h"
#include "app/MailApplicationEventsPorts.h"
#include "gui/settings/GuiSettings.h"
#include "jmap/cache/AccountReadRepository.h"
#include "jmap/cache/IdentityReader.h"

#include <QApplication>
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
        [[nodiscard]] std::variant<std::vector<javelin::jmap::domain::Identity>,
                                   javelin::jmap::cache::DatabaseError>
        listByAccount(std::string_view) const override
        {
            return std::vector<javelin::jmap::domain::Identity>{};
        }

        [[nodiscard]] std::variant<std::optional<javelin::jmap::domain::Identity>,
                                   javelin::jmap::cache::DatabaseError>
        find(std::string_view, std::string_view) const override
        {
            return std::optional<javelin::jmap::domain::Identity>{};
        }

        [[nodiscard]] std::variant<std::vector<javelin::jmap::cache::PendingIdentityCreate>,
                                   javelin::jmap::cache::DatabaseError>
        listPendingCreates(std::string_view) const override
        {
            return std::vector<javelin::jmap::cache::PendingIdentityCreate>{};
        }
    };

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
