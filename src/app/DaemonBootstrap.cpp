#include "app/DaemonBootstrap.h"

#include "app/DaemonServices.h"

#include <memory>

namespace javelin::app
{
    DaemonBootstrap::DaemonBootstrap() : m_services(std::make_unique<DaemonServices>())
    {
    }

    DaemonBootstrap::~DaemonBootstrap() = default;

    DaemonServices& DaemonBootstrap::services()
    {
        return *m_services;
    }
} // namespace javelin::app
