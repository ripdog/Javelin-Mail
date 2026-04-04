#include "app/ProcessServices.h"

#include "jmap/JmapCore.h"

#include <memory>

namespace javelin::app
{

    ProcessServices::ProcessServices() : m_jmapCore(std::make_unique<javelin::jmap::JmapCore>())
    {
    }

    ProcessServices::~ProcessServices() = default;

    javelin::jmap::JmapCore& ProcessServices::jmapCore()
    {
        return *m_jmapCore;
    }

    const javelin::jmap::JmapCore& ProcessServices::jmapCore() const
    {
        return *m_jmapCore;
    }

} // namespace javelin::app
