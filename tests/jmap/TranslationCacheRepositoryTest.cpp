#include "jmap/cache/TranslationCacheRepository.h"

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
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

    [[nodiscard]] QString makeConnectionName()
    {
        static int counter = 0;
        ++counter;
        return QStringLiteral("javelin-translation-cache-%1").arg(counter);
    }

} // namespace

TEST_CASE("translation cache repository round-trips translated strings",
          "[jmap][cache][translation]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    QTemporaryDir temporaryDir;
    REQUIRE(temporaryDir.isValid());

    auto connectionResult = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = makeConnectionName(),
        .databasePath = temporaryDir.filePath(QStringLiteral("cache.sqlite3")),
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
                          .targetLanguage = QStringLiteral("en"),
                          .inputText = QStringLiteral("こんにちは"),
                          .translatedText = QStringLiteral("Hello"),
                      })
                      .has_value());

    const auto result =
        repository.find(QStringLiteral("auto"), QStringLiteral("en"), QStringLiteral("こんにちは"));
    REQUIRE(std::holds_alternative<std::optional<QString>>(result));
    const auto translated = std::get<std::optional<QString>>(result);
    REQUIRE(translated.has_value());
    CHECK(*translated == QStringLiteral("Hello"));

    const auto miss =
        repository.find(QStringLiteral("auto"), QStringLiteral("en"), QStringLiteral("こんばんは"));
    REQUIRE(std::holds_alternative<std::optional<QString>>(miss));
    CHECK_FALSE(std::get<std::optional<QString>>(miss).has_value());
}
