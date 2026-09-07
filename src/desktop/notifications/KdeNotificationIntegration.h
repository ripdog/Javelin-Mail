#pragma once

#include <QString>

#include <memory>

namespace javelin::app
{
    struct KdeNotificationPresentation
    {
        bool popup = true;
        bool sound = false;
        QString soundName;
    };

    class KdeNotificationIntegration final
    {
      public:
        KdeNotificationIntegration(QString applicationName, QString applicationDisplayName,
                                   QString desktopEntry, QString iconName);
        ~KdeNotificationIntegration();

        KdeNotificationIntegration(const KdeNotificationIntegration&) = delete;
        KdeNotificationIntegration& operator=(const KdeNotificationIntegration&) = delete;

        [[nodiscard]] KdeNotificationPresentation
        presentationFor(const QString& eventId, const QString& fallbackSoundName = {}) const;
        [[nodiscard]] bool playSound(const QString& soundName);

      private:
        struct Private;
        std::unique_ptr<Private> d;
    };

} // namespace javelin::app
