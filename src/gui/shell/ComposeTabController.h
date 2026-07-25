#pragma once

#include "gui/shell/ComposeTabPolicy.h"
#include "gui/shell/TabWorkspace.h"
#include "jmap/OperationError.h"
#include "jmap/submission/ComposeTypes.h"

#include <QObject>
#include <QString>

#include <cstddef>
#include <optional>
#include <vector>

class QStackedWidget;
class QWidget;

namespace javelin::app
{
    class ComposeService;
}

namespace javelin::jmap::cache
{
    class IdentityRepository;
}

namespace javelin::jmap::contacts
{
    class ContactIdentityLookup;
}

namespace javelin::gui::compose
{
    class ComposeTabWidget;
}

namespace javelin::gui::shell
{
    struct PersistedComposeTab;

    class ComposeTabController final : public QObject
    {
        Q_OBJECT

      public:
        ComposeTabController(javelin::app::ComposeService& composeService,
                             javelin::jmap::cache::IdentityRepository& identityRepository,
                             javelin::jmap::contacts::ContactIdentityLookup& contactIdentityLookup,
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
        [[nodiscard]] QWidget* contentWidgetForTab(const TabState* tab) const;

      Q_SIGNALS:
        void tabReady(int index);
        void tabBarChanged();
        void closeRequested(int index);
        void statusMessage(QString message, int durationMilliseconds);
        void userInterventionRequired(QString message);
        void operationFailed(javelin::jmap::OperationError error);

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

        javelin::app::ComposeService& m_composeService;
        javelin::jmap::cache::IdentityRepository& m_identityRepository;
        javelin::jmap::contacts::ContactIdentityLookup& m_contactIdentityLookup;
        QStackedWidget& m_contentStack;
        std::vector<TabState>& m_tabs;
    };
} // namespace javelin::gui::shell
