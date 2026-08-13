#pragma once

#include "gui/translation/LanguageDetection.h"
#include "gui/translation/TranslationTypes.h"
#include "jmap/cache/MessageViewReader.h"

#include <QObject>
#include <QString>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

class QTextBrowser;

namespace javelin::gui::translation
{
    class TranslationService;
}

namespace javelin::gui::messageview
{
    class HtmlMessageView;
    class MessageBodyPresenter;

    class MessageTranslationController final : public QObject
    {
        Q_OBJECT

      public:
        MessageTranslationController(
            javelin::gui::translation::TranslationService& service,
            MessageBodyPresenter& bodyPresenter, QTextBrowser& plainText, HtmlMessageView& html,
            const std::optional<javelin::jmap::cache::MessageViewSnapshot>& snapshot,
            const std::optional<std::string>& emailId, std::function<QString()> senderAddress,
            std::function<QString()> senderDomain, QObject* parent = nullptr);

        void reset();
        void settingsChanged();
        void startLanguageDetection();
        void maybeAutoTranslate();
        void translateCurrentMessage();
        void restoreCurrentTranslation();
        void setAutoTranslateSender(bool enabled);
        void setAutoTranslateDomain(bool enabled);

        [[nodiscard]] bool inProgress() const;
        [[nodiscard]] bool messageTranslated() const;
        [[nodiscard]] bool translationWasAutomatic() const;
        [[nodiscard]] const QString& error() const;
        [[nodiscard]] const QString& progressText() const;
        [[nodiscard]] const std::optional<javelin::gui::translation::LanguageDetectionResult>&
        languageDetection() const;
        [[nodiscard]] bool shouldOfferTranslation() const;
        [[nodiscard]] bool canTranslateActiveView() const;
        [[nodiscard]] bool senderRuleEnabled() const;
        [[nodiscard]] bool domainRuleEnabled() const;

      Q_SIGNALS:
        void stateChanged();

      private:
        void translateCurrentMessage(bool automatic,
                                     javelin::gui::translation::ExternalFetchPolicy fetchPolicy);
        [[nodiscard]] bool
        applyTranslatedChunks(const javelin::gui::translation::TranslationChunks& chunks);
        void updateOfferState();

        javelin::gui::translation::TranslationService& m_service;
        MessageBodyPresenter& m_bodyPresenter;
        QTextBrowser& m_plainText;
        HtmlMessageView& m_html;
        const std::optional<javelin::jmap::cache::MessageViewSnapshot>& m_snapshot;
        const std::optional<std::string>& m_emailId;
        std::function<QString()> m_senderAddress;
        std::function<QString()> m_senderDomain;

        bool m_inProgress = false;
        std::uint64_t m_requestToken = 0;
        bool m_messageTranslated = false;
        QString m_originalPlainText;
        QString m_error;
        QString m_progressText;
        bool m_autoTranslateAttempted = false;
        bool m_translationWasAutomatic = false;
        bool m_languageDetectionStarted = false;
        std::optional<javelin::gui::translation::LanguageDetectionResult> m_languageDetection;
        bool m_shouldOfferTranslation = false;
    };
} // namespace javelin::gui::messageview
