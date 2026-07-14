#pragma once

#include <QTreeView>

namespace javelin::gui::mailboxes
{
    class MailboxTreeView final : public QTreeView
    {
      public:
        explicit MailboxTreeView(QWidget* parent = nullptr);
    };
} // namespace javelin::gui::mailboxes
