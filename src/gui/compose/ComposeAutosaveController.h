#pragma once

#include <QObject>

#include <functional>

class QTimer;

namespace javelin::gui::compose
{
    class ComposeAutosaveController final : public QObject
    {
        Q_OBJECT

      public:
        ComposeAutosaveController(bool hasUnsavedChanges, std::function<void()> persist,
                                  QObject* parent = nullptr);

        [[nodiscard]] bool hasUnsavedChanges() const;
        void schedule();
        void setBusy(bool busy);
        void markSaved();

      private:
        QTimer* m_timer = nullptr;
        std::function<void()> m_persist;
        bool m_hasUnsavedChanges = false;
        bool m_busy = false;
    };
} // namespace javelin::gui::compose
