#include "jmap/render/HtmlTextExtractor.h"

#include <catch2/catch_test_macros.hpp>

#include <QMessageLogContext>
#include <QString>
#include <QtLogging>

#include <atomic>

namespace
{
    std::atomic_int invalidPixelSizeWarnings = 0;
    QtMessageHandler previousMessageHandler = nullptr;

    void captureInvalidPixelSizeWarning(const QtMsgType type, const QMessageLogContext& context,
                                        const QString& message)
    {
        if (message.startsWith(QStringLiteral("QFont::setPixelSize: Pixel size <= 0")))
            ++invalidPixelSizeWarnings;
        if (previousMessageHandler != nullptr)
            previousMessageHandler(type, context, message);
    }

    class WarningCapture final
    {
      public:
        WarningCapture()
        {
            invalidPixelSizeWarnings = 0;
            previousMessageHandler = qInstallMessageHandler(captureInvalidPixelSizeWarning);
        }

        ~WarningCapture()
        {
            qInstallMessageHandler(previousMessageHandler);
            previousMessageHandler = nullptr;
        }

        WarningCapture(const WarningCapture&) = delete;
        WarningCapture& operator=(const WarningCapture&) = delete;
    };
} // namespace

TEST_CASE("HTML text extraction accepts zero-sized email markup")
{
    const WarningCapture warningCapture;
    const auto text = javelin::jmap::render::plainTextFromHtml(QStringLiteral(
        R"(<style>.preheader { FONT-SIZE: 0 !important; }</style>
           <p style="font-size:0px">Hidden preheader</p>
           <table><tr><td style='font-size: 0.0pt; line-height: 0'>Spacer</td></tr></table>
           <p>Visible body</p>)"));

    CHECK(text.contains(QStringLiteral("Hidden preheader")));
    CHECK(text.contains(QStringLiteral("Spacer")));
    CHECK(text.contains(QStringLiteral("Visible body")));
    CHECK(invalidPixelSizeWarnings == 0);
}
