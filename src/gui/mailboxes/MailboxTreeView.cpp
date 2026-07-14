#include "gui/mailboxes/MailboxTreeView.h"

#include "gui/mailboxes/MailboxTreeDelegate.h"

#include <QSize>

namespace javelin::gui::mailboxes
{
    MailboxTreeView::MailboxTreeView(QWidget* parent) : QTreeView(parent)
    {
        setItemDelegate(new MailboxTreeDelegate(this));
        setIconSize(QSize{20, 20});
        setHeaderHidden(true);
        setExpandsOnDoubleClick(false);
    }
} // namespace javelin::gui::mailboxes
