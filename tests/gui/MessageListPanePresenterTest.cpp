#include "gui/messages/MessageListPanePresenter.h"

#include "gui/shell/ElidingLabel.h"

#include <QLabel>
#include <QListView>
#include <QProgressBar>
#include <QToolButton>
#include <QWidget>

#include <catch2/catch_test_macros.hpp>

namespace
{
    struct Fixture
    {
        QWidget root;
        javelin::gui::shell::ElidingLabel title{&root};
        QLabel meta{&root};
        QListView messageView{&root};
        QWidget emptyPanel{&root};
        QLabel emptyLabel{&emptyPanel};
        QToolButton emptyAction{&emptyPanel};
        QProgressBar loadingIndicator{&root};
        QToolButton searchServerButton{&root};
        QWidget continuationFooter{&root};
        QLabel continuationLabel{&continuationFooter};
        QToolButton continuationRetry{&continuationFooter};
        javelin::gui::messages::MessageListPanePresenter presenter{title,
                                                                   meta,
                                                                   emptyPanel,
                                                                   emptyLabel,
                                                                   emptyAction,
                                                                   messageView,
                                                                   loadingIndicator,
                                                                   searchServerButton,
                                                                   continuationFooter,
                                                                   continuationLabel,
                                                                   continuationRetry};
    };
} // namespace

TEST_CASE("message list empty presentation replaces the list without changing its pane")
{
    Fixture fixture;

    fixture.presenter.showEmptyState({
        .itemCount = 0,
        .kind = javelin::gui::messages::MessageListEmptyStateKind::EmptyMailbox,
        .action = javelin::gui::messages::MessageListEmptyAction::None,
        .detail = {},
    });

    CHECK_FALSE(fixture.emptyPanel.isHidden());
    CHECK(fixture.messageView.isHidden());
    CHECK(fixture.emptyLabel.text() == QStringLiteral("This mailbox is empty."));

    fixture.presenter.showEmptyState({
        .itemCount = 1,
        .kind = javelin::gui::messages::MessageListEmptyStateKind::EmptyMailbox,
        .action = javelin::gui::messages::MessageListEmptyAction::None,
        .detail = {},
    });

    CHECK(fixture.emptyPanel.isHidden());
    CHECK_FALSE(fixture.messageView.isHidden());
}
