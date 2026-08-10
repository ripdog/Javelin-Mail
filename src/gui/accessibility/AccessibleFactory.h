#pragma once

#include <QLatin1StringView>
#include <QObject>
#include <QString>

#include <concepts>

namespace javelin::gui::accessibility
{
    // QAccessible factories are process-global and receive arbitrary Qt objects, including
    // implementation types built without C++ RTTI. Match through Qt metadata before casting.
    template <typename Object>
        requires std::derived_from<Object, QObject>
    [[nodiscard]] Object* factoryObject(const QString& key, QObject* object)
    {
        if (key != QLatin1StringView{Object::staticMetaObject.className()})
            return nullptr;
        return qobject_cast<Object*>(object);
    }

    template <typename Object, typename QtBase>
        requires std::derived_from<Object, QtBase> && std::derived_from<QtBase, QObject>
    [[nodiscard]] Object* namedFactoryObject(const QString& key, QObject* object,
                                             const QLatin1StringView objectName)
    {
        auto* base = factoryObject<QtBase>(key, object);
        return base != nullptr && base->objectName() == objectName ? static_cast<Object*>(base)
                                                                   : nullptr;
    }

    template <typename Object, typename QtBase>
        requires std::derived_from<Object, QtBase> && std::derived_from<QtBase, QObject>
    [[nodiscard]] Object* namedObject(QObject* object, const QLatin1StringView objectName)
    {
        auto* base = qobject_cast<QtBase*>(object);
        return base != nullptr && base->objectName() == objectName ? static_cast<Object*>(base)
                                                                   : nullptr;
    }
} // namespace javelin::gui::accessibility
