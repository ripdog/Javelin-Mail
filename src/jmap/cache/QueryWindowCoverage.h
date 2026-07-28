#pragma once

namespace javelin::jmap::cache
{

    enum class QueryWindowCoverage
    {
        Server,
        LocallyProjected,
        Stale,
    };

    enum class QueryWindowMaterialization
    {
        Complete,
        Partial,
    };

    [[nodiscard]] constexpr bool isDisplayCurrent(const QueryWindowCoverage coverage,
                                                  const QueryWindowMaterialization materialization)
    {
        return coverage != QueryWindowCoverage::Stale &&
               materialization == QueryWindowMaterialization::Complete;
    }

    [[nodiscard]] constexpr bool
    isPaginationAuthoritative(const QueryWindowCoverage coverage,
                              const QueryWindowMaterialization materialization)
    {
        return coverage == QueryWindowCoverage::Server &&
               materialization == QueryWindowMaterialization::Complete;
    }

} // namespace javelin::jmap::cache
