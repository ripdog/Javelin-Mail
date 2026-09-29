#include "gui/messageview/MessageViewContainer.h"
#include "gui/messageview/HtmlMessageView.h"
#include "gui/settings/GuiSettings.h"
#include "gui/translation/GoogleTranslationBackend.h"
#include "gui/translation/TranslationCache.h"
#include "gui/translation/TranslationService.h"
#include "gui/translation/TranslationSettingsStore.h"
#include "jmap/cache/ContactReader.h"
#include "jmap/contacts/ContactIdentityLookup.h"

#include <KMessageWidget>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QKeyEvent>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QPromise>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QUrl>

#include <catch2/catch_test_macros.hpp>

namespace
{
    class StaticMessageViewReader final : public javelin::jmap::cache::MessageViewReader
    {
      public:
        explicit StaticMessageViewReader(javelin::jmap::cache::MessageViewSnapshot snapshot)
            : m_snapshot(std::move(snapshot))
        {
        }

        [[nodiscard]] javelin::jmap::cache::MessageViewResult load(std::string_view,
                                                                   std::string_view) const override
        {
            return std::optional<javelin::jmap::cache::MessageViewSnapshot>{m_snapshot};
        }

        [[nodiscard]] QFuture<javelin::jmap::cache::MessageViewResult>
        loadAsync(std::string, std::string) const override
        {
            QPromise<javelin::jmap::cache::MessageViewResult> promise;
            promise.start();
            auto future = promise.future();
            promise.addResult(load({}, {}));
            promise.finish();
            return future;
        }

      private:
        javelin::jmap::cache::MessageViewSnapshot m_snapshot;
    };

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

TEST_CASE("message view find footer and internal mailto routing",
          "[qtwebengine][gui][message-view]")
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

    QString activatedMailto;
    QObject::connect(&view, &javelin::gui::messageview::MessageViewContainer::mailtoActivated,
                     [&activatedMailto](const QString& uri) { activatedMailto = uri; });

    auto* plainTextView = view.findChild<QTextBrowser*>();
    REQUIRE(plainTextView != nullptr);
    plainTextView->anchorClicked(
        QUrl{QStringLiteral("mailto:plain@example.test?subject=Plain%20link")});
    CHECK(activatedMailto == QStringLiteral("mailto:plain@example.test?subject=Plain%20link"));

    auto* htmlView = view.findChild<javelin::gui::messageview::HtmlMessageView*>();
    REQUIRE(htmlView != nullptr);
    htmlView->linkActivated(QUrl{QStringLiteral("mailto:html@example.test?subject=HTML%20link")});
    CHECK(activatedMailto == QStringLiteral("mailto:html@example.test?subject=HTML%20link"));

    if (previousConfigHome.isNull())
    {
        qunsetenv("XDG_CONFIG_HOME");
    }
    else
    {
        qputenv("XDG_CONFIG_HOME", previousConfigHome);
    }
}

TEST_CASE("message view exposes detected authentication code as a banner action",
          "[qtwebengine][gui][message-view][auth-action]")
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

    javelin::jmap::cache::MessageViewSnapshot snapshot;
    snapshot.email.id = "email-1";
    snapshot.email.receivedAt = "2026-09-29T08:00:00Z";
    snapshot.email.subject = "Your verification code";
    snapshot.plainTextBody = javelin::jmap::cache::MessageBody{
        .kind = javelin::jmap::cache::MessageBodyKind::PlainText,
        .partId = "1",
        .isTruncated = false,
        .value = "Use 381942 to finish signing in.",
    };
    snapshot.authActionDetection = {
        .action =
            javelin::jmap::cache::MessageAuthAction{
                .kind = javelin::jmap::cache::MessageAuthActionKind::CopyCode,
                .value = QStringLiteral("381942"),
                .score = 8,
            },
        .diagnostics = {QStringLiteral("candidate score=8: authentication wording")},
    };
    StaticMessageViewReader reader{std::move(snapshot)};

    javelin::gui::messageview::MessageViewContainer view{settings, translationService,
                                                         contactIdentityLookup};
    view.show();
    view.setSelection(reader, std::string{"account-1"}, std::string{"inbox"},
                      std::string{"email-1"}, std::nullopt);
    for (int i = 0; i < 4; ++i)
        QApplication::processEvents();

    auto* banner = view.findChild<KMessageWidget*>(QStringLiteral("messageAuthActionBanner"));
    auto* primary = view.findChild<QAction*>(QStringLiteral("messageAuthActionPrimary"));
    auto* debug = view.findChild<QAction*>(QStringLiteral("messageAuthActionDebug"));
    REQUIRE(banner != nullptr);
    REQUIRE(primary != nullptr);
    REQUIRE(debug != nullptr);
    CHECK(banner->isVisible());
    CHECK(banner->text().contains(QStringLiteral("381942")));
    CHECK(primary->text().contains(QStringLiteral("Copy"), Qt::CaseInsensitive));

    QApplication::clipboard()->clear();
    primary->trigger();
    CHECK(QApplication::clipboard()->text() == QStringLiteral("381942"));

    debug->trigger();
    CHECK(QApplication::clipboard()->text().contains(QStringLiteral("decision=copy-code")));
    CHECK_FALSE(QApplication::clipboard()->text().contains(QStringLiteral("381942")));

    if (previousConfigHome.isNull())
    {
        qunsetenv("XDG_CONFIG_HOME");
    }
    else
    {
        qputenv("XDG_CONFIG_HOME", previousConfigHome);
    }
}
