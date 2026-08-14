#include "gui/NoRttiAccessibleObject.h"

#include <QObject>

namespace
{
    class NoRttiAccessibleObject final : public QObject
    {
      public:
        NoRttiAccessibleObject() = default;
        ~NoRttiAccessibleObject() override = default;
    };
} // namespace

NoRttiAccessibleObjectHandle::NoRttiAccessibleObjectHandle() : m_object(new NoRttiAccessibleObject)
{
}

NoRttiAccessibleObjectHandle::~NoRttiAccessibleObjectHandle()
{
    delete static_cast<NoRttiAccessibleObject*>(m_object);
}

QObject* NoRttiAccessibleObjectHandle::get() const
{
    return m_object;
}
