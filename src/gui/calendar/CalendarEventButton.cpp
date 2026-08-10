#include "gui/calendar/CalendarEventButton.h"

#include <QAccessible>
#include <QAccessibleWidget>
#include <QPalette>
#include <QResizeEvent>

#include <algorithm>
#include <cmath>

namespace javelin::gui::calendar
{
    namespace
    {
        [[nodiscard]] double relativeLuminance(const QColor& color)
        {
            const auto channel = [](const double value)
            {
                const auto normalized = value / 255.0;
                return normalized <= 0.04045 ? normalized / 12.92
                                             : std::pow((normalized + 0.055) / 1.055, 2.4);
            };
            return 0.2126 * channel(color.red()) + 0.7152 * channel(color.green()) +
                   0.0722 * channel(color.blue());
        }

        [[nodiscard]] double contrastRatio(const QColor& left, const QColor& right)
        {
            const auto lighter = std::max(relativeLuminance(left), relativeLuminance(right));
            const auto darker = std::min(relativeLuminance(left), relativeLuminance(right));
            return (lighter + 0.05) / (darker + 0.05);
        }

        class AccessibleCalendarEventButton final : public QAccessibleWidget
        {
          public:
            explicit AccessibleCalendarEventButton(CalendarEventButton* button)
                : QAccessibleWidget(button, QAccessible::Button)
            {
            }

            [[nodiscard]] QAccessible::State state() const override
            {
                auto result = QAccessibleWidget::state();
                const auto* button = eventButton();
                if (button == nullptr)
                    return result;
                result.focusable = button->focusPolicy() != Qt::NoFocus;
                result.focused = button->hasFocus();
                result.checkable = button->isCheckable();
                result.checked = button->isChecked();
                result.pressed = button->isDown();
                return result;
            }

            [[nodiscard]] QList<std::pair<QAccessibleInterface*, QAccessible::Relation>>
            relations(const QAccessible::Relation match = QAccessible::AllRelations) const override
            {
                auto result = QAccessibleWidget::relations(match);
                const auto* button = eventButton();
                if (button == nullptr || button->controlledWidget() == nullptr ||
                    !match.testFlag(QAccessible::Controller))
                    return result;
                if (auto* target =
                        QAccessible::queryAccessibleInterface(button->controlledWidget());
                    target != nullptr)
                    result.push_back({target, QAccessible::Controller});
                return result;
            }

            [[nodiscard]] QStringList actionNames() const override
            {
                return {QAccessibleActionInterface::pressAction()};
            }

            void doAction(const QString& actionName) override
            {
                if (actionName == QAccessibleActionInterface::pressAction())
                {
                    if (auto* button = eventButton(); button != nullptr && button->isEnabled())
                        button->click();
                }
            }

            [[nodiscard]] QStringList keyBindingsForAction(const QString& actionName) const override
            {
                Q_UNUSED(actionName);
                return {};
            }

          private:
            [[nodiscard]] CalendarEventButton* eventButton() const
            {
                return dynamic_cast<CalendarEventButton*>(object());
            }
        };

        [[nodiscard]] QAccessibleInterface* calendarEventButtonFactory(const QString& key,
                                                                       QObject* object)
        {
            Q_UNUSED(key);
            if (auto* button = dynamic_cast<CalendarEventButton*>(object); button != nullptr)
                return new AccessibleCalendarEventButton(button);
            return nullptr;
        }

        void ensureAccessibilityFactoryInstalled()
        {
            static const bool installed = []
            {
                QAccessible::installFactory(calendarEventButtonFactory);
                return true;
            }();
            Q_UNUSED(installed);
        }
    } // namespace

    CalendarEventButton::CalendarEventButton(QWidget* parent) : QToolButton(parent)
    {
        ensureAccessibilityFactoryInstalled();
        setMinimumWidth(0);
    }

    void CalendarEventButton::setEventPresentation(QString visualText, QString accessibleText,
                                                   QColor color,
                                                   const CalendarEventButtonAppearance appearance,
                                                   const bool segmentBegins, const bool segmentEnds)
    {
        m_fullText = std::move(visualText);
        m_color = std::move(color);
        m_appearance = appearance;
        m_segmentBegins = segmentBegins;
        m_segmentEnds = segmentEnds;
        setAccessibleName(std::move(accessibleText));
        applyPresentation();
    }

    void CalendarEventButton::setControlledWidget(QWidget* widget)
    {
        m_controlledWidget = widget;
    }

    QWidget* CalendarEventButton::controlledWidget() const
    {
        return m_controlledWidget;
    }

    void CalendarEventButton::resizeEvent(QResizeEvent* event)
    {
        QToolButton::resizeEvent(event);
        if (m_appearance == CalendarEventButtonAppearance::MonthSegment)
        {
            setText(fontMetrics().elidedText(m_fullText, Qt::ElideRight, std::max(0, width() - 8)));
        }
    }

    void CalendarEventButton::applyPresentation()
    {
        const auto color = m_color.isValid() ? m_color : palette().color(QPalette::Highlight);
        const auto text = palette().color(QPalette::Text);
        const auto base = palette().color(QPalette::Base);
        const auto foreground =
            contrastRatio(color, text) >= contrastRatio(color, base) ? text : base;

        if (m_appearance == CalendarEventButtonAppearance::MonthSegment)
        {
            setCheckable(false);
            setAutoRaise(true);
            setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            const auto leftRadius = m_segmentBegins ? QStringLiteral("3px") : QStringLiteral("0px");
            const auto rightRadius = m_segmentEnds ? QStringLiteral("3px") : QStringLiteral("0px");
            setStyleSheet(
                QStringLiteral("QToolButton { background: %1; color: %2; "
                               "border-top-left-radius: %3; border-bottom-left-radius: %3; "
                               "border-top-right-radius: %4; border-bottom-right-radius: %4; "
                               "padding: 1px 4px; text-align: left; }")
                    .arg(color.name(QColor::HexArgb), foreground.name(QColor::HexArgb), leftRadius,
                         rightRadius));
            setText(fontMetrics().elidedText(m_fullText, Qt::ElideRight, std::max(0, width() - 8)));
            return;
        }

        setCheckable(true);
        setAutoRaise(false);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        setStyleSheet(
            QStringLiteral(
                "QToolButton { background: %1; color: %2; border: 1px solid palette(mid); "
                "border-radius: 3px; padding: 3px 5px; text-align: left; } "
                "QToolButton:checked { border: 2px solid palette(highlight); }")
                .arg(color.name(QColor::HexArgb), foreground.name(QColor::HexArgb)));
        setText(m_fullText);
    }
} // namespace javelin::gui::calendar
