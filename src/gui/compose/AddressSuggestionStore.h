#pragma once

#include <QFutureWatcher>
#include <QObject>
#include <QStringList>
#include <QStringListModel>
#include <QTimer>

#include <optional>

namespace javelin::app
{
    class AddressSuggestionStore final : public QObject
    {
        Q_OBJECT

      public:
        static AddressSuggestionStore& instance();
        void initialize(QString databasePath);
        [[nodiscard]] QStringListModel& model();

      public Q_SLOTS:
        void refresh();

      private:
        AddressSuggestionStore();
        void startRefresh();

        QString m_databasePath;
        QStringListModel m_model;
        QTimer m_refreshTimer;
        QFutureWatcher<std::optional<QStringList>> m_refreshWatcher;
        bool m_refreshPending = false;
    };
} // namespace javelin::app
