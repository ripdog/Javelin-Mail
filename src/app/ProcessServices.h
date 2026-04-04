#pragma once

#include <memory>

namespace javelin::jmap
{
    class JmapCore;
}

namespace javelin::app
{

    class ProcessServices
    {
      public:
        ProcessServices();
        ~ProcessServices();

        ProcessServices(const ProcessServices&) = delete;
        ProcessServices& operator=(const ProcessServices&) = delete;
        ProcessServices(ProcessServices&&) = delete;
        ProcessServices& operator=(ProcessServices&&) = delete;

        [[nodiscard]] javelin::jmap::JmapCore& jmapCore();
        [[nodiscard]] const javelin::jmap::JmapCore& jmapCore() const;

      private:
        std::unique_ptr<javelin::jmap::JmapCore> m_jmapCore;
    };

} // namespace javelin::app
