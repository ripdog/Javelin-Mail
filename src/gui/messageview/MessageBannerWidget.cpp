#include "gui/messageview/MessageBannerWidget.h"

#include <KLocalizedString>

#include <QEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QSizePolicy>
#include <QToolButton>
#include <QVBoxLayout>

namespace javelin::gui::messageview
{
    namespace
    {
        constexpr auto HoverTextProperty = "messageBannerHoverText";
    }

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

        auto* outerLayout = new QVBoxLayout(this);
        outerLayout->setContentsMargins(8, 6, 8, 6);
        outerLayout->setSpacing(4);

        auto* primaryRow = new QWidget(this);
        primaryRow->setObjectName(QStringLiteral("messageBannerPrimaryRow"));
        m_layout = new QHBoxLayout(primaryRow);
        m_layout->setContentsMargins(0, 0, 0, 0);
        m_layout->setSpacing(8);

        m_iconLabel = new QLabel(primaryRow);
        m_iconLabel->setObjectName(QStringLiteral("messageBannerIcon"));
        m_iconLabel->setFixedSize(20, 20);
        m_iconLabel->setAlignment(Qt::AlignCenter);

        m_textLabel = new QLabel(primaryRow);
        m_textLabel->setObjectName(QStringLiteral("messageBannerText"));
        m_textLabel->setWordWrap(false);
        m_textLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        m_textLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

        m_closeButton = new QToolButton(primaryRow);
        m_closeButton->setObjectName(QStringLiteral("messageBannerClose"));
        m_closeButton->setAutoRaise(true);
        m_closeButton->setIcon(QIcon::fromTheme(QStringLiteral("window-close")));
        m_closeButton->setToolTip(i18nc("@info:tooltip", "Close"));
        connect(m_closeButton, &QToolButton::clicked, this, &MessageBannerWidget::dismissed);

        m_layout->addWidget(m_iconLabel, 0, Qt::AlignVCenter);
        m_layout->addWidget(m_textLabel, 1, Qt::AlignVCenter);
        m_layout->addWidget(m_closeButton, 0, Qt::AlignVCenter);

        m_previewLabel = new QLabel(this);
        m_previewLabel->setObjectName(QStringLiteral("messageBannerPreview"));
        m_previewLabel->setContentsMargins(28, 0, 0, 0);
        m_previewLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_previewLabel->setWordWrap(true);
        m_previewLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        m_previewLabel->hide();

        outerLayout->addWidget(primaryRow);
        outerLayout->addWidget(m_previewLabel);
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

    void MessageBannerWidget::setButtonHoverText(QToolButton* button, const QString& text)
    {
        if (button == nullptr)
        {
            return;
        }

        button->setProperty(HoverTextProperty, text);
        button->installEventFilter(this);
        if (m_previewSource != button)
        {
            return;
        }

        m_previewLabel->setText(text);
        m_previewLabel->setVisible(!text.isEmpty());
        if (text.isEmpty())
        {
            m_previewSource = nullptr;
        }
    }

    bool MessageBannerWidget::eventFilter(QObject* watched, QEvent* event)
    {
        if (event->type() == QEvent::Enter)
        {
            const auto text = watched->property(HoverTextProperty).toString();
            if (!text.isEmpty())
            {
                m_previewSource = watched;
                m_previewLabel->setText(text);
                m_previewLabel->show();
            }
        }
        else if (event->type() == QEvent::Leave && m_previewSource == watched)
        {
            m_previewSource = nullptr;
            m_previewLabel->clear();
            m_previewLabel->hide();
        }

        return QWidget::eventFilter(watched, event);
    }
} // namespace javelin::gui::messageview
