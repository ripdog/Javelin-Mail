#include "gui/messageview/MessageViewContainer.h"
#include "gui/settings/GuiSettings.h"
#include "gui/translation/GoogleTranslationBackend.h"
#include "gui/translation/TranslationCache.h"
#include "gui/translation/TranslationService.h"
#include "gui/translation/TranslationSettingsStore.h"
#include "jmap/cache/ContactReader.h"
#include "jmap/contacts/ContactIdentityLookup.h"

#include <QApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

namespace
{
    class EmptyContactReader final : public javelin::jmap::cache::ContactReader
    {
      public:
        QMetaObject::Connection connectChanged(QObject*,
                                               std::function<void(const QString&)>) override
        {
            return {};
        }

        std::variant<std::vector<javelin::jmap::api::AddressBook>,
                     javelin::jmap::cache::DatabaseError>
        listAddressBooks(std::string_view, bool) const override
        {
            return std::vector<javelin::jmap::api::AddressBook>{};
        }

        std::variant<std::optional<std::string>, javelin::jmap::cache::DatabaseError>
        addressBookState(std::string_view) const override
        {
            return std::optional<std::string>{std::nullopt};
        }

        std::variant<std::optional<std::string>, javelin::jmap::cache::DatabaseError>
        contactState(std::string_view) const override
        {
            return std::optional<std::string>{std::nullopt};
        }

        std::variant<std::vector<javelin::jmap::cache::ContactAccount>,
                     javelin::jmap::cache::DatabaseError>
        listAccounts(std::optional<std::string_view>) const override
        {
            return std::vector<javelin::jmap::cache::ContactAccount>{};
        }

        std::variant<std::vector<javelin::jmap::contacts::ContactSummary>,
                     javelin::jmap::cache::DatabaseError>
        listContacts(std::string_view, std::optional<std::string_view>,
                     std::string_view) const override
        {
            return std::vector<javelin::jmap::contacts::ContactSummary>{};
        }

        std::variant<std::optional<javelin::jmap::contacts::ContactSummary>,
                     javelin::jmap::cache::DatabaseError>
        findContact(std::string_view, std::string_view) const override
        {
            return std::optional<javelin::jmap::contacts::ContactSummary>{std::nullopt};
        }

        std::variant<std::optional<javelin::jmap::contacts::ContactSummary>,
                     javelin::jmap::cache::DatabaseError>
        findByEmail(std::string_view, std::optional<std::string_view>) const override
        {
            return std::optional<javelin::jmap::contacts::ContactSummary>{std::nullopt};
        }

        std::variant<std::vector<std::string>, javelin::jmap::cache::DatabaseError>
        listEmailAddresses() const override
        {
            return std::vector<std::string>{};
        }
    };
} // namespace

TEST_CASE("message view constructs with the find footer", "[qtwebengine]")
{
    QTemporaryDir temporaryDirectory;
    REQUIRE(temporaryDirectory.isValid());

    const QByteArray previousConfigHome = qgetenv("XDG_CONFIG_HOME");
    qputenv("XDG_CONFIG_HOME", temporaryDirectory.path().toUtf8());

    javelin::gui::settings::GuiSettings settings{javelin::protocol::SettingsSnapshot{}};
    javelin::gui::translation::TranslationSettingsStore translationSettings;
    javelin::gui::translation::TranslationCache translationCache{
        temporaryDirectory.filePath(QStringLiteral("translations.sqlite"))};
    QNetworkAccessManager networkAccessManager;
    javelin::gui::translation::GoogleTranslationBackend googleBackend{networkAccessManager};
    javelin::gui::translation::TranslationService translationService{
        translationSettings, translationCache, googleBackend, std::string{}};
    EmptyContactReader contactReader;
    javelin::jmap::contacts::ContactIdentityLookup contactIdentityLookup{contactReader};

    javelin::gui::messageview::MessageViewContainer view{settings, translationService,
                                                         contactIdentityLookup};
    CHECK_FALSE(view.readerActionsAvailable());

    auto* findBar = view.findChild<QWidget*>(QStringLiteral("messageFindBar"));
    auto* findEdit = view.findChild<QLineEdit*>(QStringLiteral("messageFindEdit"));
    REQUIRE(findBar != nullptr);
    REQUIRE(findEdit != nullptr);

    view.show();
    findBar->show();
    findEdit->setFocus(Qt::ShortcutFocusReason);
    QApplication::processEvents();
    REQUIRE(findEdit->hasFocus());

    QKeyEvent escapePress{QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier};
    QApplication::sendEvent(findEdit, &escapePress);
    CHECK(findBar->isHidden());

    if (previousConfigHome.isNull())
    {
        qunsetenv("XDG_CONFIG_HOME");
    }
    else
    {
        qputenv("XDG_CONFIG_HOME", previousConfigHome);
    }
}
