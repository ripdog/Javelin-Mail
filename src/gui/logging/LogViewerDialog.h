#pragma once

#include "app/LogStore.h"

#include <QDialog>

class QComboBox;
class QLineEdit;
class QPlainTextEdit;

namespace javelin::gui::logging
{
    class LogViewerDialog final : public QDialog
    {
        Q_OBJECT
      public:
        explicit LogViewerDialog(QWidget* parent = nullptr);

      private:
        void rebuild();
        void append(const javelin::app::LogEntry& entry);
        [[nodiscard]] bool accepts(const javelin::app::LogEntry& entry) const;

        QComboBox* m_level = nullptr;
        QComboBox* m_subsystem = nullptr;
        QLineEdit* m_search = nullptr;
        QPlainTextEdit* m_output = nullptr;
    };
} // namespace javelin::gui::logging
