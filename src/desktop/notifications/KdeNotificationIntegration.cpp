#include "desktop/notifications/KdeNotificationIntegration.h"

#include <KConfig>
#include <KConfigGroup>

#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QUrl>

#include <canberra.h>

#include <utility>

namespace javelin::app
{
    namespace
    {
        constexpr auto notificationDataDirectory = "knotifications6/";
        constexpr auto notificationConfigSuffix = ".notifyrc";
        constexpr auto defaultSoundTheme = "ocean";

        [[nodiscard]] QString readNotificationEntry(KConfig& userConfig, KConfig& eventConfig,
                                                    const QString& groupName, const QString& key)
        {
            if (userConfig.hasGroup(groupName))
            {
                const KConfigGroup group{&userConfig, groupName};
                if (group.hasKey(key))
                    return group.readEntry(key, QString{});
            }
            if (eventConfig.hasGroup(groupName))
            {
                const KConfigGroup group{&eventConfig, groupName};
                if (group.hasKey(key))
                    return group.readEntry(key, QString{});
            }
            return {};
        }

        [[nodiscard]] QString soundFallbackPath(const QString& soundName)
        {
            const auto dataLocations =
                QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation);
            for (const auto& dataLocation : dataLocations)
            {
                const auto url = QUrl::fromUserInput(
                    soundName, dataLocation + QStringLiteral("/sounds"), QUrl::AssumeLocalFile);
                if (url.isLocalFile() && QFileInfo::exists(url.toLocalFile()))
                    return url.toLocalFile();
            }
            return {};
        }
    } // namespace

    struct KdeNotificationIntegration::Private
    {
        Private(QString applicationNameValue, QString applicationDisplayNameValue,
                QString desktopEntryValue, QString iconNameValue)
            : applicationName(std::move(applicationNameValue)),
              applicationDisplayName(std::move(applicationDisplayNameValue)),
              desktopEntry(std::move(desktopEntryValue)), iconName(std::move(iconNameValue))
        {
        }

        ~Private()
        {
            if (soundContext != nullptr)
                ca_context_destroy(soundContext);
        }

        [[nodiscard]] bool ensureSoundContext()
        {
            if (soundContext != nullptr)
                return true;

            const int createResult = ca_context_create(&soundContext);
            if (createResult != CA_SUCCESS)
            {
                qWarning().noquote() << "Failed to initialize desktop notification sound context:"
                                     << ca_strerror(createResult);
                soundContext = nullptr;
                return false;
            }

            const auto displayNameUtf8 = applicationDisplayName.toUtf8();
            const auto desktopEntryUtf8 = desktopEntry.toUtf8();
            const auto iconNameUtf8 = iconName.toUtf8();
            const int propertyResult = ca_context_change_props(
                soundContext, CA_PROP_APPLICATION_NAME, displayNameUtf8.constData(),
                CA_PROP_APPLICATION_ID, desktopEntryUtf8.constData(), CA_PROP_APPLICATION_ICON_NAME,
                iconNameUtf8.constData(), nullptr);
            if (propertyResult != CA_SUCCESS)
            {
                qWarning().noquote() << "Failed to identify desktop notification sound context:"
                                     << ca_strerror(propertyResult);
            }
            return true;
        }

        QString applicationName;
        QString applicationDisplayName;
        QString desktopEntry;
        QString iconName;
        ca_context* soundContext = nullptr;
        uint32_t nextSoundId = 1;
    };

    KdeNotificationIntegration::KdeNotificationIntegration(QString applicationName,
                                                           QString applicationDisplayName,
                                                           QString desktopEntry, QString iconName)
        : d(std::make_unique<Private>(std::move(applicationName), std::move(applicationDisplayName),
                                      std::move(desktopEntry), std::move(iconName)))
    {
    }

    KdeNotificationIntegration::~KdeNotificationIntegration() = default;

    KdeNotificationPresentation
    KdeNotificationIntegration::presentationFor(const QString& eventId,
                                                const QString& fallbackSoundName) const
    {
        KConfig eventConfig{QString::fromLatin1(notificationDataDirectory) + d->applicationName +
                                QString::fromLatin1(notificationConfigSuffix),
                            KConfig::NoGlobals, QStandardPaths::GenericDataLocation};
        KConfig userConfig{d->applicationName + QString::fromLatin1(notificationConfigSuffix),
                           KConfig::NoGlobals, QStandardPaths::GenericConfigLocation};
        const auto groupName = QStringLiteral("Event/") + eventId;
        const bool configured = userConfig.hasGroup(groupName) || eventConfig.hasGroup(groupName);
        if (!configured)
        {
            return KdeNotificationPresentation{
                .popup = true,
                .sound = !fallbackSoundName.isEmpty(),
                .soundName = fallbackSoundName,
            };
        }

        const auto action =
            readNotificationEntry(userConfig, eventConfig, groupName, QStringLiteral("Action"));
        const auto actions = action.split(QLatin1Char('|'), Qt::SkipEmptyParts);
        auto soundName =
            readNotificationEntry(userConfig, eventConfig, groupName, QStringLiteral("Sound"));

        return KdeNotificationPresentation{
            .popup = actions.contains(QStringLiteral("Popup")),
            .sound = actions.contains(QStringLiteral("Sound")),
            .soundName = std::move(soundName),
        };
    }

    bool KdeNotificationIntegration::playSound(const QString& soundName)
    {
        if (soundName.isEmpty())
        {
            qWarning() << "Desktop notification requested sound without a sound name";
            return false;
        }

        KConfig kdeGlobals{QStringLiteral("kdeglobals"), KConfig::NoGlobals,
                           QStandardPaths::GenericConfigLocation};
        const KConfigGroup sounds{&kdeGlobals, QStringLiteral("Sounds")};
        if (!sounds.readEntry("Enable", true))
            return true;
        const auto soundTheme = sounds.readEntry("Theme", QString::fromLatin1(defaultSoundTheme));

        if (!d->ensureSoundContext())
            return false;

        ca_proplist* properties = nullptr;
        if (ca_proplist_create(&properties) != CA_SUCCESS || properties == nullptr)
        {
            qWarning() << "Failed to allocate desktop notification sound properties";
            return false;
        }

        const auto soundNameUtf8 = soundName.toUtf8();
        const auto soundThemeUtf8 = soundTheme.toUtf8();
        ca_proplist_sets(properties, CA_PROP_EVENT_ID, soundNameUtf8.constData());
        ca_proplist_sets(properties, CA_PROP_CANBERRA_XDG_THEME_NAME, soundThemeUtf8.constData());

        const auto fallbackPath = soundFallbackPath(soundName);
        const auto fallbackPathBytes = QFile::encodeName(fallbackPath);
        if (!fallbackPath.isEmpty())
            ca_proplist_sets(properties, CA_PROP_MEDIA_FILENAME, fallbackPathBytes.constData());
        ca_proplist_sets(properties, CA_PROP_CANBERRA_CACHE_CONTROL, "volatile");

        const int playResult =
            ca_context_play_full(d->soundContext, d->nextSoundId++, properties, nullptr, nullptr);
        ca_proplist_destroy(properties);
        if (playResult != CA_SUCCESS)
        {
            qWarning().noquote() << "Failed to play desktop notification sound:"
                                 << ca_strerror(playResult);
            return false;
        }
        return true;
    }

} // namespace javelin::app
