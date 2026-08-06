#pragma once

#include "jmap/domain/MailEntities.h"

#include <QString>

#include <cstddef>

namespace javelin::gui::compose
{
    [[nodiscard]] QString identityAddressLabel(const javelin::jmap::domain::Identity& identity);
    [[nodiscard]] QString identitySignaturePreview(const javelin::jmap::domain::Identity& identity);
    [[nodiscard]] QString
    composeIdentityDisplayText(const javelin::jmap::domain::Identity& identity,
                               const QString& accountDisplayName,
                               std::size_t sameAddressIdentityCount, bool includeAccountName);
    [[nodiscard]] bool isWildcardSenderIdentity(const javelin::jmap::domain::Identity& identity);
} // namespace javelin::gui::compose
