#include "gui/messageview/MessageBannerWidget.h"

#include <KLocalizedString>

#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QSizePolicy>
#include <QToolButton>

namespace javelin::gui::messageview
{
    MessageBannerWidget::MessageBannerWidget(QWidget* parent) : QWidget(parent)
    {
        setObjectName(QStringLiteral("messageBanner"));
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
        setStyleSheet(QStringLiteral(
            "QWidget#messageBanner { background: palette(alternate-base); border: 1px solid "
            "palette(mid); border-radius: 6px; }"
            "QWidget#messageBanner QLabel { border: 0; background: transparent; }"
            "QWidget#messageBanner QToolButton { border: 1px solid palette(mid); border-radius: "
            "4px; padding: 4px 8px; }"));

        m_layout = new QHBoxLayout(this);
        m_layout->setContentsMargins(8, 6, 8, 6);
        m_layout->setSpacing(8);

        m_iconLabel = new QLabel(this);
        m_iconLabel->setObjectName(QStringLiteral("messageBannerIcon"));
        m_iconLabel->setFixedSize(20, 20);
        m_iconLabel->setAlignment(Qt::AlignCenter);

        m_textLabel = new QLabel(this);
        m_textLabel->setObjectName(QStringLiteral("messageBannerText"));
        m_textLabel->setWordWrap(false);
        m_textLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        m_textLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

        m_closeButton = new QToolButton(this);
        m_closeButton->setObjectName(QStringLiteral("messageBannerClose"));
        m_closeButton->setAutoRaise(true);
        m_closeButton->setIcon(QIcon::fromTheme(QStringLiteral("window-close")));
        m_closeButton->setToolTip(i18nc("@info:tooltip", "Close"));
        connect(m_closeButton, &QToolButton::clicked, this, &MessageBannerWidget::dismissed);

        m_layout->addWidget(m_iconLabel, 0, Qt::AlignVCenter);
        m_layout->addWidget(m_textLabel, 1, Qt::AlignVCenter);
        m_layout->addWidget(m_closeButton, 0, Qt::AlignVCenter);
    }

    void MessageBannerWidget::setIcon(const QIcon& icon)
    {
        m_iconLabel->setPixmap(icon.pixmap(18, 18));
    }

    void MessageBannerWidget::setText(const QString& text)
    {
        m_textLabel->setText(text);
    }

    QToolButton* MessageBannerWidget::addButton(const QString& text)
    {
        auto* button = new QToolButton(this);
        button->setText(text);
        m_layout->insertWidget(m_layout->count() - 1, button, 0, Qt::AlignVCenter);
        return button;
    }
} // namespace javelin::gui::messageview
