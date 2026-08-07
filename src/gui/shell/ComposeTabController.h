#pragma once

#include "app/ComposeApplicationPorts.h"
#include "gui/shell/ComposeTabPolicy.h"
#include "gui/shell/TabWorkspace.h"
#include "jmap/OperationError.h"
#include "jmap/submission/ComposeTypes.h"

#include <QObject>
#include <QString>

#include <cstddef>
#include <optional>
#include <vector>

class QMenu;
class QStackedWidget;
class QWidget;

namespace javelin::app
{
    class MailApplicationEventsPort;
}

namespace javelin::jmap::cache
{
    class AccountReader;
    class IdentityReader;
} // namespace javelin::jmap::cache

namespace javelin::jmap::contacts
{
    class ContactIdentityLookup;
}

namespace javelin::gui::compose
{
    class ComposeTabWidget;
}
namespace javelin::gui::settings
{
    class GuiSettings;
}

namespace javelin::gui::shell
{
    struct PersistedComposeTab;

    struct ComposeToolbarState
    {
        bool richText = true;
        bool canSend = false;
        bool canUseSignature = false;
        bool canToggleRichText = false;
    };

    class ComposeTabController final : public QObject
    {
        Q_OBJECT

      public:
        ComposeTabController(javelin::gui::settings::GuiSettings& settings,
                             javelin::app::ComposeCommandPort& composeCommandPort,
                             javelin::jmap::cache::AccountReader& accountReader,
                             javelin::jmap::cache::IdentityReader& identityRepository,
                             javelin::jmap::contacts::ContactIdentityLookup& contactIdentityLookup,
                             javelin::app::MailApplicationEventsPort& mailEvents,
                             QStackedWidget& contentStack, std::vector<TabState>& tabs,
                             QObject* parent = nullptr);

        void open(javelin::jmap::submission::OpenComposeRequest request);
        [[nodiscard]] bool restore(const PersistedComposeTab& persisted);

        [[nodiscard]] std::optional<ComposeTabCloseInput> closeInput(int index) const;
        [[nodiscard]] bool closeImmediately(int index);
        [[nodiscard]] bool discardAndClose(int index);
        void saveDraftAndClose(int index);

        void sendMessage(const TabState* tab);
        void saveDraft(const TabState* tab);
        void attachFiles(const TabState* tab);
        void setRichTextEnabled(const TabState* tab, bool enabled);
        void editCurrentSignature(const TabState* tab);
        [[nodiscard]] ComposeToolbarState toolbarState(const TabState* tab) const;
        [[nodiscard]] QMenu* signatureMenuForTab(const TabState* tab) const;
        [[nodiscard]] QWidget* contentWidgetForTab(const TabState* tab) const;

      Q_SIGNALS:
        void tabReady(int index);
        void tabBarChanged();
        void closeRequested(int index);
        void statusMessage(QString message, int durationMilliseconds);
        void userInterventionRequired(QString message);
        void operationFailed(javelin::jmap::OperationError error);
        void toolbarStateChanged();
        void manageIdentitiesRequested(QString accountId, QString identityId);

      private:
        [[nodiscard]] std::size_t materialize(javelin::jmap::submission::DraftSnapshot snapshot);
        void attachWidget(javelin::gui::compose::ComposeTabWidget* widget, std::size_t index);
        [[nodiscard]] ComposeTabState* composeTabAt(int index);
        [[nodiscard]] const ComposeTabState* composeTabAt(int index) const;
        [[nodiscard]] std::optional<std::size_t>
        indexForWidget(const javelin::gui::compose::ComposeTabWidget* widget) const;
        [[nodiscard]] javelin::gui::compose::ComposeTabWidget*
        composeWidgetForTab(const TabState* tab) const;
        [[nodiscard]] bool detachWidget(int index);

        javelin::gui::settings::GuiSettings& m_settings;
        javelin::app::ComposeCommandPort& m_composeCommandPort;
        javelin::jmap::cache::AccountReader& m_accountReader;
        javelin::jmap::cache::IdentityReader& m_identityRepository;
        javelin::jmap::contacts::ContactIdentityLookup& m_contactIdentityLookup;
        QStackedWidget& m_contentStack;
        std::vector<TabState>& m_tabs;
    };
} // namespace javelin::gui::shell
