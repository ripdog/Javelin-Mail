#include "app/TranslationService.h"

#include "jmap/cache/Database.h"
#include "jmap/cache/TranslationCacheRepository.h"

#include <QCoroTask>
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QNetworkAccessManager>
#include <QSettings>
#include <QTemporaryDir>

#include <memory>
#include <variant>

namespace
{
    class ApplicationGuard
    {
      public:
        ApplicationGuard()
        {
            if (QCoreApplication::instance() != nullptr)
            {
                return;
            }

            static int argc = 1;
            static char appName[] = "javelin-tests";
            static char* argv[] = {appName, nullptr};
            m_application = std::make_unique<QCoreApplication>(argc, argv);
        }

      private:
        std::unique_ptr<QCoreApplication> m_application;
    };

    class ApplicationIdentityGuard
    {
      public:
        ApplicationIdentityGuard()
            : m_applicationName(QCoreApplication::applicationName()),
              m_organizationName(QCoreApplication::organizationName())
        {
        }

        ~ApplicationIdentityGuard()
        {
            QCoreApplication::setApplicationName(m_applicationName);
            QCoreApplication::setOrganizationName(m_organizationName);
        }

      private:
        QString m_applicationName;
        QString m_organizationName;
    };

    void useTemporarySettings(const QTemporaryDir& directory)
    {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
        QSettings{}.clear();
    }

    [[nodiscard]] QString makeConnectionName()
    {
        static int counter = 0;
        return QStringLiteral("javelin-translation-service-%1").arg(++counter);
    }
} // namespace

TEST_CASE("translation settings persist provider overrides and normalized auto rules",
          "[app][translation][settings]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    QTemporaryDir settingsDirectory;
    REQUIRE(settingsDirectory.isValid());
    useTemporarySettings(settingsDirectory);

    javelin::app::TranslationService::saveSettings({
        .enabled = false,
        .apiKeyOverride = QStringLiteral(" custom-key "),
        .targetLanguage = QStringLiteral("JA"),
        .autoTranslateSenders = {QStringLiteral(" Sender@Example.test "),
                                 QStringLiteral("sender@example.test")},
        .autoTranslateDomains = {QStringLiteral(" Example.test ")},
    });

    const auto settings = javelin::app::TranslationService::loadSettings();
    CHECK_FALSE(settings.enabled);
    CHECK(settings.apiKeyOverride == QStringLiteral("custom-key"));
    CHECK(settings.targetLanguage == QStringLiteral("ja"));
    CHECK(settings.autoTranslateSenders == QStringList{QStringLiteral("sender@example.test")});
    CHECK(settings.autoTranslateDomains == QStringList{QStringLiteral("example.test")});
}

TEST_CASE("translation service uses the configured target-language cache before the network",
          "[app][translation][cache]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    QTemporaryDir settingsDirectory;
    QTemporaryDir cacheDirectory;
    REQUIRE(settingsDirectory.isValid());
    REQUIRE(cacheDirectory.isValid());
    useTemporarySettings(settingsDirectory);
    javelin::app::TranslationService::saveSettings({
        .enabled = true,
        .apiKeyOverride = {},
        .targetLanguage = QStringLiteral("ja"),
        .autoTranslateSenders = {},
        .autoTranslateDomains = {},
    });

    auto connectionResult = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = makeConnectionName(),
        .databasePath = cacheDirectory.filePath(QStringLiteral("cache.sqlite3")),
    });
    if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&connectionResult))
    {
        FAIL(error->message.toStdString());
    }
    auto connection =
        std::get<javelin::jmap::cache::DatabaseConnection>(std::move(connectionResult));
    javelin::jmap::cache::TranslationCacheRepository repository{connection};
    REQUIRE_FALSE(repository
                      .upsert({
                          .sourceLanguage = QStringLiteral("auto"),
                          .targetLanguage = QStringLiteral("ja"),
                          .inputText = QStringLiteral("Hello"),
                          .translatedText = QStringLiteral("こんにちは"),
                      })
                      .has_value());

    QNetworkAccessManager networkAccessManager;
    javelin::app::TranslationService service{networkAccessManager, repository};
    javelin::app::TranslationService::TranslationChunks chunks;
    chunks.push_back(QStringList{QStringLiteral("Hello")});
    const auto result =
        QCoro::waitFor(service.translate(std::move(chunks), QStringLiteral("auto"), false));

    REQUIRE(std::holds_alternative<javelin::app::TranslationService::TranslationChunks>(result));
    const auto& translated = std::get<javelin::app::TranslationService::TranslationChunks>(result);
    REQUIRE(translated.size() == 1);
    CHECK(translated.front() == QStringList{QStringLiteral("こんにちは")});

    javelin::app::TranslationService::TranslationChunks missing;
    missing.push_back(QStringList{QStringLiteral("Not cached")});
    const auto miss =
        QCoro::waitFor(service.translate(std::move(missing), QStringLiteral("auto"), false));
    CHECK(std::holds_alternative<javelin::app::TranslationUnavailable>(miss));

    auto disabledSettings = service.settings();
    disabledSettings.enabled = false;
    javelin::app::TranslationService::saveSettings(std::move(disabledSettings));
    service.reloadSettings();
    javelin::app::TranslationService::TranslationChunks disabledChunks;
    disabledChunks.push_back(QStringList{QStringLiteral("Hello")});
    const auto disabled =
        QCoro::waitFor(service.translate(std::move(disabledChunks), QStringLiteral("auto"), false));
    REQUIRE(std::holds_alternative<QString>(disabled));
    CHECK(std::get<QString>(disabled).contains(QStringLiteral("disabled"), Qt::CaseInsensitive));
}

