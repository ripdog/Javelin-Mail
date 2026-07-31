#pragma once

#include <QCoroTask>

#include <string>
#include <vector>

namespace javelin::app
{
    class DaemonServices;
}

namespace javelin::tools
{
    struct AutonomousSuiteAccount
    {
        std::string connectionId;
        std::vector<std::string> accountIds;
    };

    [[nodiscard]] QCoro::Task<int>
    runUndoRedoAutonomousSuite(javelin::app::DaemonServices& services,
                               AutonomousSuiteAccount account);
} // namespace javelin::tools
