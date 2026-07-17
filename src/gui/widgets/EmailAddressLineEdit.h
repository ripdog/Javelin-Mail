#pragma once

#include <QLineEdit>
#include <QPlainTextEdit>

class QCompleter;

namespace javelin::gui::widgets
{
    class EmailAddressLineEdit final : public QLineEdit
    {
        Q_OBJECT

      public:
        explicit EmailAddressLineEdit(bool multipleAddresses, QWidget* parent = nullptr);
        EmailAddressLineEdit(const QString& text, bool multipleAddresses,
                             QWidget* parent = nullptr);

      private:
        void configureCompletion(bool multipleAddresses);

        QCompleter* m_completer = nullptr;
    };

    class EmailAddressPlainTextEdit final : public QPlainTextEdit
    {
        Q_OBJECT

      public:
        explicit EmailAddressPlainTextEdit(QWidget* parent = nullptr);

      private:
        QCompleter* m_completer = nullptr;
    };
} // namespace javelin::gui::widgets
