#pragma once

#include <QCoroTask>

#include <string>
#include <vector>

namespace javelin::app
{
    class ProcessServices;
}

namespace javelin::tools
{
    struct AutonomousSuiteAccount
    {
        std::string connectionId;
        std::vector<std::string> accountIds;
    };

    [[nodiscard]] QCoro::Task<int>
    runUndoRedoAutonomousSuite(javelin::app::ProcessServices& services,
                               AutonomousSuiteAccount account);
} // namespace javelin::tools
