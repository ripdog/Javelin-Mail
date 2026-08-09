#include "gui/shell/ComposeTabController.h"

#include "app/MailApplicationEventsPorts.h"
#include "gui/compose/ComposeTabWidget.h"
#include "gui/compose/ComposeUiPreferences.h"
#include "gui/settings/ConnectionSettingsAdapter.h"
#include "gui/settings/GuiSettings.h"
#include "gui/shell/MainWindowStateStore.h"
#include "jmap/cache/AccountReadRepository.h"
#include "jmap/cache/IdentityReader.h"
#include "jmap/contacts/ContactIdentityLookup.h"

#include <QCoroTask>

#include <KLocalizedString>

#include <QDebug>
#include <QStackedWidget>

#include <string_view>
#include <utility>
#include <variant>

namespace javelin::gui::shell
{
    ComposeTabController::ComposeTabController(
        javelin::gui::settings::GuiSettings& settings,
        javelin::app::ComposeCommandPort& composeCommandPort,
        javelin::jmap::cache::AccountReader& accountReader,
        javelin::jmap::cache::IdentityReader& identityRepository,
        javelin::jmap::contacts::ContactIdentityLookup& contactIdentityLookup,
        javelin::app::MailApplicationEventsPort& mailEvents, QStackedWidget& contentStack,
        std::vector<TabState>& tabs, QObject* parent)
        : QObject(parent), m_settings(settings), m_composeCommandPort(composeCommandPort),
          m_accountReader(accountReader), m_identityRepository(identityRepository),
          m_contactIdentityLookup(contactIdentityLookup), m_contentStack(contentStack), m_tabs(tabs)
    {
        connect(&mailEvents, &javelin::app::MailApplicationEventsPort::cacheInvalidated, this,
                [this](const javelin::app::MailCacheInvalidation& invalidation)
                {
                    if (!invalidation.change.identitiesChanged)
                        return;
                    for (auto& tab : m_tabs)
                    {
                        auto* composeTab = std::get_if<ComposeTabState>(&tab.content);
                        if (composeTab != nullptr && composeTab->widget != nullptr)
                        {
                            composeTab->widget->reloadSenderIdentities(
                                invalidation.change.accountId);
                        }
                    }
                });
    }

    void ComposeTabController::open(javelin::jmap::submission::OpenComposeRequest request)
    {
        const auto settings =
            m_settings.accountForCachedId(QString::fromStdString(request.accountId));
        if (settings.sessionUrl.isEmpty() || settings.loginEmail.isEmpty() ||
            !settings.hasCredentials)
        {
            Q_EMIT userInterventionRequired(i18n("Sign in to this account in Preferences first."));
            return;
        }

        request.initialEditorMode =
            javelin::gui::compose::ComposeUiPreferences::richTextDefault(m_settings)
                ? javelin::jmap::submission::BodyEditorMode::RichText
                : javelin::jmap::submission::BodyEditorMode::PlainText;
        auto task = m_composeCommandPort.open(
            javelin::gui::settings::toAccountConnectionSettings(settings), std::move(request));
        QCoro::connect(
            std::move(task), this,
            [this](std::variant<javelin::jmap::submission::DraftSnapshot,
                                javelin::jmap::OperationError>
                       result)
            {
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                {
                    qWarning().noquote() << "GUI compose open failed" << error->message;
                    Q_EMIT operationFailed(*error);
                    return;
                }

                const auto index = materialize(
                    std::get<javelin::jmap::submission::DraftSnapshot>(std::move(result)));
                Q_EMIT tabReady(static_cast<int>(index));
            });
    }

