#include "gui/messageview/MessageTranslationController.h"

#include "gui/messageview/HtmlMessageView.h"
#include "gui/messageview/MessageBodyPresenter.h"
#include "gui/messageview/PlainTextLinkifier.h"
#include "gui/translation/TranslationService.h"
#include "jmap/render/HtmlTextExtractor.h"

#include <QCoroTask>

#include <KLocalizedString>

#include <QFutureWatcher>
#include <QLocale>
#include <QTextBrowser>

#include <algorithm>
#include <utility>
#include <variant>

namespace javelin::gui::messageview
{
    namespace
    {
        [[nodiscard]] QString
        detectionText(const javelin::jmap::cache::MessageViewSnapshot& snapshot)
        {
            const bool hasCompletePlainText =
                snapshot.plainTextBody.has_value() && !snapshot.plainTextBody->isTruncated;
            const bool hasCompleteHtml =
                snapshot.htmlBody.has_value() && !snapshot.htmlBody->isTruncated;
            if (hasCompletePlainText)
                return QString::fromStdString(snapshot.plainTextBody->value);
            if (hasCompleteHtml)
            {
                return javelin::jmap::render::plainTextFromHtml(
                    QString::fromStdString(snapshot.htmlBody->value));
            }
            return {};
        }

        [[nodiscard]] QString languageName(const QString& languageCode)
        {
            const auto locale = QLocale{languageCode};
            const auto name = QLocale::languageToString(locale.language());
            return name == QStringLiteral("C") ? languageCode : name;
        }
    } // namespace

    MessageTranslationController::MessageTranslationController(
        javelin::gui::translation::TranslationService& service, MessageBodyPresenter& bodyPresenter,
        QTextBrowser& plainText, HtmlMessageView& html,
        const std::optional<javelin::jmap::cache::MessageViewSnapshot>& snapshot,
        const std::optional<std::string>& emailId, std::function<QString()> senderAddress,
        std::function<QString()> senderDomain, QObject* parent)
        : QObject(parent), m_service(service), m_bodyPresenter(bodyPresenter),
          m_plainText(plainText), m_html(html), m_snapshot(snapshot), m_emailId(emailId),
          m_senderAddress(std::move(senderAddress)), m_senderDomain(std::move(senderDomain))
    {
        connect(&m_service,
                &javelin::gui::translation::TranslationService::localModelDownloadProgress, this,
                [this](const QString& sourceLanguage, const QString& targetLanguage,
                       const qint64 received, const qint64 total)
                {
                    auto* translating = std::get_if<TranslationInProgress>(&m_translation);
                    if (translating == nullptr)
                        return;
                    const auto direction = QStringLiteral("%1 → %2").arg(
                        languageName(sourceLanguage), languageName(targetLanguage));
                    if (total > 0)
                    {
                        const auto percentage = static_cast<int>(std::clamp(
                            100.0 * static_cast<double>(received) / static_cast<double>(total), 0.0,
                            100.0));
                        translating->progressText =
                            i18n("Downloading %1 translation model… %2%", direction, percentage);
                    }
                    else
                    {
                        translating->progressText =
                            i18n("Downloading %1 translation model…", direction);
                    }
                    Q_EMIT stateChanged();
                });
    }

    void MessageTranslationController::reset()
    {
        ++m_requestToken;
        m_translation = TranslationIdle{};
        m_error.clear();
        m_autoTranslateAttempted = false;
        m_languageDetectionStarted = false;
        ++m_languageDetectionToken;
        m_languageDetection.reset();
        m_shouldOfferTranslation = false;
        Q_EMIT stateChanged();
    }

    void MessageTranslationController::settingsChanged()
    {
        ++m_requestToken;
        m_error.clear();
        m_autoTranslateAttempted = false;
        if (std::holds_alternative<MessageTranslated>(m_translation))
        {
            restoreCurrentTranslation();
        }
        else
        {
            m_translation = TranslationIdle{};
            Q_EMIT stateChanged();
        }

        if (!m_service.isEnabled() || !m_snapshot.has_value())
            return;
        if (m_languageDetection.has_value())
        {
            updateOfferState();
            Q_EMIT stateChanged();
            maybeAutoTranslate();
            return;
        }
        m_languageDetectionStarted = false;
        startLanguageDetection();
    }

    void MessageTranslationController::updateOfferState()
    {
        m_shouldOfferTranslation =
            m_languageDetection.has_value() &&
            javelin::gui::translation::shouldOfferTranslation(
                *m_languageDetection, m_service.targetLanguage().toStdString()) &&
            m_service.supportsTranslationRoute(
                QString::fromStdString(m_languageDetection->languageCode));
    }

