#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>
#include <memory>
#include <variant>

namespace javelin::gui::messageview
{

    class GoogleHtmlTranslator final : public QObject
    {
      public:
        using TranslationChunks = QVector<QStringList>;
        using Result = std::variant<TranslationChunks, QString>;
        using Callback = std::function<void(Result)>;

        explicit GoogleHtmlTranslator(QObject* parent = nullptr);

        void translate(TranslationChunks sourceChunks, QString sourceLanguage,
                       QString targetLanguage, Callback callback);

        struct PendingRequest
        {
            qsizetype chunkIndex = 0;
            QString requestText;
        };

      private:
        struct RunState
        {
            TranslationChunks results;
            qsizetype remainingBatches = 0;
            bool finished = false;
            Callback callback;
        };

        [[nodiscard]] static QVector<QVector<PendingRequest>>
        makeRequestBatches(const TranslationChunks& sourceChunks);
        void translateBatch(QVector<PendingRequest> requests, QString sourceLanguage,
                            QString targetLanguage, std::shared_ptr<RunState> state);

        [[nodiscard]] static QString transformRequest(const QStringList& sourceArray);
        [[nodiscard]] static QStringList transformResponse(QString result);
    };

} // namespace javelin::gui::messageview
