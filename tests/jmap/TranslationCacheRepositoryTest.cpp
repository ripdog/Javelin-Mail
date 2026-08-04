#include "gui/translation/TranslationCache.h"

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QTemporaryDir>

#include <memory>
#include <optional>
#include <variant>

using namespace javelin::gui::translation;

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
            static char appName[] = "javelin-translation-cache-tests";
            static char* argv[] = {appName, nullptr};
            m_application = std::make_unique<QCoreApplication>(argc, argv);
        }

      private:
        std::unique_ptr<QCoreApplication> m_application;
    };
} // namespace

TEST_CASE("GUI translation cache round-trips Unicode and isolates provider revisions",
          "[translation][cache]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    QTemporaryDir temporaryDirectory;
    REQUIRE(temporaryDirectory.isValid());
    TranslationCache cache{temporaryDirectory.filePath(QStringLiteral("cache.sqlite3"))};
    REQUIRE_FALSE(cache.open().has_value());

    REQUIRE_FALSE(cache
                      .upsert({TranslationCacheRecord{
                          .provider = TranslationProvider::Google,
                          .sourceLanguage = QStringLiteral("ja"),
                          .targetLanguage = QStringLiteral("en"),
                          .backendRevision = QStringLiteral("google-v1"),
                          .inputText = QStringLiteral("こんにちは 🌏"),
                          .translatedText = QStringLiteral("Hello 🌏"),
                      }})
                      .has_value());

    const auto hit =
        cache.find(TranslationProvider::Google, QStringLiteral("ja"), QStringLiteral("en"),
                   QStringLiteral("google-v1"), QStringLiteral("こんにちは 🌏"));
    REQUIRE(std::holds_alternative<std::optional<QString>>(hit));
    REQUIRE(std::get<std::optional<QString>>(hit).has_value());
    CHECK(*std::get<std::optional<QString>>(hit) == QStringLiteral("Hello 🌏"));

    const auto providerMiss =
        cache.find(TranslationProvider::Local, QStringLiteral("ja"), QStringLiteral("en"),
                   QStringLiteral("google-v1"), QStringLiteral("こんにちは 🌏"));
    REQUIRE(std::holds_alternative<std::optional<QString>>(providerMiss));
    CHECK_FALSE(std::get<std::optional<QString>>(providerMiss).has_value());

    const auto revisionMiss =
        cache.find(TranslationProvider::Google, QStringLiteral("ja"), QStringLiteral("en"),
                   QStringLiteral("google-v2"), QStringLiteral("こんにちは 🌏"));
    REQUIRE(std::holds_alternative<std::optional<QString>>(revisionMiss));
    CHECK_FALSE(std::get<std::optional<QString>>(revisionMiss).has_value());
}

TEST_CASE("GUI translation cache replaces an existing identity transactionally",
          "[translation][cache]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    QTemporaryDir temporaryDirectory;
    REQUIRE(temporaryDirectory.isValid());
    TranslationCache cache{temporaryDirectory.filePath(QStringLiteral("cache.sqlite3"))};

    const TranslationCacheRecord initial{
        .provider = TranslationProvider::Local,
        .sourceLanguage = QStringLiteral("fr"),
        .targetLanguage = QStringLiteral("en"),
        .backendRevision = QStringLiteral("model-a"),
        .inputText = QStringLiteral("Bonjour"),
        .translatedText = QStringLiteral("Hello"),
    };
    REQUIRE_FALSE(cache.upsert({initial}).has_value());
    auto replacement = initial;
    replacement.translatedText = QStringLiteral("Good morning");
    REQUIRE_FALSE(cache.upsert({replacement}).has_value());

    const auto result =
        cache.find(TranslationProvider::Local, QStringLiteral("fr"), QStringLiteral("en"),
                   QStringLiteral("model-a"), QStringLiteral("Bonjour"));
    REQUIRE(std::holds_alternative<std::optional<QString>>(result));
    CHECK(std::get<std::optional<QString>>(result) ==
          std::optional<QString>{QStringLiteral("Good morning")});
}