    void MessageTranslationController::startLanguageDetection()
    {
        if (!m_service.isEnabled() || m_languageDetectionStarted || !m_snapshot.has_value())
            return;

        const auto text = detectionText(*m_snapshot);
        if (text.isEmpty())
            return;

        m_languageDetectionStarted = true;
        const auto selectedEmailId = m_emailId;
        const auto detectionToken = ++m_languageDetectionToken;
        auto* watcher =
            new QFutureWatcher<std::optional<javelin::gui::translation::LanguageDetectionResult>>{
                this};
        connect(watcher,
                &QFutureWatcher<
                    std::optional<javelin::gui::translation::LanguageDetectionResult>>::finished,
                this,
                [this, watcher, selectedEmailId, detectionToken]
                {
                    const auto detection = watcher->result();
                    watcher->deleteLater();
                    // A reset or a restarted detection supersedes this result, even when the same
                    // email is shown again.
                    if (m_emailId != selectedEmailId ||
                        detectionToken != m_languageDetectionToken || !m_snapshot.has_value())
                        return;
                    m_languageDetection = detection;
                    updateOfferState();
                    Q_EMIT stateChanged();
                    maybeAutoTranslate();
                });
        watcher->setFuture(m_service.detectLanguage(text));
    }

    void MessageTranslationController::maybeAutoTranslate()
    {
        if (!m_service.isEnabled() || m_autoTranslateAttempted ||
            !std::holds_alternative<TranslationIdle>(m_translation) || !m_snapshot.has_value() ||
            !m_shouldOfferTranslation)
            return;
        if (m_bodyPresenter.activeView() == MessageBodyPresenter::View::Html &&
            !m_bodyPresenter.htmlDocumentLoaded())
            return;

        const auto fetchPolicy =
            m_service.shouldAutoTranslate(m_senderAddress(), m_senderDomain())
                ? javelin::gui::translation::ExternalFetchPolicy::AllowExternalFetch
                : javelin::gui::translation::ExternalFetchPolicy::InstalledAndCachedOnly;
        m_autoTranslateAttempted = true;
        translateCurrentMessage(true, fetchPolicy);
    }

    void MessageTranslationController::translateCurrentMessage()
    {
        if (std::holds_alternative<MessageTranslated>(m_translation))
        {
            restoreCurrentTranslation();
            return;
        }
        translateCurrentMessage(false,
                                javelin::gui::translation::ExternalFetchPolicy::AllowExternalFetch);
    }

    std::optional<MessageTranslationController::MessageTranslated>
    MessageTranslationController::applyTranslatedChunks(
        const javelin::gui::translation::TranslationChunks& chunks, const bool automatic)
    {
        QStringList accessibleChunks;
        for (const auto& group : chunks)
        {
            for (const auto& chunk : group)
            {
                if (!chunk.trimmed().isEmpty())
                    accessibleChunks.push_back(chunk);
            }
        }

        if (m_bodyPresenter.activeView() == MessageBodyPresenter::View::PlainText)
        {
            if (chunks.empty() || chunks.front().empty())
                return std::nullopt;
            auto originalPlainText = m_plainText.toPlainText();
            m_plainText.setHtml(linkifyPlainText(chunks.front().front()));
            return MessageTranslated{.automatic = automatic,
                                     .originalPlainText = std::move(originalPlainText),
                                     .translatedText = accessibleChunks.join(QLatin1Char{'\n'})};
        }
        if (m_bodyPresenter.activeView() == MessageBodyPresenter::View::Html)
        {
            m_html.applyTranslationChunks(chunks);
            return MessageTranslated{.automatic = automatic,
                                     .originalPlainText = {},
                                     .translatedText = accessibleChunks.join(QLatin1Char{'\n'})};
        }
        return std::nullopt;
    }

    void MessageTranslationController::translateCurrentMessage(
        const bool automatic, const javelin::gui::translation::ExternalFetchPolicy fetchPolicy)
    {
        if (!m_service.isEnabled() || !m_snapshot.has_value() ||
            !std::holds_alternative<TranslationIdle>(m_translation))
            return;

        const auto selectedEmailId = m_emailId;
        const auto requestToken = ++m_requestToken;
        m_translation = TranslationInProgress{.automatic = automatic, .progressText = {}};
        m_error.clear();
        Q_EMIT stateChanged();

        const auto translateChunks = [this, selectedEmailId, requestToken, fetchPolicy, automatic](
                                         javelin::gui::translation::TranslationChunks chunks)
        {
            const auto sourceLanguage =
                m_languageDetection.has_value()
                    ? QString::fromStdString(m_languageDetection->languageCode)
                    : QStringLiteral("auto");
            auto task = m_service.translate(std::move(chunks), sourceLanguage, fetchPolicy);
            QCoro::connect(
                std::move(task), this,
                [this, selectedEmailId, requestToken,
                 automatic](javelin::gui::translation::TranslationResult result)
                {
                    if (m_emailId != selectedEmailId || requestToken != m_requestToken)
                        return;
                    m_translation = TranslationIdle{};
                    if (std::holds_alternative<javelin::gui::translation::TranslationUnavailable>(
                            result))
                    {
                        Q_EMIT stateChanged();
                        return;
                    }
                    if (const auto* error =
                            std::get_if<javelin::gui::translation::TranslationError>(&result))
                    {
                        m_error = i18n("Translation failed: %1", error->message);
                        Q_EMIT stateChanged();
                        return;
                    }

                    const auto& translatedChunks =
                        std::get<javelin::gui::translation::TranslationChunks>(result);
                    auto translated = applyTranslatedChunks(translatedChunks, automatic);
                    if (!translated.has_value())
                    {
                        m_error = i18n("Translation failed: no translated text was returned.");
                        Q_EMIT stateChanged();
                        return;
                    }
                    m_translation = std::move(*translated);
                    Q_EMIT stateChanged();
                });
        };

        if (m_bodyPresenter.activeView() == MessageBodyPresenter::View::PlainText)
        {
            javelin::gui::translation::TranslationChunks chunks;
            chunks.push_back(QStringList{m_plainText.toPlainText()});
            translateChunks(std::move(chunks));
            return;
        }

        if (m_bodyPresenter.activeView() == MessageBodyPresenter::View::Html)
        {
            m_html.collectTranslationChunks(
                [this, selectedEmailId, requestToken, translateChunks](QVector<QStringList> chunks)
                {
                    if (m_emailId != selectedEmailId || requestToken != m_requestToken)
                        return;
                    if (chunks.empty())
                    {
                        m_translation = TranslationIdle{};
                        m_error = i18n("Translation failed: no message text was found.");
                        Q_EMIT stateChanged();
                        return;
                    }
                    translateChunks(std::move(chunks));
                });
        }
    }

