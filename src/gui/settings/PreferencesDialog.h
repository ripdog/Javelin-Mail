#pragma once

#include <QDialog>

#include <optional>

class QLineEdit;

namespace javelin::gui::settings
{

    struct ConnectionSettings
    {
        QString sessionUrl;
        QString loginEmail;
        QString apiKey;
    };

    class PreferencesDialog : public QDialog
    {
        Q_OBJECT

      public:
        explicit PreferencesDialog(QWidget* parent = nullptr);
        ~PreferencesDialog() override;

        [[nodiscard]] ConnectionSettings settings() const;

        [[nodiscard]] static ConnectionSettings loadSettings();
        static void saveSettings(const ConnectionSettings& settings);

      private:
        QLineEdit* m_sessionUrlEdit = nullptr;
        QLineEdit* m_loginEmailEdit = nullptr;
        QLineEdit* m_apiKeyEdit = nullptr;
    };

} // namespace javelin::gui::settings