TEST_CASE("translation service accepts daemon-owned settings without consulting QSettings",
          "[app][translation][settings]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    QTemporaryDir settingsDirectory;
    QTemporaryDir cacheDirectory;
    REQUIRE(settingsDirectory.isValid());
    REQUIRE(cacheDirectory.isValid());
    useTemporarySettings(settingsDirectory);
    javelin::app::TranslationService::saveSettings({
        .enabled = false,
        .apiKeyOverride = {},
        .targetLanguage = QStringLiteral("en"),
        .autoTranslateSenders = {},
        .autoTranslateDomains = {},
    });

    auto connectionResult = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = makeConnectionName(),
        .databasePath = cacheDirectory.filePath(QStringLiteral("cache.sqlite3")),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(connectionResult));
    auto connection =
        std::get<javelin::jmap::cache::DatabaseConnection>(std::move(connectionResult));
    javelin::jmap::cache::TranslationCacheRepository repository{connection};
    QNetworkAccessManager networkAccessManager;
    javelin::app::TranslationService service{networkAccessManager, repository};
    REQUIRE_FALSE(service.isEnabled());

    service.applySettings({
        .enabled = true,
        .apiKeyOverride = QStringLiteral(" override "),
        .targetLanguage = QStringLiteral("JA"),
        .autoTranslateSenders = {QStringLiteral(" Sender@Example.test ")},
        .autoTranslateDomains = {QStringLiteral(" Example.test ")},
    });

    CHECK(service.isEnabled());
    CHECK(service.targetLanguage() == QStringLiteral("ja"));
    CHECK(service.settings().apiKeyOverride == QStringLiteral("override"));
    CHECK(service.shouldAutoTranslate(QStringLiteral("sender@example.test"), QString{}));
    CHECK(service.shouldAutoTranslate(QString{}, QStringLiteral("example.test")));
}

TEST_CASE("translation service reloads settings after the application identity is finalized",
          "[app][translation][settings]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    ApplicationIdentityGuard identity;
    QTemporaryDir settingsDirectory;
    QTemporaryDir cacheDirectory;
    REQUIRE(settingsDirectory.isValid());
    REQUIRE(cacheDirectory.isValid());
    useTemporarySettings(settingsDirectory);

    QCoreApplication::setOrganizationName(QStringLiteral("Javelin Mail"));
    QCoreApplication::setApplicationName(QStringLiteral("Javelin Mail"));
    javelin::app::TranslationService::saveSettings({
        .enabled = true,
        .apiKeyOverride = {},
        .targetLanguage = QStringLiteral("en"),
        .autoTranslateSenders = {},
        .autoTranslateDomains = {},
    });

    auto connectionResult = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = makeConnectionName(),
        .databasePath = cacheDirectory.filePath(QStringLiteral("cache.sqlite3")),
    });
    if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&connectionResult))
    {
        FAIL(error->message.toStdString());
    }
    auto connection =
        std::get<javelin::jmap::cache::DatabaseConnection>(std::move(connectionResult));
    javelin::jmap::cache::TranslationCacheRepository repository{connection};
    QNetworkAccessManager networkAccessManager;
    javelin::app::TranslationService service{networkAccessManager, repository};

    QCoreApplication::setApplicationName(QStringLiteral("javelinmail"));
    javelin::app::TranslationService::saveSettings({
        .enabled = true,
        .apiKeyOverride = {},
        .targetLanguage = QStringLiteral("en"),
        .autoTranslateSenders = {QStringLiteral("no-reply@ci-en.net")},
        .autoTranslateDomains = {},
    });

    CHECK_FALSE(service.shouldAutoTranslate(QStringLiteral("no-reply@ci-en.net"),
                                            QStringLiteral("ci-en.net")));
    service.reloadSettings();
    CHECK(service.shouldAutoTranslate(QStringLiteral("no-reply@ci-en.net"),
                                      QStringLiteral("ci-en.net")));
}
