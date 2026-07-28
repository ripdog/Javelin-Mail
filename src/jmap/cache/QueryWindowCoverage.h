#pragma once

namespace javelin::jmap::cache
{

    enum class QueryWindowCoverage
    {
        Server,
        LocallyProjected,
        Stale,
    };

    [[nodiscard]] constexpr bool isDisplayCurrent(const QueryWindowCoverage coverage)
    {
        return coverage != QueryWindowCoverage::Stale;
    }

    [[nodiscard]] constexpr bool isPaginationAuthoritative(const QueryWindowCoverage coverage)
    {
        return coverage == QueryWindowCoverage::Server;
    }

} // namespace javelin::jmap::cache
