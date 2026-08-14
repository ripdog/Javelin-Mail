#include "storage/migrations/DefaultMigrationSteps.h"

namespace javelin::jmap::cache::migrations
{
    std::vector<MigrationStep> migrationSteps1To13()
    {
        return {
            MigrationStep{
                .version = 1,
                .name = QStringLiteral("initial_cache_schema"),
                .statements =
                    {
                        QStringLiteral("CREATE TABLE IF NOT EXISTS accounts ("
                                       "account_id TEXT PRIMARY KEY,"
                                       "email_address TEXT NOT NULL,"
                                       "session_url TEXT NOT NULL,"
                                       "is_primary INTEGER NOT NULL DEFAULT 0,"
                                       "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
                                       "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP"
                                       ") STRICT"),
                        QStringLiteral(
                            "CREATE TABLE IF NOT EXISTS sessions ("
                            "account_id TEXT PRIMARY KEY REFERENCES accounts(account_id) ON "
                            "DELETE CASCADE,"
                            "api_url TEXT NOT NULL,"
                            "download_url TEXT,"
                            "upload_url TEXT,"
                            "event_source_url TEXT,"
                            "state TEXT,"
                            "username TEXT NOT NULL"
                            ") STRICT"),
                        QStringLiteral("CREATE TABLE IF NOT EXISTS mailboxes ("
                                       "account_id TEXT NOT NULL REFERENCES "
                                       "accounts(account_id) ON DELETE "
                                       "CASCADE,"
                                       "mailbox_id TEXT NOT NULL,"
                                       "parent_mailbox_id TEXT,"
                                       "name TEXT NOT NULL,"
                                       "role TEXT,"
                                       "sort_order INTEGER NOT NULL DEFAULT 0,"
                                       "total_emails INTEGER NOT NULL DEFAULT 0,"
                                       "unread_emails INTEGER NOT NULL DEFAULT 0,"
                                       "total_threads INTEGER NOT NULL DEFAULT 0,"
                                       "unread_threads INTEGER NOT NULL DEFAULT 0,"
                                       "rights_json TEXT NOT NULL DEFAULT '{}',"
                                       "state TEXT,"
                                       "PRIMARY KEY (account_id, mailbox_id)"
                                       ") STRICT"),
                        QStringLiteral("CREATE TABLE IF NOT EXISTS threads ("
                                       "account_id TEXT NOT NULL REFERENCES "
                                       "accounts(account_id) ON DELETE "
                                       "CASCADE,"
                                       "thread_id TEXT NOT NULL,"
                                       "email_ids_json TEXT NOT NULL DEFAULT '[]',"
                                       "state TEXT,"
                                       "PRIMARY KEY (account_id, thread_id)"
                                       ") STRICT"),
                        QStringLiteral("CREATE TABLE IF NOT EXISTS emails ("
                                       "account_id TEXT NOT NULL REFERENCES "
                                       "accounts(account_id) ON DELETE "
                                       "CASCADE,"
                                       "email_id TEXT NOT NULL,"
                                       "thread_id TEXT,"
                                       "blob_id TEXT,"
                                       "received_at TEXT,"
                                       "sent_at TEXT,"
                                       "subject TEXT NOT NULL DEFAULT '',"
                                       "preview TEXT NOT NULL DEFAULT '',"
                                       "mailbox_ids_json TEXT NOT NULL DEFAULT '[]',"
                                       "keywords_json TEXT NOT NULL DEFAULT '{}',"
                                       "has_attachment INTEGER NOT NULL DEFAULT 0,"
                                       "size INTEGER NOT NULL DEFAULT 0,"
                                       "state TEXT,"
                                       "PRIMARY KEY (account_id, email_id)"
                                       ") STRICT"),
                        QStringLiteral("CREATE TABLE IF NOT EXISTS email_mailboxes ("
                                       "account_id TEXT NOT NULL REFERENCES "
                                       "accounts(account_id) ON DELETE "
                                       "CASCADE,"
                                       "email_id TEXT NOT NULL,"
                                       "mailbox_id TEXT NOT NULL,"
                                       "PRIMARY KEY (account_id, email_id, mailbox_id)"
                                       ") STRICT"),
                        QStringLiteral("CREATE TABLE IF NOT EXISTS email_keywords ("
                                       "account_id TEXT NOT NULL REFERENCES "
                                       "accounts(account_id) ON DELETE "
                                       "CASCADE,"
                                       "email_id TEXT NOT NULL,"
                                       "keyword TEXT NOT NULL,"
                                       "PRIMARY KEY (account_id, email_id, keyword)"
                                       ") STRICT"),
                        QStringLiteral(
                            "CREATE TABLE IF NOT EXISTS email_addresses ("
                            "account_id TEXT NOT NULL REFERENCES accounts(account_id) ON "
                            "DELETE "
                            "CASCADE,"
                            "email_id TEXT NOT NULL,"
                            "field_name TEXT NOT NULL,"
                            "position INTEGER NOT NULL,"
                            "display_name TEXT,"
                            "address TEXT NOT NULL,"
                            "PRIMARY KEY (account_id, email_id, field_name, position)"
                            ") STRICT"),
                        QStringLiteral("CREATE TABLE IF NOT EXISTS identities ("
                                       "account_id TEXT NOT NULL REFERENCES "
                                       "accounts(account_id) ON DELETE "
                                       "CASCADE,"
                                       "identity_id TEXT NOT NULL,"
                                       "email_address TEXT NOT NULL,"
                                       "name TEXT,"
                                       "reply_to_json TEXT NOT NULL DEFAULT '[]',"
                                       "bcc_json TEXT NOT NULL DEFAULT '[]',"
                                       "text_signature TEXT,"
                                       "html_signature TEXT,"
                                       "may_delete INTEGER NOT NULL DEFAULT 0,"
                                       "state TEXT,"
                                       "PRIMARY KEY (account_id, identity_id)"
                                       ") STRICT"),
                        QStringLiteral("CREATE TABLE IF NOT EXISTS submissions ("
                                       "account_id TEXT NOT NULL REFERENCES "
                                       "accounts(account_id) ON DELETE "
                                       "CASCADE,"
                                       "submission_id TEXT NOT NULL,"
                                       "email_id TEXT NOT NULL,"
                                       "thread_id TEXT,"
                                       "envelope_json TEXT NOT NULL DEFAULT '{}',"
                                       "undo_status TEXT,"
                                       "delivery_status TEXT,"
                                       "state TEXT,"
                                       "PRIMARY KEY (account_id, submission_id)"
                                       ") STRICT"),
                        QStringLiteral("CREATE TABLE IF NOT EXISTS sync_state ("
                                       "account_id TEXT NOT NULL REFERENCES "
                                       "accounts(account_id) ON DELETE "
                                       "CASCADE,"
                                       "object_type TEXT NOT NULL,"
                                       "query_key TEXT NOT NULL DEFAULT '',"
                                       "state_token TEXT NOT NULL,"
                                       "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
                                       "PRIMARY KEY (account_id, object_type, query_key)"
                                       ") STRICT"),
                        QStringLiteral("CREATE TABLE IF NOT EXISTS pending_actions ("
                                       "pending_action_id TEXT PRIMARY KEY,"
                                       "account_id TEXT NOT NULL REFERENCES "
                                       "accounts(account_id) ON DELETE "
                                       "CASCADE,"
                                       "action_type TEXT NOT NULL,"
                                       "status TEXT NOT NULL,"
                                       "payload_json TEXT NOT NULL,"
                                       "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
                                       "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP"
                                       ") STRICT"),
                        QStringLiteral(
                            "CREATE TABLE IF NOT EXISTS notifications ("
                            "notification_id TEXT PRIMARY KEY,"
                            "account_id TEXT REFERENCES accounts(account_id) ON DELETE CASCADE,"
                            "kind TEXT NOT NULL,"
                            "payload_json TEXT NOT NULL,"
                            "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
                            "read_at TEXT"
                            ") STRICT"),
                        QStringLiteral("CREATE TABLE IF NOT EXISTS settings ("
                                       "scope TEXT NOT NULL,"
                                       "key TEXT NOT NULL,"
                                       "value_json TEXT NOT NULL,"
                                       "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
                                       "PRIMARY KEY (scope, key)"
                                       ") STRICT"),
                        QStringLiteral(
                            "CREATE INDEX IF NOT EXISTS idx_mailboxes_parent ON mailboxes "
                            "(account_id, parent_mailbox_id, sort_order, mailbox_id)"),
                        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_emails_thread ON emails "
                                       "(account_id, "
                                       "thread_id)"),
                        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_emails_received ON emails "
                                       "(account_id, received_at DESC, email_id)"),
                        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_email_mailboxes_mailbox ON "
                                       "email_mailboxes (account_id, mailbox_id, email_id)"),
                        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_email_keywords_keyword ON "
                                       "email_keywords (account_id, keyword, email_id)"),
                        QStringLiteral(
                            "CREATE INDEX IF NOT EXISTS idx_sync_state_object ON sync_state "
                            "(account_id, object_type, query_key)"),
                        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_pending_actions_status ON "
                                       "pending_actions (account_id, status, created_at)"),
                    },
            },
            MigrationStep{
                .version = 2,
                .name = QStringLiteral("mailboxes_is_subscribed"),
                .statements =
                    {
                        QStringLiteral(
                            "ALTER TABLE mailboxes ADD COLUMN is_subscribed INTEGER NOT NULL "
                            "DEFAULT 0"),
                    },
            },
            MigrationStep{
                .version = 3,
                .name = QStringLiteral("session_and_account_metadata"),
                .statements =
                    {
                        QStringLiteral(
                            "ALTER TABLE accounts ADD COLUMN name TEXT NOT NULL DEFAULT ''"),
                        QStringLiteral("ALTER TABLE accounts ADD COLUMN is_personal INTEGER "
                                       "NOT NULL DEFAULT "
                                       "0"),
                        QStringLiteral("ALTER TABLE accounts ADD COLUMN is_read_only INTEGER "
                                       "NOT NULL DEFAULT "
                                       "0"),
                        QStringLiteral("ALTER TABLE accounts ADD COLUMN cap_mail INTEGER NOT "
                                       "NULL DEFAULT 0"),
                        QStringLiteral(
                            "ALTER TABLE accounts ADD COLUMN cap_submission INTEGER NOT NULL "
                            "DEFAULT 0"),
                        QStringLiteral("ALTER TABLE sessions ADD COLUMN has_core_capability "
                                       "INTEGER NOT NULL "
                                       "DEFAULT 0"),
                        QStringLiteral("ALTER TABLE sessions ADD COLUMN has_mail_capability "
                                       "INTEGER NOT NULL "
                                       "DEFAULT 0"),
                        QStringLiteral("ALTER TABLE sessions ADD COLUMN "
                                       "has_submission_capability INTEGER NOT "
                                       "NULL DEFAULT 0"),
                        QStringLiteral("ALTER TABLE sessions ADD COLUMN core_capabilities_json "
                                       "TEXT NOT NULL "
                                       "DEFAULT 'null'"),
                        QStringLiteral(
                            "ALTER TABLE sessions ADD COLUMN primary_mail_account_id TEXT"),
                        QStringLiteral("ALTER TABLE sessions ADD COLUMN "
                                       "primary_submission_account_id TEXT"),
                    },
            },
            MigrationStep{
                .version = 4,
                .name = QStringLiteral("compose_and_threading_metadata"),
                .statements =
                    {
                        QStringLiteral("ALTER TABLE emails ADD COLUMN message_id_json TEXT "
                                       "NOT NULL DEFAULT '[]'"),
                        QStringLiteral("ALTER TABLE emails ADD COLUMN in_reply_to_json TEXT "
                                       "NOT NULL DEFAULT '[]'"),
                        QStringLiteral("ALTER TABLE emails ADD COLUMN references_json TEXT "
                                       "NOT NULL DEFAULT '[]'"),
                        QStringLiteral("CREATE TABLE IF NOT EXISTS compose_sessions ("
                                       "compose_session_id TEXT PRIMARY KEY,"
                                       "account_id TEXT NOT NULL REFERENCES "
                                       "accounts(account_id) ON DELETE CASCADE,"
                                       "draft_email_id TEXT,"
                                       "mode TEXT NOT NULL,"
                                       "editor_mode TEXT NOT NULL,"
                                       "snapshot_json TEXT NOT NULL,"
                                       "last_saved_at TEXT,"
                                       "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP"
                                       ") STRICT"),
                        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_compose_sessions_account ON "
                                       "compose_sessions (account_id, updated_at DESC)"),
                    },
            },
            MigrationStep{
                .version = 5,
                .name = QStringLiteral("raw_message_sources"),
                .statements =
                    {
                        QStringLiteral("CREATE TABLE IF NOT EXISTS raw_message_sources ("
                                       "account_id TEXT NOT NULL REFERENCES "
                                       "accounts(account_id) ON DELETE CASCADE,"
                                       "email_id TEXT NOT NULL,"
                                       "blob_id TEXT NOT NULL,"
                                       "payload BLOB NOT NULL,"
                                       "fetched_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
                                       "PRIMARY KEY (account_id, email_id)"
                                       ") STRICT"),
                        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_raw_message_sources_blob ON "
                                       "raw_message_sources (account_id, blob_id)"),
                    },
            },
            MigrationStep{
                .version = 8,
                .name = QStringLiteral("account_session_ownership"),
                .statements =
                    {
                        QStringLiteral("ALTER TABLE accounts ADD COLUMN owner_account_id TEXT"),
                        QStringLiteral("UPDATE accounts SET owner_account_id = account_id"),
                        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_accounts_owner ON accounts "
                                       "(owner_account_id, account_id)"),
                    },
            },
            MigrationStep{
                .version = 9,
                .name = QStringLiteral("ensure_raw_message_sources"),
                .statements =
                    {
                        QStringLiteral("CREATE TABLE IF NOT EXISTS raw_message_sources ("
                                       "account_id TEXT NOT NULL REFERENCES "
                                       "accounts(account_id) ON DELETE CASCADE,"
                                       "email_id TEXT NOT NULL,"
                                       "blob_id TEXT NOT NULL,"
                                       "payload BLOB NOT NULL,"
                                       "fetched_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
                                       "PRIMARY KEY (account_id, email_id)"
                                       ") STRICT"),
                        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_raw_message_sources_blob ON "
                                       "raw_message_sources (account_id, blob_id)"),
                    },
            },
            MigrationStep{
                .version = 10,
                .name = QStringLiteral("translation_cache"),
                .statements =
                    {
                        QStringLiteral("CREATE TABLE IF NOT EXISTS translation_cache ("
                                       "source_language TEXT NOT NULL,"
                                       "target_language TEXT NOT NULL,"
                                       "input_hash TEXT NOT NULL,"
                                       "input_text TEXT NOT NULL,"
                                       "translated_text TEXT NOT NULL,"
                                       "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
                                       "PRIMARY KEY (source_language, target_language, "
                                       "input_hash)"
                                       ") STRICT"),
                        QStringLiteral(
                            "CREATE INDEX IF NOT EXISTS idx_translation_cache_updated ON "
                            "translation_cache (updated_at DESC)"),
                    },
            },
            MigrationStep{
                .version = 11,
                .name = QStringLiteral("contacts_capabilities"),
                .statements =
                    {
                        QStringLiteral("ALTER TABLE accounts ADD COLUMN cap_contacts INTEGER "
                                       "NOT NULL DEFAULT 0"),
                        QStringLiteral("ALTER TABLE accounts ADD COLUMN "
                                       "contacts_capabilities_json TEXT NOT NULL DEFAULT "
                                       "'null'"),
                        QStringLiteral("ALTER TABLE sessions ADD COLUMN "
                                       "has_contacts_capability INTEGER NOT NULL DEFAULT 0"),
                        QStringLiteral("ALTER TABLE sessions ADD COLUMN "
                                       "primary_contacts_account_id TEXT"),
                    },
            },
            MigrationStep{
                .version = 12,
                .name = QStringLiteral("contacts_cache"),
                .statements =
                    {
                        QStringLiteral("CREATE TABLE address_books (account_id TEXT NOT NULL "
                                       "REFERENCES accounts(account_id) ON DELETE CASCADE, "
                                       "address_book_id TEXT NOT NULL, name TEXT NOT NULL, "
                                       "description TEXT, sort_order INTEGER NOT NULL, "
                                       "is_default INTEGER NOT NULL, is_subscribed INTEGER NOT "
                                       "NULL, share_with_json TEXT NOT NULL, my_rights_json TEXT "
                                       "NOT NULL, state TEXT NOT NULL, "
                                       "PRIMARY KEY(account_id,address_book_id)) STRICT"),
                        QStringLiteral("CREATE TABLE contact_cards (account_id TEXT NOT NULL "
                                       "REFERENCES accounts(account_id) ON DELETE CASCADE, "
                                       "contact_id TEXT NOT NULL, uid TEXT NOT NULL, kind TEXT "
                                       "NOT NULL, display_name TEXT NOT NULL, organization TEXT, "
                                       "document_json TEXT NOT NULL, PRIMARY KEY(account_id,"
                                       "contact_id), UNIQUE(account_id,uid)) STRICT"),
                        QStringLiteral(
                            "CREATE TABLE contact_card_address_books (account_id TEXT "
                            "NOT NULL, contact_id TEXT NOT NULL, address_book_id TEXT "
                            "NOT NULL, PRIMARY KEY(account_id,contact_id,address_book_id), "
                            "FOREIGN KEY(account_id,contact_id) REFERENCES contact_cards"
                            "(account_id,contact_id) ON DELETE CASCADE, FOREIGN KEY"
                            "(account_id,address_book_id) REFERENCES address_books"
                            "(account_id,address_book_id) ON DELETE CASCADE) STRICT"),
                        QStringLiteral(
                            "CREATE TABLE contact_emails (account_id TEXT NOT NULL, "
                            "contact_id TEXT NOT NULL, entry_key TEXT NOT NULL, address "
                            "TEXT NOT NULL, normalized_address TEXT NOT NULL, label TEXT, "
                            "preference INTEGER, PRIMARY KEY(account_id,contact_id,entry_key), "
                            "FOREIGN KEY(account_id,contact_id) REFERENCES contact_cards"
                            "(account_id,contact_id) ON DELETE CASCADE) STRICT"),
                        QStringLiteral("CREATE INDEX idx_contact_cards_name ON contact_cards"
                                       "(account_id,display_name COLLATE NOCASE,contact_id)"),
                        QStringLiteral("CREATE INDEX idx_contact_emails_address ON contact_emails"
                                       "(normalized_address,account_id,contact_id)"),
                        QStringLiteral("CREATE INDEX idx_contact_books_membership ON "
                                       "contact_card_address_books(account_id,address_book_id,"
                                       "contact_id)"),
                    },
            },
            MigrationStep{
                .version = 13,
                .name = QStringLiteral("websocket_push_capability"),
                .statements =
                    {
                        QStringLiteral("ALTER TABLE sessions ADD COLUMN websocket_url TEXT"),
                        QStringLiteral("ALTER TABLE sessions ADD COLUMN "
                                       "websocket_supports_push INTEGER NOT NULL DEFAULT 0"),
                    },
            },
        };
    }
} // namespace javelin::jmap::cache::migrations
