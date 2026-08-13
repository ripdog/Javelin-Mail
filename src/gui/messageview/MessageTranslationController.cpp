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
                    if (!m_inProgress)
                        return;
                    const auto direction = QStringLiteral("%1 → %2").arg(
                        languageName(sourceLanguage), languageName(targetLanguage));
                    if (total > 0)
                    {
                        const auto percentage = static_cast<int>(std::clamp(
                            100.0 * static_cast<double>(received) / static_cast<double>(total), 0.0,
                            100.0));
                        m_progressText =
                            i18n("Downloading %1 translation model… %2%", direction, percentage);
                    }
                    else
                    {
                        m_progressText = i18n("Downloading %1 translation model…", direction);
                    }
                    Q_EMIT stateChanged();
                });
    }

    void MessageTranslationController::reset()
    {
        ++m_requestToken;
        m_inProgress = false;
        m_messageTranslated = false;
        m_originalPlainText.clear();
        m_error.clear();
        m_progressText.clear();
        m_autoTranslateAttempted = false;
        m_translationWasAutomatic = false;
        m_languageDetectionStarted = false;
        m_languageDetection.reset();
        m_shouldOfferTranslation = false;
        Q_EMIT stateChanged();
    }

    void MessageTranslationController::settingsChanged()
    {
        ++m_requestToken;
        m_inProgress = false;
        m_error.clear();
        m_progressText.clear();
        m_autoTranslateAttempted = false;
        m_translationWasAutomatic = false;
        if (m_messageTranslated)
            restoreCurrentTranslation();
        else
            Q_EMIT stateChanged();

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
        auto* watcher =
            new QFutureWatcher<std::optional<javelin::gui::translation::LanguageDetectionResult>>{
                this};
        connect(watcher,
                &QFutureWatcher<
                    std::optional<javelin::gui::translation::LanguageDetectionResult>>::finished,
                this,
                [this, watcher, selectedEmailId]
                {
                    const auto detection = watcher->result();
                    watcher->deleteLater();
                    if (m_emailId != selectedEmailId || !m_snapshot.has_value())
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
        if (!m_service.isEnabled() || m_autoTranslateAttempted || m_messageTranslated ||
            m_inProgress || !m_snapshot.has_value() || !m_shouldOfferTranslation)
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
        if (m_messageTranslated)
        {
            restoreCurrentTranslation();
            return;
        }
        translateCurrentMessage(false,
                                javelin::gui::translation::ExternalFetchPolicy::AllowExternalFetch);
    }

    bool MessageTranslationController::applyTranslatedChunks(
        const javelin::gui::translation::TranslationChunks& chunks)
    {
        if (m_bodyPresenter.activeView() == MessageBodyPresenter::View::PlainText)
        {
            if (chunks.empty() || chunks.front().empty())
                return false;
            m_originalPlainText = m_plainText.toPlainText();
            m_plainText.setHtml(linkifyPlainText(chunks.front().front()));
            return true;
        }
        if (m_bodyPresenter.activeView() == MessageBodyPresenter::View::Html)
        {
            m_html.applyTranslationChunks(chunks);
            return true;
        }
        return false;
    }

    void MessageTranslationController::translateCurrentMessage(
        const bool automatic, const javelin::gui::translation::ExternalFetchPolicy fetchPolicy)
    {
        if (!m_service.isEnabled() || !m_snapshot.has_value() || m_inProgress)
            return;

        const auto selectedEmailId = m_emailId;
        const auto requestToken = ++m_requestToken;
        m_inProgress = true;
        m_error.clear();
        m_progressText.clear();
        m_translationWasAutomatic = automatic;
        Q_EMIT stateChanged();

        const auto translateChunks = [this, selectedEmailId, requestToken, fetchPolicy](
                                         javelin::gui::translation::TranslationChunks chunks)
        {
            const auto sourceLanguage =
                m_languageDetection.has_value()
                    ? QString::fromStdString(m_languageDetection->languageCode)
                    : QStringLiteral("auto");
            auto task = m_service.translate(std::move(chunks), sourceLanguage, fetchPolicy);
            QCoro::connect(
                std::move(task), this,
                [this, selectedEmailId,
                 requestToken](javelin::gui::translation::TranslationResult result)
                {
                    if (m_emailId != selectedEmailId || requestToken != m_requestToken)
                        return;
                    m_inProgress = false;
                    if (std::holds_alternative<javelin::gui::translation::TranslationUnavailable>(
                            result))
                    {
                        m_translationWasAutomatic = false;
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
                    if (!applyTranslatedChunks(translatedChunks))
                    {
                        m_error = i18n("Translation failed: no translated text was returned.");
                        Q_EMIT stateChanged();
                        return;
                    }
                    m_messageTranslated = true;
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
                        m_inProgress = false;
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
        if (m_bodyPresenter.activeView() == MessageBodyPresenter::View::PlainText &&
            !m_originalPlainText.isEmpty())
        {
            m_plainText.setHtml(linkifyPlainText(m_originalPlainText));
        }
        else if (m_bodyPresenter.activeView() == MessageBodyPresenter::View::Html)
        {
            m_html.restoreOriginalText();
        }

        m_inProgress = false;
        m_messageTranslated = false;
        m_originalPlainText.clear();
        m_error.clear();
        m_progressText.clear();
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
        return m_inProgress;
    }

    bool MessageTranslationController::messageTranslated() const
    {
        return m_messageTranslated;
    }

    bool MessageTranslationController::translationWasAutomatic() const
    {
        return m_translationWasAutomatic;
    }

    const QString& MessageTranslationController::error() const
    {
        return m_error;
    }

    const QString& MessageTranslationController::progressText() const
    {
        return m_progressText;
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
