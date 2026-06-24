#include "gui/IconUtils.h"

#include <QFile>
#include <QGuiApplication>
#include <QIconEngine>
#include <QPainter>
#include <QScreen>
#include <QSvgRenderer>

#include <algorithm>
#include <cmath>
#include <utility>

namespace javelin::gui
{
    namespace
    {

        [[nodiscard]] QByteArray themedSvgData(const QString& resourcePath, const QColor& color)
        {
            QFile file{resourcePath};
            if (!file.open(QIODevice::ReadOnly))
            {
                return {};
            }

            auto svg = QString::fromUtf8(file.readAll());
            const auto themeColor = color.name(QColor::HexRgb);
            svg.replace(
                QStringLiteral("fill=\"context-fill\" fill-opacity=\"context-fill-opacity\""),
                QStringLiteral("fill=\"%1\" fill-opacity=\"1\"").arg(themeColor));
            svg.replace(
                QStringLiteral("fill-opacity=\"context-fill-opacity\" fill=\"context-fill\""),
                QStringLiteral("fill-opacity=\"1\" fill=\"%1\"").arg(themeColor));
            svg.replace(QStringLiteral("fill-opacity=\"context-fill-opacity\""),
                        QStringLiteral("fill-opacity=\"1\""));
            // Handle stroke-trigger variants BEFORE the standalone fill="context-stroke"
            // replacement below; otherwise the original fill-opacity="context-stroke-opacity"
            // would survive and the element would end up with two fill-opacity attributes,
            // which QXmlStreamReader treats as a fatal parse error (the path silently fails to
            // render — see Thunderbird replies.svg's second <path>).
            svg.replace(
                QStringLiteral("fill=\"context-stroke\" fill-opacity=\"context-stroke-opacity\""),
                QStringLiteral("fill=\"%1\" fill-opacity=\"1\"").arg(themeColor));
            svg.replace(
                QStringLiteral("fill-opacity=\"context-stroke-opacity\" fill=\"context-stroke\""),
                QStringLiteral("fill-opacity=\"1\" fill=\"%1\"").arg(themeColor));
            svg.replace(QStringLiteral("fill-opacity=\"context-stroke-opacity\""),
                        QStringLiteral("fill-opacity=\"1\""));
            svg.replace(QStringLiteral("fill=\"context-fill transparent\""),
                        QStringLiteral("fill=\"%1\" fill-opacity=\"0.2\"").arg(themeColor));
            svg.replace(QStringLiteral("fill=\"context-stroke transparent\""),
                        QStringLiteral("fill=\"%1\" fill-opacity=\"1\"").arg(themeColor));
            svg.replace(QStringLiteral("fill=\"context-fill\""),
                        QStringLiteral("fill=\"%1\" fill-opacity=\"0.2\"").arg(themeColor));
            svg.replace(QStringLiteral("fill=\"context-stroke\""),
                        QStringLiteral("fill=\"%1\" fill-opacity=\"1\"").arg(themeColor));
            svg.replace(QStringLiteral("stroke=\"context-fill transparent\""),
                        QStringLiteral("stroke=\"%1\" stroke-opacity=\"0.2\"").arg(themeColor));
            svg.replace(QStringLiteral("stroke=\"context-stroke transparent\""),
                        QStringLiteral("stroke=\"%1\" stroke-opacity=\"1\"").arg(themeColor));
            svg.replace(QStringLiteral("stroke=\"context-fill\""),
                        QStringLiteral("stroke=\"%1\" stroke-opacity=\"0.2\"").arg(themeColor));
            svg.replace(QStringLiteral("stroke=\"context-stroke\""),
                        QStringLiteral("stroke=\"%1\" stroke-opacity=\"1\"").arg(themeColor));
            return svg.toUtf8();
        }

        [[nodiscard]] QPixmap renderThemedSvg(const QByteArray& svg, const QSize& size)
        {
            if (svg.isEmpty() || !size.isValid() || size.isEmpty())
            {
                return {};
            }

            QSvgRenderer renderer{svg};
            if (!renderer.isValid())
            {
                return {};
            }

            const auto* screen = QGuiApplication::primaryScreen();
            const auto devicePixelRatio =
                std::max(2.0, screen == nullptr ? 1.0 : screen->devicePixelRatio());
            const auto physicalSize =
                QSize{static_cast<int>(std::ceil(size.width() * devicePixelRatio)),
                      static_cast<int>(std::ceil(size.height() * devicePixelRatio))};

            QPixmap pixmap{physicalSize};
            pixmap.setDevicePixelRatio(devicePixelRatio);
            pixmap.fill(Qt::transparent);
            {
                QPainter painter{&pixmap};
                renderer.render(&painter, QRectF{QPointF{0, 0}, QSizeF{size}});
            }
            return pixmap;
        }

        class ThemedSvgIconEngine final : public QIconEngine
        {
          public:
            ThemedSvgIconEngine(QString resourcePath, QColor color)
                : m_resourcePath(std::move(resourcePath)),
                  m_svg(themedSvgData(m_resourcePath, color)), m_color(std::move(color))
            {
            }

            void paint(QPainter* painter, const QRect& rect, QIcon::Mode, QIcon::State) override
            {
                if (m_svg.isEmpty() || rect.isEmpty())
                {
                    return;
                }

                QSvgRenderer renderer{m_svg};
                if (renderer.isValid())
                {
                    renderer.render(painter, QRectF{rect});
                }
            }

            [[nodiscard]] QPixmap pixmap(const QSize& size, QIcon::Mode, QIcon::State) override
            {
                return renderThemedSvg(m_svg, size);
            }

            [[nodiscard]] QSize actualSize(const QSize& size, QIcon::Mode, QIcon::State) override
            {
                return size;
            }

            [[nodiscard]] QIconEngine* clone() const override
            {
                return new ThemedSvgIconEngine{m_resourcePath, m_color};
            }

          private:
            QString m_resourcePath;
            QByteArray m_svg;
            QColor m_color;
        };

    } // namespace

    QPixmap themedSvgPixmap(const QString& resourcePath, const QColor& color, const int size)
    {
        return renderThemedSvg(themedSvgData(resourcePath, color), QSize{size, size});
    }

    QIcon themedSvgIcon(const QString& resourcePath, const QColor& color)
    {
        return QIcon{new ThemedSvgIconEngine{resourcePath, color}};
    }

} // namespace javelin::gui