    bool ComposeTabController::restore(const PersistedComposeTab& persisted)
    {
        const auto draftResult = m_composeCommandPort.loadWorkingCopy(persisted.composeSessionId);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&draftResult))
        {
            Q_EMIT operationFailed(*error);
            return false;
        }

        const auto& snapshot =
            std::get<std::optional<javelin::jmap::submission::DraftSnapshot>>(draftResult);
        if (!snapshot.has_value())
            return false;

        static_cast<void>(materialize(*snapshot, persisted.hasUnsavedChanges));
        return true;
    }

    std::optional<ComposeTabCloseInput> ComposeTabController::closeInput(const int index) const
    {
        const auto* composeTab = composeTabAt(index);
        if (composeTab == nullptr || composeTab->widget == nullptr)
            return std::nullopt;

        return ComposeTabCloseInput{
            .operationInFlight = composeTab->widget->operationInFlight(),
            .closeWithoutPrompt = composeTab->widget->closeWithoutPrompt(),
            .emptyDraft = composeTab->widget->isEmptyDraft(),
            .savedDraft = composeTab->widget->draftEmailId().has_value(),
            .hasUnsavedChanges = composeTab->widget->hasUnsavedChanges(),
        };
    }

    bool ComposeTabController::closeImmediately(const int index)
    {
        return detachWidget(index);
    }

    bool ComposeTabController::discardAndClose(const int index)
    {
        auto* composeTab = composeTabAt(index);
        if (composeTab == nullptr || composeTab->widget == nullptr)
            return false;

        if (const auto error = m_composeCommandPort.discard(composeTab->widget->composeSessionId()))
        {
            Q_EMIT operationFailed(*error);
            return false;
        }
        return detachWidget(index);
    }

    void ComposeTabController::saveDraftAndClose(const int index)
    {
        auto* composeTab = composeTabAt(index);
        if (composeTab != nullptr && composeTab->widget != nullptr)
            composeTab->widget->saveDraftAndClose();
    }

    void ComposeTabController::sendMessage(const TabState* tab)
    {
        if (auto* widget = composeWidgetForTab(tab); widget != nullptr)
            widget->sendMessage();
    }

    void ComposeTabController::scheduleMessage(const TabState* tab)
    {
        if (auto* widget = composeWidgetForTab(tab); widget != nullptr)
            widget->scheduleMessage();
    }

    void ComposeTabController::saveDraft(const TabState* tab)
    {
        if (auto* widget = composeWidgetForTab(tab); widget != nullptr)
            widget->saveDraft();
    }

    void ComposeTabController::attachFiles(const TabState* tab)
    {
        if (auto* widget = composeWidgetForTab(tab); widget != nullptr)
            widget->attachFiles();
    }

    void ComposeTabController::setRichTextEnabled(const TabState* tab, const bool enabled)
    {
        if (auto* widget = composeWidgetForTab(tab); widget != nullptr)
            widget->setRichTextEnabled(enabled);
        Q_EMIT toolbarStateChanged();
    }

    void ComposeTabController::editCurrentSignature(const TabState* tab)
    {
        if (auto* widget = composeWidgetForTab(tab); widget != nullptr)
            widget->editCurrentSignature();
    }

    ComposeToolbarState ComposeTabController::toolbarState(const TabState* tab) const
    {
        const auto* widget = composeWidgetForTab(tab);
        if (widget == nullptr)
            return {};
        return {
            .richText = widget->richTextEnabled(),
            .canSend = widget->canSend(),
            .canScheduleSend = widget->canScheduleSend(),
            .canUseSignature = widget->canSend(),
            .canToggleRichText = !widget->operationInFlight(),
        };
    }

    QMenu* ComposeTabController::signatureMenuForTab(const TabState* tab) const
    {
        const auto* widget = composeWidgetForTab(tab);
        return widget != nullptr ? widget->signatureMenu() : nullptr;
    }

    QWidget* ComposeTabController::contentWidgetForTab(const TabState* tab) const
    {
        return composeWidgetForTab(tab);
    }

    std::size_t ComposeTabController::materialize(javelin::jmap::submission::DraftSnapshot snapshot,
                                                  const bool hasUnsavedChanges)
    {
        std::vector<ComposeTabDescriptor> descriptors;
        descriptors.reserve(m_tabs.size());
        for (std::size_t index = 0; index < m_tabs.size(); ++index)
        {
            const auto* composeTab = std::get_if<ComposeTabState>(&m_tabs[index].content);
            if (composeTab != nullptr)
            {
                descriptors.push_back({
                    .index = index,
                    .composeSessionId = composeTab->composeSessionId,
                });
            }
        }

        const auto subject = snapshot.subject.has_value()
                                 ? std::optional<std::string_view>{*snapshot.subject}
                                 : std::optional<std::string_view>{std::nullopt};
        const auto plan =
            planComposeTabOpen(descriptors, {
                                                .composeSessionId = snapshot.composeSessionId,
                                                .subject = subject,
                                            });
        if (plan.existingIndex.has_value())
        {
            auto* composeTab = std::get_if<ComposeTabState>(&m_tabs[*plan.existingIndex].content);
            if (composeTab != nullptr && plan.updateExistingTitle)
            {
                composeTab->title = QString::fromStdString(plan.title);
                Q_EMIT tabBarChanged();
            }
            return *plan.existingIndex;
        }

        m_tabs.push_back(TabState{
            .content =
                ComposeTabState{
                    .accountId = snapshot.accountId,
                    .composeSessionId = snapshot.composeSessionId,
                    .title = QString::fromStdString(plan.title),
                    .widget = nullptr,
                    .selection = {},
                },
        });
        const auto index = m_tabs.size() - 1;
        auto* widget = new javelin::gui::compose::ComposeTabWidget(
            m_settings, m_composeCommandPort, m_accountReader, m_identityRepository,
            m_contactIdentityLookup, std::move(snapshot), &m_contentStack, hasUnsavedChanges);
        attachWidget(widget, index);
        return index;
    }

    void ComposeTabController::attachWidget(javelin::gui::compose::ComposeTabWidget* widget,
                                            const std::size_t tabIndex)
    {
        if (widget == nullptr || tabIndex >= m_tabs.size())
            return;

        auto* attachedTab = std::get_if<ComposeTabState>(&m_tabs[tabIndex].content);
        if (attachedTab == nullptr)
            return;

        attachedTab->widget = widget;
        attachedTab->title = widget->tabTitle();
        m_contentStack.addWidget(widget);
        connect(widget, &javelin::gui::compose::ComposeTabWidget::titleChanged, this,
                [this, widget](const QString& title)
                {
                    const auto matchingIndex = indexForWidget(widget);
                    if (!matchingIndex.has_value())
                        return;
                    auto* matchingTab =
                        std::get_if<ComposeTabState>(&m_tabs[*matchingIndex].content);
                    if (matchingTab == nullptr)
                        return;
                    matchingTab->title = title;
                    Q_EMIT tabBarChanged();
                });
        connect(widget, &javelin::gui::compose::ComposeTabWidget::accountChanged, this,
                [this, widget](const QString& accountId)
                {
                    const auto matchingIndex = indexForWidget(widget);
                    if (!matchingIndex.has_value())
                        return;
                    auto* matchingTab =
                        std::get_if<ComposeTabState>(&m_tabs[*matchingIndex].content);
                    if (matchingTab != nullptr)
                        matchingTab->accountId = accountId.toStdString();
                });
        connect(widget, &javelin::gui::compose::ComposeTabWidget::statusMessageRequested, this,
                &ComposeTabController::statusMessage);
        connect(widget, &javelin::gui::compose::ComposeTabWidget::toolbarStateChanged, this,
                &ComposeTabController::toolbarStateChanged);
        connect(widget, &javelin::gui::compose::ComposeTabWidget::manageIdentitiesRequested, this,
                &ComposeTabController::manageIdentitiesRequested);
        connect(widget, &javelin::gui::compose::ComposeTabWidget::userInterventionRequired, this,
                &ComposeTabController::userInterventionRequired);
        connect(widget, &javelin::gui::compose::ComposeTabWidget::closeRequested, this,
                [this, widget]
                {
                    const auto matchingIndex = indexForWidget(widget);
                    if (matchingIndex.has_value())
                        Q_EMIT closeRequested(static_cast<int>(*matchingIndex));
                });
    }

    javelin::gui::compose::ComposeTabWidget*
    ComposeTabController::composeWidgetForTab(const TabState* tab) const
    {
        if (tab == nullptr)
            return nullptr;
        const auto* composeTab = std::get_if<ComposeTabState>(&tab->content);
        return composeTab != nullptr ? composeTab->widget : nullptr;
    }

    ComposeTabState* ComposeTabController::composeTabAt(const int index)
    {
        if (index < 0 || static_cast<std::size_t>(index) >= m_tabs.size())
            return nullptr;
        return std::get_if<ComposeTabState>(&m_tabs[static_cast<std::size_t>(index)].content);
    }

    const ComposeTabState* ComposeTabController::composeTabAt(const int index) const
    {
        if (index < 0 || static_cast<std::size_t>(index) >= m_tabs.size())
            return nullptr;
        return std::get_if<ComposeTabState>(&m_tabs[static_cast<std::size_t>(index)].content);
    }

    std::optional<std::size_t> ComposeTabController::indexForWidget(
        const javelin::gui::compose::ComposeTabWidget* widget) const
    {
        for (std::size_t index = 0; index < m_tabs.size(); ++index)
        {
            const auto* composeTab = std::get_if<ComposeTabState>(&m_tabs[index].content);
            if (composeTab != nullptr && composeTab->widget == widget)
                return index;
        }
        return std::nullopt;
    }

    bool ComposeTabController::detachWidget(const int index)
    {
        auto* composeTab = composeTabAt(index);
        if (composeTab == nullptr || composeTab->widget == nullptr)
            return false;

        auto* widget = composeTab->widget;
        m_contentStack.removeWidget(widget);
        widget->deleteLater();
        composeTab->widget = nullptr;
        return true;
    }
} // namespace javelin::gui::shell
