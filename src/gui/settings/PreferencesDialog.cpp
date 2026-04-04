#include "gui/settings/PreferencesDialog.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QSettings>
#include <QVBoxLayout>

namespace javelin::gui::settings
{
    namespace
    {
        constexpr auto connectionGroup = "connection";
        constexpr auto sessionUrlKey = "sessionUrl";
        constexpr auto loginEmailKey = "loginEmail";
        constexpr auto apiKeyKey = "apiKey";

    } // namespace

    PreferencesDialog::PreferencesDialog(QWidget* parent) : QDialog(parent)
    {
        setWindowTitle(QStringLiteral("Preferences"));
        resize(560, 0);

        auto* layout = new QVBoxLayout(this);
        auto* formLayout = new QFormLayout();

        m_sessionUrlEdit = new QLineEdit(this);
        m_sessionUrlEdit->setPlaceholderText(
            QStringLiteral("https://api.fastmail.com/jmap/session"));

        m_loginEmailEdit = new QLineEdit(this);
        m_loginEmailEdit->setPlaceholderText(QStringLiteral("name@example.com"));

        m_apiKeyEdit = new QLineEdit(this);
        m_apiKeyEdit->setEchoMode(QLineEdit::Password);
        m_apiKeyEdit->setPlaceholderText(QStringLiteral("Paste API key"));

        formLayout->addRow(QStringLiteral("Session URL"), m_sessionUrlEdit);
        formLayout->addRow(QStringLiteral("Login Email"), m_loginEmailEdit);
        formLayout->addRow(QStringLiteral("API Key"), m_apiKeyEdit);

        auto* buttonBox =
            new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        connect(buttonBox, &QDialogButtonBox::accepted, this,
                [this]
                {
                    saveSettings(settings());
                    accept();
                });
        connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

        const auto storedSettings = loadSettings();
        m_sessionUrlEdit->setText(storedSettings.sessionUrl);
        m_loginEmailEdit->setText(storedSettings.loginEmail);
        m_apiKeyEdit->setText(storedSettings.apiKey);

        layout->addLayout(formLayout);
        layout->addWidget(buttonBox);
    }

    PreferencesDialog::~PreferencesDialog() = default;

    ConnectionSettings PreferencesDialog::settings() const
    {
        return ConnectionSettings{
            .sessionUrl = m_sessionUrlEdit->text().trimmed(),
            .loginEmail = m_loginEmailEdit->text().trimmed(),
            .apiKey = m_apiKeyEdit->text().trimmed(),
        };
    }

    ConnectionSettings PreferencesDialog::loadSettings()
    {
        QSettings settings;
        settings.beginGroup(QLatin1StringView{connectionGroup});
        const ConnectionSettings loadedSettings{
            .sessionUrl = settings.value(QLatin1StringView{sessionUrlKey}).toString().trimmed(),
            .loginEmail = settings.value(QLatin1StringView{loginEmailKey}).toString().trimmed(),
            .apiKey = settings.value(QLatin1StringView{apiKeyKey}).toString().trimmed(),
        };
        settings.endGroup();
        return loadedSettings;
    }

    void PreferencesDialog::saveSettings(const ConnectionSettings& connectionSettings)
    {
        QSettings settings;
        settings.beginGroup(QLatin1StringView{connectionGroup});
        settings.setValue(QLatin1StringView{sessionUrlKey}, connectionSettings.sessionUrl);
        settings.setValue(QLatin1StringView{loginEmailKey}, connectionSettings.loginEmail);
        settings.setValue(QLatin1StringView{apiKeyKey}, connectionSettings.apiKey);
        settings.endGroup();
        settings.sync();
    }

} // namespace javelin::gui::settings
