#pragma once

#include <memory>

namespace javelin::app
{
    class DaemonServices;

    class DaemonBootstrap final
    {
      public:
        DaemonBootstrap();
        ~DaemonBootstrap();

        DaemonBootstrap(const DaemonBootstrap&) = delete;
        DaemonBootstrap& operator=(const DaemonBootstrap&) = delete;
        DaemonBootstrap(DaemonBootstrap&&) = delete;
        DaemonBootstrap& operator=(DaemonBootstrap&&) = delete;

        [[nodiscard]] DaemonServices& services();

      private:
        std::unique_ptr<DaemonServices> m_services;
    };
} // namespace javelin::app
