#pragma once

#include <QString>
#include <Qt>

#include <functional>
#include <string>
#include <unordered_set>

class QComboBox;
class QObject;

namespace javelin::app
{
    class ComposeCommandPort;
}
namespace javelin::gui::settings
{
    class GuiSettings;
}
namespace javelin::jmap::cache
{
    class AccountReader;
    class IdentityReader;
} // namespace javelin::jmap::cache

namespace javelin::gui::compose
{
    class ComposeIdentityController final
    {
      public:
        static constexpr int identityIdRole = Qt::UserRole;
        static constexpr int accountIdRole = Qt::UserRole + 1;
        static constexpr int emailRole = Qt::UserRole + 2;
        static constexpr int textSignatureRole = Qt::UserRole + 3;
        static constexpr int htmlSignatureRole = Qt::UserRole + 4;
        static constexpr int bccRole = Qt::UserRole + 5;

        ComposeIdentityController(javelin::gui::settings::GuiSettings& settings,
                                  javelin::app::ComposeCommandPort& composeCommandPort,
                                  javelin::jmap::cache::AccountReader& accountReader,
                                  javelin::jmap::cache::IdentityReader& identityReader,
                                  QComboBox& combo, QObject& context,
                                  std::function<void(QString, int)> statusMessage,
                                  std::function<void()> asynchronousReloadRequested);

        void load(const std::string& selectedAccountId, const std::string& selectedIdentityId);

      private:
        javelin::gui::settings::GuiSettings& m_settings;
        javelin::app::ComposeCommandPort& m_composeCommandPort;
        javelin::jmap::cache::AccountReader& m_accountReader;
        javelin::jmap::cache::IdentityReader& m_identityReader;
        QComboBox& m_combo;
        QObject& m_context;
        std::function<void(QString, int)> m_statusMessage;
        std::function<void()> m_asynchronousReloadRequested;
        std::unordered_set<std::string> m_identityLoadsStarted;
    };
} // namespace javelin::gui::compose