    void MessageTranslationController::restoreCurrentTranslation()
    {
        const auto* translated = std::get_if<MessageTranslated>(&m_translation);
        if (m_bodyPresenter.activeView() == MessageBodyPresenter::View::PlainText &&
            translated != nullptr && !translated->originalPlainText.isEmpty())
        {
            m_plainText.setHtml(linkifyPlainText(translated->originalPlainText));
        }
        else if (m_bodyPresenter.activeView() == MessageBodyPresenter::View::Html)
        {
            m_html.restoreOriginalText();
        }

        m_translation = TranslationIdle{};
        m_error.clear();
        Q_EMIT stateChanged();
    }

    void MessageTranslationController::setAutoTranslateSender(const bool enabled)
    {
        if (const auto error = m_service.setAutoTranslateSender(m_senderAddress(), enabled))
        {
            m_error = i18n("Could not save translation rule: %1", error->message);
            Q_EMIT stateChanged();
            return;
        }
        if (enabled)
        {
            if (const auto error = m_service.setAutoTranslateDomain(m_senderDomain(), false))
            {
                m_error = i18n("Could not save translation rule: %1", error->message);
                Q_EMIT stateChanged();
                return;
            }
            m_autoTranslateAttempted = false;
        }
        Q_EMIT stateChanged();
        maybeAutoTranslate();
    }

    void MessageTranslationController::setAutoTranslateDomain(const bool enabled)
    {
        if (const auto error = m_service.setAutoTranslateDomain(m_senderDomain(), enabled))
        {
            m_error = i18n("Could not save translation rule: %1", error->message);
            Q_EMIT stateChanged();
            return;
        }
        if (enabled)
        {
            if (const auto error = m_service.setAutoTranslateSender(m_senderAddress(), false))
            {
                m_error = i18n("Could not save translation rule: %1", error->message);
                Q_EMIT stateChanged();
                return;
            }
            m_autoTranslateAttempted = false;
        }
        Q_EMIT stateChanged();
        maybeAutoTranslate();
    }

    bool MessageTranslationController::inProgress() const
    {
        return std::holds_alternative<TranslationInProgress>(m_translation);
    }

    bool MessageTranslationController::messageTranslated() const
    {
        return std::holds_alternative<MessageTranslated>(m_translation);
    }

    bool MessageTranslationController::translationWasAutomatic() const
    {
        const auto* translated = std::get_if<MessageTranslated>(&m_translation);
        return translated != nullptr && translated->automatic;
    }

    const QString& MessageTranslationController::error() const
    {
        return m_error;
    }

    const QString& MessageTranslationController::progressText() const
    {
        static const QString none;
        const auto* translating = std::get_if<TranslationInProgress>(&m_translation);
        return translating == nullptr ? none : translating->progressText;
    }

    const QString& MessageTranslationController::translatedText() const
    {
        static const QString none;
        const auto* translated = std::get_if<MessageTranslated>(&m_translation);
        return translated == nullptr ? none : translated->translatedText;
    }

    const std::optional<javelin::gui::translation::LanguageDetectionResult>&
    MessageTranslationController::languageDetection() const
    {
        return m_languageDetection;
    }

    bool MessageTranslationController::shouldOfferTranslation() const
    {
        return m_shouldOfferTranslation;
    }

    bool MessageTranslationController::canTranslateActiveView() const
    {
        const auto view = m_bodyPresenter.activeView();
        return m_service.isEnabled() && (view == MessageBodyPresenter::View::PlainText ||
                                         view == MessageBodyPresenter::View::Html);
    }

    bool MessageTranslationController::senderRuleEnabled() const
    {
        return m_service.settings().autoTranslateSenders.contains(m_senderAddress(),
                                                                  Qt::CaseInsensitive);
    }

    bool MessageTranslationController::domainRuleEnabled() const
    {
        return m_service.settings().autoTranslateDomains.contains(m_senderDomain(),
                                                                  Qt::CaseInsensitive);
    }
} // namespace javelin::gui::messageview
