#include "gui/compose/ComposeAutosaveController.h"

#include <QTimer>

#include <utility>

namespace javelin::gui::compose
{
    ComposeAutosaveController::ComposeAutosaveController(const bool hasUnsavedChanges,
                                                         std::function<void()> persist,
                                                         QObject* parent)
        : QObject(parent), m_persist(std::move(persist)), m_hasUnsavedChanges(hasUnsavedChanges)
    {
        m_timer = new QTimer(this);
        m_timer->setSingleShot(true);
        m_timer->setInterval(350);
        connect(m_timer, &QTimer::timeout, this,
                [this]
                {
                    if (!m_busy && m_persist)
                        m_persist();
                });
    }

    bool ComposeAutosaveController::hasUnsavedChanges() const
    {
        return m_hasUnsavedChanges;
    }

    void ComposeAutosaveController::schedule()
    {
        m_hasUnsavedChanges = true;
        if (!m_busy)
            m_timer->start();
    }

    void ComposeAutosaveController::setBusy(const bool busy)
    {
        m_busy = busy;
        if (m_busy)
            m_timer->stop();
        else if (m_hasUnsavedChanges)
            m_timer->start();
    }

    void ComposeAutosaveController::markSaved()
    {
        m_hasUnsavedChanges = false;
        m_timer->stop();
    }
} // namespace javelin::gui::compose
