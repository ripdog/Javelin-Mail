#pragma once

#include <cstddef>
#include <functional>

class QHBoxLayout;
class QScrollArea;
class QWidget;

namespace javelin::jmap::submission
{
    struct DraftSnapshot;
}

namespace javelin::gui::compose
{
    class AttachmentController final
    {
      public:
        AttachmentController(QScrollArea& scrollArea, QWidget& strip, QHBoxLayout& layout,
                             javelin::jmap::submission::DraftSnapshot& snapshot,
                             std::function<void(std::size_t)> removeAction,
                             std::function<void(std::size_t, bool)> embedAction);

        void refresh(bool busy);

      private:
        QScrollArea& m_scrollArea;
        QWidget& m_strip;
        QHBoxLayout& m_layout;
        javelin::jmap::submission::DraftSnapshot& m_snapshot;
        std::function<void(std::size_t)> m_removeAction;
        std::function<void(std::size_t, bool)> m_embedAction;
    };
} // namespace javelin::gui::compose
