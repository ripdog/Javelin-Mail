#include <KLocalizedString>

#include <QByteArray>

namespace
{
    struct TranslationDomainInitializer final
    {
        TranslationDomainInitializer()
        {
            KLocalizedString::setApplicationDomain(QByteArrayLiteral("javelinmail"));
        }
    };

    [[maybe_unused]] const TranslationDomainInitializer translationDomainInitializer;
} // namespace
