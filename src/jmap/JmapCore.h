#pragma once

// JmapCore was retired by structural refactor phase 4.
//
// Depend on the narrow capability that owns the operation instead:
//   - SessionRefreshClient / AccountBootstrapClient
//   - MailQueryClient / MailQueryMaterializer
//   - MessageContentClient
//   - EmailMutationEngine
//   - MailboxMutationEngine
