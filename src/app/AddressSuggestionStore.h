#pragma once

#include <QObject>
#include <QStringListModel>

namespace javelin::jmap::cache
{
    class DatabaseConnection;
}

namespace javelin::app
{
    class AddressSuggestionStore final : public QObject
    {
        Q_OBJECT

      public:
        static AddressSuggestionStore& instance();
        void initialize(javelin::jmap::cache::DatabaseConnection& connection);
        [[nodiscard]] QStringListModel& model();

      public Q_SLOTS:
        void refresh();

      private:
        AddressSuggestionStore() = default;

        javelin::jmap::cache::DatabaseConnection* m_connection = nullptr;
        QStringListModel m_model;
    };
} // namespace javelin::app
