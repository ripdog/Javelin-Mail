#pragma once

#include "jmap/cache/Database.h"

#include <memory>

namespace javelin::jmap
{
    class JmapCore;
}

namespace javelin::jmap::cache
{
    class AccountRepository;
    class MessageViewService;
    class QueryService;
} // namespace javelin::jmap::cache

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
        [[nodiscard]] javelin::jmap::cache::AccountRepository& accountRepository();
        [[nodiscard]] javelin::jmap::cache::MessageViewService& messageViewService();
        [[nodiscard]] javelin::jmap::cache::QueryService& queryService();

      private:
        std::unique_ptr<javelin::jmap::JmapCore> m_jmapCore;
        javelin::jmap::cache::DatabaseConnection m_databaseConnection;
        std::unique_ptr<javelin::jmap::cache::AccountRepository> m_accountRepository;
        std::unique_ptr<javelin::jmap::cache::MessageViewService> m_messageViewService;
        std::unique_ptr<javelin::jmap::cache::QueryService> m_queryService;
    };

} // namespace javelin::app
