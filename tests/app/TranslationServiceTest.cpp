#include "gui/translation/TranslationService.h"
#include "gui/translation/GoogleTranslationBackend.h"
#include "gui/translation/TranslationBackend.h"
#include "gui/translation/TranslationCache.h"
#include "gui/translation/TranslationSettingsStore.h"

#include <QCoroTask>
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QNetworkAccessManager>
#include <QSettings>
#include <QTemporaryDir>

#include <memory>
#include <optional>
#include <utility>
#include <variant>

namespace
{
    using namespace javelin::gui::translation;

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
            static char appName[] = "javelin-translation-tests";
            static char* argv[] = {appName, nullptr};
            m_application = std::make_unique<QCoreApplication>(argc, argv);
        }

      private:
        std::unique_ptr<QCoreApplication> m_application;
    };

    void useTemporarySettings(const QTemporaryDir& directory)
    {
        QCoreApplication::setOrganizationName(QStringLiteral("Javelin Mail Tests"));
        QCoreApplication::setApplicationName(QStringLiteral("translation-tests"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
        QSettings settings;
        settings.clear();
        settings.sync();
    }

    class FakeBackend final : public TranslationBackend
    {
      public:
        [[nodiscard]] QString revision(QStringView sourceLanguage,
                                       QStringView targetLanguage) const override
        {
            return QStringLiteral("fake-v1:%1-%2").arg(sourceLanguage, targetLanguage);
        }

        [[nodiscard]] QCoro::Task<BackendResult> translate(BackendRequest request) override
        {
            ++calls;
            receivedTexts += request.texts.size();
            lastPolicy = request.fetchPolicy;
            if (request.fetchPolicy != ExternalFetchPolicy::AllowExternalFetch)
            {
                co_return TranslationUnavailable{
                    .reason = TranslationUnavailable::Reason::ExternalFetchNotAllowed,
                };
            }

            QVector<QString> translated;
            translated.reserve(request.texts.size());
            for (const auto& text : request.texts)
            {
                translated.push_back(QStringLiteral("translated:%1").arg(text));
            }
            co_return BackendTranslation{
                .texts = std::move(translated),
                .backendRevision = revision(request.sourceLanguage, request.targetLanguage),
            };
        }

        void releaseResources() override
        {
            ++asynchronousReleases;
        }

        void releaseResourcesAndWait() override
        {
            ++synchronousReleases;
        }

        int calls = 0;
        int asynchronousReleases = 0;
        int synchronousReleases = 0;
        qsizetype receivedTexts = 0;
        ExternalFetchPolicy lastPolicy = ExternalFetchPolicy::InstalledAndCachedOnly;
    };

    struct ServiceFixture
    {
        explicit ServiceFixture(const QString& cachePath,
                                TranslationBackend* localBackend = nullptr)
            : cache(cachePath), google(network),
              service(settingsStore, cache, google, std::string{}, localBackend)
        {
        }

        TranslationSettingsStore settingsStore;
        TranslationCache cache;
        QNetworkAccessManager network;
        GoogleTranslationBackend google;
        TranslationService service;
    };
} // namespace

TEST_CASE("translation settings migrate the legacy enabled flag and normalize values",
          "[translation][settings]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    QTemporaryDir settingsDirectory;
    REQUIRE(settingsDirectory.isValid());
    useTemporarySettings(settingsDirectory);

    QSettings legacy;
    legacy.beginGroup(QStringLiteral("translation"));
    legacy.setValue(QStringLiteral("enabled"), false);
    legacy.setValue(QStringLiteral("apiKeyOverride"), QStringLiteral(" custom-key "));
    legacy.setValue(QStringLiteral("targetLanguage"), QStringLiteral("zh_cn"));
    legacy.setValue(QStringLiteral("autoTranslateSenders"),
                    QStringList{QStringLiteral(" Sender@Example.test "),
                                QStringLiteral("sender@example.test")});
    legacy.setValue(QStringLiteral("autoTranslateDomains"),
                    QStringList{QStringLiteral(" Example.test ")});
    legacy.endGroup();
    legacy.sync();

    TranslationSettingsStore store;
    const auto loaded = store.load();
    REQUIRE(std::holds_alternative<TranslationSettings>(loaded));
    const auto& settings = std::get<TranslationSettings>(loaded);
    CHECK(settings.provider == TranslationProvider::Disabled);
    CHECK(settings.apiKeyOverride == QStringLiteral("custom-key"));
    CHECK(settings.targetLanguage == QStringLiteral("zh-Hans"));
    CHECK(settings.autoTranslateSenders == QStringList{QStringLiteral("sender@example.test")});
    CHECK(settings.autoTranslateDomains == QStringList{QStringLiteral("example.test")});

    QSettings migrated;
    migrated.beginGroup(QStringLiteral("translation"));
    CHECK(migrated.value(QStringLiteral("provider")).toString() == QStringLiteral("disabled"));
    CHECK_FALSE(migrated.contains(QStringLiteral("enabled")));
    migrated.endGroup();
}

TEST_CASE("translation service deduplicates misses and caches backend results",
          "[translation][service][cache]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    QTemporaryDir settingsDirectory;
    QTemporaryDir cacheDirectory;
    REQUIRE(settingsDirectory.isValid());
    REQUIRE(cacheDirectory.isValid());
    useTemporarySettings(settingsDirectory);

    TranslationSettingsStore store;
    REQUIRE_FALSE(store
                      .save({
                          .provider = TranslationProvider::Local,
                          .apiKeyOverride = {},
                          .targetLanguage = QStringLiteral("en"),
                          .autoTranslateSenders = {},
                          .autoTranslateDomains = {},
                      })
                      .has_value());
    FakeBackend backend;
    ServiceFixture fixture{cacheDirectory.filePath(QStringLiteral("cache.sqlite3")), &backend};

    TranslationChunks chunks{
        QStringList{QStringLiteral("Bonjour"), QStringLiteral("Bonjour")},
        QStringList{QStringLiteral("Monde")},
    };
    const auto translated = QCoro::waitFor(fixture.service.translate(
        chunks, QStringLiteral("fr"), ExternalFetchPolicy::AllowExternalFetch));
    REQUIRE(std::holds_alternative<TranslationChunks>(translated));
    CHECK(
        std::get<TranslationChunks>(translated) ==
        TranslationChunks{
            QStringList{QStringLiteral("translated:Bonjour"), QStringLiteral("translated:Bonjour")},
            QStringList{QStringLiteral("translated:Monde")},
        });
    CHECK(backend.calls == 1);
    CHECK(backend.receivedTexts == 2);

    const auto cached = QCoro::waitFor(fixture.service.translate(
        chunks, QStringLiteral("fr"), ExternalFetchPolicy::InstalledAndCachedOnly));
    REQUIRE(std::holds_alternative<TranslationChunks>(cached));
    CHECK(std::get<TranslationChunks>(cached) == std::get<TranslationChunks>(translated));
    CHECK(backend.calls == 1);
}

TEST_CASE("automatic translation does not fetch without an explicit saved rule",
          "[translation][service][policy]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    QTemporaryDir settingsDirectory;
    QTemporaryDir cacheDirectory;
    REQUIRE(settingsDirectory.isValid());
    REQUIRE(cacheDirectory.isValid());
    useTemporarySettings(settingsDirectory);

    TranslationSettingsStore store;
    REQUIRE_FALSE(store
                      .save({
                          .provider = TranslationProvider::Local,
                          .apiKeyOverride = {},
                          .targetLanguage = QStringLiteral("en"),
                          .autoTranslateSenders = {},
                          .autoTranslateDomains = {},
                      })
                      .has_value());
    FakeBackend backend;
    ServiceFixture fixture{cacheDirectory.filePath(QStringLiteral("cache.sqlite3")), &backend};

    const auto result = QCoro::waitFor(fixture.service.translate(
        TranslationChunks{QStringList{QStringLiteral("Bonjour")}}, QStringLiteral("fr"),
        ExternalFetchPolicy::InstalledAndCachedOnly));
    REQUIRE(std::holds_alternative<TranslationUnavailable>(result));
    CHECK(std::get<TranslationUnavailable>(result).reason ==
          TranslationUnavailable::Reason::ExternalFetchNotAllowed);
    CHECK(backend.calls == 1);

    REQUIRE_FALSE(
        fixture.service.setAutoTranslateDomain(QStringLiteral("Example.test"), true).has_value());
    CHECK(fixture.service.shouldAutoTranslate(QString{}, QStringLiteral("example.test")));
}

TEST_CASE("leaving local translation releases models without blocking settings changes",
          "[translation][service][lifecycle]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    QTemporaryDir settingsDirectory;
    QTemporaryDir cacheDirectory;
    REQUIRE(settingsDirectory.isValid());
    REQUIRE(cacheDirectory.isValid());
    useTemporarySettings(settingsDirectory);

    TranslationSettingsStore store;
    REQUIRE_FALSE(store
                      .save({
                          .provider = TranslationProvider::Local,
                          .apiKeyOverride = {},
                          .targetLanguage = QStringLiteral("en"),
                          .autoTranslateSenders = {},
                          .autoTranslateDomains = {},
                      })
                      .has_value());
    FakeBackend backend;
    {
        ServiceFixture fixture{cacheDirectory.filePath(QStringLiteral("cache.sqlite3")), &backend};
        REQUIRE_FALSE(fixture.service
                          .saveSettings({
                              .provider = TranslationProvider::Google,
                              .apiKeyOverride = {},
                              .targetLanguage = QStringLiteral("en"),
                              .autoTranslateSenders = {},
                              .autoTranslateDomains = {},
                          })
                          .has_value());
        CHECK(backend.asynchronousReleases == 1);
        CHECK(backend.synchronousReleases == 0);
    }
    CHECK(backend.synchronousReleases == 1);
}

TEST_CASE("translation route offers follow the active provider", "[translation][service][policy]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    QTemporaryDir settingsDirectory;
    QTemporaryDir cacheDirectory;
    REQUIRE(settingsDirectory.isValid());
    REQUIRE(cacheDirectory.isValid());
    useTemporarySettings(settingsDirectory);

    ServiceFixture fixture{cacheDirectory.filePath(QStringLiteral("cache.sqlite3"))};
    CHECK(fixture.service.supportsTranslationRoute(QStringLiteral("fr")));
    CHECK_FALSE(fixture.service.supportsTranslationRoute(QStringLiteral("en")));
    REQUIRE_FALSE(fixture.service
                      .saveSettings({
                          .provider = TranslationProvider::Disabled,
                          .apiKeyOverride = {},
                          .targetLanguage = QStringLiteral("en"),
                          .autoTranslateSenders = {},
                          .autoTranslateDomains = {},
                      })
                      .has_value());
    CHECK_FALSE(fixture.service.supportsTranslationRoute(QStringLiteral("fr")));
}

TEST_CASE("translation service returns identity text without invoking a provider",
          "[translation][service]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    QTemporaryDir settingsDirectory;
    QTemporaryDir cacheDirectory;
    REQUIRE(settingsDirectory.isValid());
    REQUIRE(cacheDirectory.isValid());
    useTemporarySettings(settingsDirectory);

    TranslationSettingsStore store;
    REQUIRE_FALSE(store
                      .save({
                          .provider = TranslationProvider::Local,
                          .apiKeyOverride = {},
                          .targetLanguage = QStringLiteral("fr"),
                          .autoTranslateSenders = {},
                          .autoTranslateDomains = {},
                      })
                      .has_value());
    FakeBackend backend;
    ServiceFixture fixture{cacheDirectory.filePath(QStringLiteral("cache.sqlite3")), &backend};
    const TranslationChunks chunks{QStringList{QStringLiteral("Bonjour")}};

    const auto result = QCoro::waitFor(fixture.service.translate(
        chunks, QStringLiteral("fr"), ExternalFetchPolicy::InstalledAndCachedOnly));
    REQUIRE(std::holds_alternative<TranslationChunks>(result));
    CHECK(std::get<TranslationChunks>(result) == chunks);
    CHECK(backend.calls == 0);
}
