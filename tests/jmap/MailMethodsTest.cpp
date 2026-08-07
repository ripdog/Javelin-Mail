#include "jmap/api/MailMethods.h"
#include "FixtureReader.h"
#include "jmap/api/PatchObject.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("get requests serialize typed account, ids, and properties", "[jmap][method][mail]")
{
    const auto json = javelin::jmap::api::serializeGetRequest({
        .accountId = "u1",
        .ids = std::vector<std::string>{"mbx-inbox"},
        .idsReference = std::nullopt,
        .properties = std::vector<std::string>{"id", "name"},
    });

    REQUIRE(json.has_value());
    CHECK(*json == R"({"accountId":"u1","ids":["mbx-inbox"],"properties":["id","name"]})");
}

TEST_CASE("get requests serialize result references for chained ids", "[jmap][method][mail]")
{
    const auto json = javelin::jmap::api::serializeGetRequest({
        .accountId = "u1",
        .ids = std::nullopt,
        .idsReference =
            javelin::jmap::api::GetRequest::ResultReference{
                .resultOf = "mailbox-query",
                .name = "Email/query",
                .path = "/ids",
            },
        .properties = std::vector<std::string>{"threadId"},
    });

    REQUIRE(json.has_value());
    CHECK(
        *json ==
        R"({"accountId":"u1","#ids":{"resultOf":"mailbox-query","name":"Email/query","path":"/ids"},"properties":["threadId"]})");
}

TEST_CASE("changes requests serialize typed state-token inputs", "[jmap][method][mail]")
{
    const auto json = javelin::jmap::api::serializeChangesRequest({
        .accountId = "u1",
        .sinceState = "state-1",
        .maxChanges = 100,
    });

    REQUIRE(json.has_value());
    CHECK(*json == R"({"accountId":"u1","sinceState":"state-1","maxChanges":100})");
}

TEST_CASE("identity set creates serialize server-backed signature variants",
          "[jmap][method][mail][identity]")
{
    const auto json = javelin::jmap::api::serializeIdentitySetRequest({
        .accountId = "u1",
        .ifInState = "identity-state-1",
        .create =
            {
                {"signature-work",
                 javelin::jmap::api::IdentitySetCreate{
                     .name = "Johnson Clark",
                     .email = "johnson@example.com",
                     .replyTo = {},
                     .bcc = {{{.name = std::nullopt, .email = "johnson+archive@example.com"}}},
                     .textSignature = "Regards,\nJohnson",
                     .htmlSignature = "<p>Regards,<br>Johnson</p>",
                 }},
            },
        .update = {},
        .destroy = {},
    });

    REQUIRE(json.has_value());
    CHECK(
        *json ==
        R"({"accountId":"u1","ifInState":"identity-state-1","create":{"signature-work":{"name":"Johnson Clark","email":"johnson@example.com","replyTo":[],"bcc":[{"email":"johnson+archive@example.com"}],"textSignature":"Regards,\nJohnson","htmlSignature":"<p>Regards,<br>Johnson</p>"}}})");
}

TEST_CASE("identity set updates never serialize immutable email", "[jmap][method][mail][identity]")
{
    const auto json = javelin::jmap::api::serializeIdentitySetRequest({
        .accountId = "u1",
        .ifInState = std::nullopt,
        .create = {},
        .update =
            {
                {"identity-1",
                 javelin::jmap::api::IdentitySetUpdate{
                     .name = "Johnson",
                     .replyTo = std::vector<javelin::jmap::domain::EmailAddress>{},
                     .bcc = std::vector<javelin::jmap::domain::EmailAddress>{},
                     .textSignature = "-- \nJohnson",
                     .htmlSignature = "<p>Johnson</p>",
                 }},
            },
        .destroy = {"identity-old"},
    });

    REQUIRE(json.has_value());
    CHECK(json->find(R"("identity-1":{"name":"Johnson")") != std::string::npos);
    CHECK(json->find(R"("textSignature":"-- \nJohnson")") != std::string::npos);
    CHECK(json->find(R"("destroy":["identity-old"])") != std::string::npos);
    CHECK(json->find(R"("email")") == std::string::npos);
}

TEST_CASE("identity changes responses preserve incremental ids", "[jmap][method][mail][identity]")
{
    const auto result = javelin::jmap::api::parseIdentityChangesResponse(
        R"({"accountId":"u1","oldState":"i1","newState":"i2","hasMoreChanges":false,"created":["identity-new"],"updated":["identity-updated"],"destroyed":["identity-old"]})");

    REQUIRE(result.ok());
    REQUIRE(result.value.has_value());
    CHECK(result.value->oldState == "i1");
    CHECK(result.value->newState == "i2");
    CHECK(result.value->created == std::vector<std::string>{"identity-new"});
    CHECK(result.value->updated == std::vector<std::string>{"identity-updated"});
    CHECK(result.value->destroyed == std::vector<std::string>{"identity-old"});
}

TEST_CASE("identity set responses retain structured per-object errors",
          "[jmap][method][mail][identity]")
{
    const auto result = javelin::jmap::api::parseIdentitySetResponse(
        R"({"accountId":"u1","oldState":"i1","newState":"i2","created":{"signature-work":{"id":"identity-2"}},"updated":{"identity-1":null},"destroyed":["identity-old"],"notCreated":{"blocked":{"type":"forbiddenFrom","description":"Address is not permitted","properties":["email"]}},"notUpdated":{"missing":{"type":"notFound"}},"notDestroyed":{}})");

    REQUIRE(result.ok());
    REQUIRE(result.value.has_value());
    CHECK(result.value->created.at("signature-work") == "identity-2");
    CHECK(result.value->updated == std::vector<std::string>{"identity-1"});
    CHECK(result.value->destroyed == std::vector<std::string>{"identity-old"});
    REQUIRE(result.value->notCreated.contains("blocked"));
    CHECK(result.value->notCreated.at("blocked").type == "forbiddenFrom");
    CHECK(result.value->notCreated.at("blocked").description ==
          std::optional<std::string>{"Address is not permitted"});
    CHECK(result.value->notCreated.at("blocked").properties == std::vector<std::string>{"email"});
    CHECK(result.value->notUpdated.at("missing").type == "notFound");
}

TEST_CASE("identity set responses accept Fastmail null states with server-created properties",
          "[jmap][method][mail][identity][fastmail]")
{
    const auto result = javelin::jmap::api::parseIdentitySetResponse(
        R"({"created":{"85ff3150-9095-4095-8f2b-cfb367d435d1":{"displayName":"","enableExternalSMTP":false,"externalCredentialId":null,"server":"","isAutoConfigured":false,"bcc":null,"showInCompose":true,"warnings":[],"port":587,"verificationState":"autoverified","mayDelete":true,"verificationCheckTime":"2026-08-07T19:14:20Z","ssl":"starttls","id":"184241207","saveOnSMTP":false,"saveSentToMailboxId":"P2F","replyTo":null,"addBccOnSMTP":false,"useForAutoReply":true}},"updated":{},"newState":null,"oldState":null,"destroyed":[],"accountId":"u74ee43a3"})");

    REQUIRE(result.ok());
    REQUIRE(result.value.has_value());
    CHECK(result.value->accountId == "u74ee43a3");
    CHECK(result.value->oldState.empty());
    CHECK_FALSE(result.value->newState.has_value());
    CHECK(result.value->created.at("85ff3150-9095-4095-8f2b-cfb367d435d1") == "184241207");
    CHECK(result.value->updated.empty());
    CHECK(result.value->destroyed.empty());
}

TEST_CASE("email query requests serialize mailbox-scoped sort windows", "[jmap][method][mail]")
{
    const auto json = javelin::jmap::api::serializeEmailQueryRequest({
        .accountId = "u1",
        .filter =
            javelin::jmap::api::EmailQueryFilter{
                .inMailbox = "mbx-inbox",
                .text = std::nullopt,
            },
        .sort =
            {
                javelin::jmap::api::EmailQuerySort{
                    .property = "receivedAt",
                    .isAscending = false,
                },
            },
        .position = 0,
        .anchor = std::nullopt,
        .anchorOffset = 0,
        .limit = 100,
        .collapseThreads = false,
        .calculateTotal = false,
    });

    REQUIRE(json.has_value());
    CHECK(
        *json ==
        R"({"accountId":"u1","filter":{"inMailbox":"mbx-inbox"},"sort":[{"property":"receivedAt","isAscending":false}],"position":0,"limit":100,"collapseThreads":false,"calculateTotal":false})");
}

TEST_CASE("email query requests serialize text search filters", "[jmap][method][mail]")
{
    const auto json = javelin::jmap::api::serializeEmailQueryRequest({
        .accountId = "u1",
        .filter =
            javelin::jmap::api::EmailQueryFilter{
                .inMailbox = std::nullopt,
                .text = "quarterly report",
            },
        .sort =
            {
                javelin::jmap::api::EmailQuerySort{
                    .property = "receivedAt",
                    .isAscending = false,
                },
            },
        .position = 0,
        .anchor = std::nullopt,
        .anchorOffset = 0,
        .limit = 25,
        .collapseThreads = true,
        .calculateTotal = true,
    });

    REQUIRE(json.has_value());
    CHECK(
        *json ==
        R"({"accountId":"u1","filter":{"text":"quarterly report"},"sort":[{"property":"receivedAt","isAscending":false}],"position":0,"limit":25,"collapseThreads":true,"calculateTotal":true})");
}

TEST_CASE("email query requests serialize anchored windows", "[jmap][method][mail]")
{
    const auto json = javelin::jmap::api::serializeEmailQueryRequest({
        .accountId = "u1",
        .filter = javelin::jmap::api::EmailQueryFilter{.inMailbox = "mbx-inbox"},
        .sort = {javelin::jmap::api::EmailQuerySort{.property = "receivedAt"}},
        .position = std::nullopt,
        .anchor = "email-50",
        .anchorOffset = 1,
        .limit = 50,
        .collapseThreads = true,
        .calculateTotal = true,
    });

    REQUIRE(json.has_value());
    CHECK(
        *json ==
        R"({"accountId":"u1","filter":{"inMailbox":"mbx-inbox"},"sort":[{"property":"receivedAt","isAscending":false}],"anchor":"email-50","anchorOffset":1,"limit":50,"collapseThreads":true,"calculateTotal":true})");
}

TEST_CASE("email query requests serialize nested address search filters", "[jmap][method][mail]")
{
    const auto json = javelin::jmap::api::serializeEmailQueryRequest({
        .accountId = "u1",
        .filter =
            javelin::jmap::api::EmailQueryFilter{
                .operatorName = "OR",
                .conditions =
                    {
                        javelin::jmap::api::EmailQueryFilter{.from = "alice@example.com"},
                        javelin::jmap::api::EmailQueryFilter{.to = "alice@example.com"},
                        javelin::jmap::api::EmailQueryFilter{.cc = "alice@example.com"},
                        javelin::jmap::api::EmailQueryFilter{.bcc = "alice@example.com"},
                    },
            },
        .sort =
            {
                javelin::jmap::api::EmailQuerySort{
                    .property = "receivedAt",
                    .isAscending = false,
                },
            },
        .position = 0,
        .anchor = std::nullopt,
        .anchorOffset = 0,
        .limit = 25,
        .collapseThreads = true,
        .calculateTotal = true,
    });

    REQUIRE(json.has_value());
    CHECK(
        *json ==
        R"({"accountId":"u1","filter":{"operator":"OR","conditions":[{"from":"alice@example.com"},{"to":"alice@example.com"},{"cc":"alice@example.com"},{"bcc":"alice@example.com"}]},"sort":[{"property":"receivedAt","isAscending":false}],"position":0,"limit":25,"collapseThreads":true,"calculateTotal":true})");
}

TEST_CASE("email queryChanges requests serialize incremental mailbox windows",
          "[jmap][method][mail]")
{
    const auto json = javelin::jmap::api::serializeEmailQueryChangesRequest({
        .accountId = "u1",
        .sinceQueryState = "query-state-1",
        .maxChanges = 50,
        .upToId = std::nullopt,
        .filter =
            javelin::jmap::api::EmailQueryFilter{
                .inMailbox = "mbx-inbox",
                .text = std::nullopt,
            },
        .sort =
            {
                javelin::jmap::api::EmailQuerySort{
                    .property = "receivedAt",
                    .isAscending = false,
                },
            },
        .collapseThreads = true,
        .calculateTotal = true,
    });

    REQUIRE(json.has_value());
    CHECK(
        *json ==
        R"({"accountId":"u1","sinceQueryState":"query-state-1","maxChanges":50,"filter":{"inMailbox":"mbx-inbox"},"sort":[{"property":"receivedAt","isAscending":false}],"collapseThreads":true,"calculateTotal":true})");
}

TEST_CASE("mailbox get responses parse into typed mailbox entities", "[jmap][method][mail]")
{
    const auto result = javelin::jmap::api::parseMailboxGetResponse(
        javelin::tests::loadFixture("jmap/method/mailbox_get_arguments.json"));

    REQUIRE(result.ok());
    REQUIRE(result.value.has_value());
    CHECK(result.value->accountId == "u1");
    CHECK(result.value->state == "mailbox-state-1");
    REQUIRE(result.value->list.size() == 1);
    CHECK(result.value->list.front().id == "mbx-inbox");
    CHECK(result.value->list.front().role == std::optional<std::string>{"inbox"});
}

TEST_CASE("email get responses parse into typed email entities", "[jmap][method][mail]")
{
    const auto result = javelin::jmap::api::parseEmailGetResponse(
        javelin::tests::loadFixture("jmap/method/email_get_arguments.json"));

    REQUIRE(result.ok());
    REQUIRE(result.value.has_value());
    CHECK(result.value->accountId == "u1");
    CHECK(result.value->state == "email-state-1");
    REQUIRE(result.value->list.size() == 1);
    CHECK(result.value->list.front().id == "eml-1");
    CHECK(result.value->list.front().threadId == "thr-123");
    CHECK(result.value->list.front().subject == std::optional<std::string>{"Quarterly update"});
}

TEST_CASE("thread get responses parse into typed thread entities", "[jmap][method][mail]")
{
    const auto result = javelin::jmap::api::parseThreadGetResponse(
        R"({"accountId":"u1","state":"thread-state-1","list":[{"id":"thr-1","emailIds":["eml-1","eml-2"]}],"notFound":[]})");

    REQUIRE(result.ok());
    REQUIRE(result.value.has_value());
    CHECK(result.value->accountId == "u1");
    CHECK(result.value->state == "thread-state-1");
    REQUIRE(result.value->list.size() == 1);
    CHECK(result.value->list.front().id == "thr-1");
    CHECK(result.value->list.front().emailIds == std::vector<std::string>{"eml-1", "eml-2"});
}

TEST_CASE("changes responses parse created updated and destroyed ids", "[jmap][method][mail]")
{
    const auto result = javelin::jmap::api::parseChangesResponse(
        javelin::tests::loadFixture("jmap/method/changes_arguments.json"));

    REQUIRE(result.ok());
    REQUIRE(result.value.has_value());
    CHECK(result.value->accountId == "u1");
    CHECK(result.value->oldState == "state-1");
    CHECK(result.value->newState == "state-2");
    CHECK(result.value->hasMoreChanges);
    CHECK(result.value->created == std::vector<std::string>{"mbx-created"});
    CHECK(result.value->updated == std::vector<std::string>{"mbx-updated"});
    CHECK(result.value->destroyed == std::vector<std::string>{"mbx-destroyed"});
}

TEST_CASE("email changes responses parse typed incremental ids", "[jmap][method][mail]")
{
    const auto result = javelin::jmap::api::parseEmailChangesResponse(
        R"({"accountId":"u1","oldState":"state-1","newState":"state-2","hasMoreChanges":false,"created":["eml-created"],"updated":["eml-updated"],"destroyed":["eml-destroyed"]})");

    REQUIRE(result.ok());
    REQUIRE(result.value.has_value());
    CHECK(result.value->accountId == "u1");
    CHECK(result.value->oldState == "state-1");
    CHECK(result.value->newState == "state-2");
    CHECK_FALSE(result.value->hasMoreChanges);
    CHECK(result.value->created == std::vector<std::string>{"eml-created"});
    CHECK(result.value->updated == std::vector<std::string>{"eml-updated"});
    CHECK(result.value->destroyed == std::vector<std::string>{"eml-destroyed"});
}

TEST_CASE("email query responses parse ids and query metadata", "[jmap][method][mail]")
{
    const auto result = javelin::jmap::api::parseEmailQueryResponse(
        R"({"accountId":"u1","queryState":"query-state-1","canCalculateChanges":true,"position":0,"ids":["eml-1","eml-2"],"total":2,"limit":50})");

    REQUIRE(result.ok());
    REQUIRE(result.value.has_value());
    CHECK(result.value->accountId == "u1");
    CHECK(result.value->queryState == "query-state-1");
    CHECK(result.value->canCalculateChanges);
    CHECK(result.value->ids == std::vector<std::string>{"eml-1", "eml-2"});
    CHECK(result.value->total == std::optional<std::uint64_t>{2});
    CHECK(result.value->limit == std::optional<std::uint64_t>{50});
}

TEST_CASE("email query responses ignore unknown server fields", "[jmap][method][mail]")
{
    const auto result = javelin::jmap::api::parseEmailQueryResponse(
        R"({"accountId":"u1","queryState":"query-state-1","canCalculateChanges":true,"position":0,"ids":["eml-1","eml-2"],"total":2,"collapseThreads":false,"isQueryComplete":true})");

    REQUIRE(result.ok());
    REQUIRE(result.value.has_value());
    CHECK(result.value->accountId == "u1");
    CHECK(result.value->queryState == "query-state-1");
    CHECK(result.value->ids == std::vector<std::string>{"eml-1", "eml-2"});
    CHECK(result.value->total == std::optional<std::uint64_t>{2});
}

TEST_CASE("email queryChanges responses parse added and removed ids", "[jmap][method][mail]")
{
    const auto result = javelin::jmap::api::parseEmailQueryChangesResponse(
        R"({"accountId":"u1","oldQueryState":"query-state-1","newQueryState":"query-state-2","added":[{"id":"eml-3","index":0}],"removed":["eml-1"],"hasMoreChanges":false,"total":2})");

    REQUIRE(result.ok());
    REQUIRE(result.value.has_value());
    CHECK(result.value->accountId == "u1");
    CHECK(result.value->oldQueryState == "query-state-1");
    CHECK(result.value->newQueryState == "query-state-2");
    REQUIRE(result.value->added.size() == 1);
    CHECK(result.value->added.front().id == "eml-3");
    CHECK(result.value->added.front().index == 0);
    CHECK(result.value->removed == std::vector<std::string>{"eml-1"});
    CHECK_FALSE(result.value->hasMoreChanges);
    CHECK(result.value->total == std::optional<std::uint64_t>{2});
}

TEST_CASE("email content get requests serialize body fetch arguments", "[jmap][method][mail]")
{
    const auto json = javelin::jmap::api::serializeEmailContentGetRequest({
        .accountId = "u1",
        .ids = {"eml-1"},
        .properties = {"id", "textBody", "htmlBody", "attachments", "bodyValues"},
        .bodyProperties = {"partId", "blobId", "size", "name", "type", "charset", "disposition",
                           "cid"},
        .fetchTextBodyValues = true,
        .fetchHTMLBodyValues = true,
        .fetchAllBodyValues = false,
        .maxBodyValueBytes = 131072,
    });

    REQUIRE(json.has_value());
    CHECK(
        *json ==
        R"({"accountId":"u1","ids":["eml-1"],"properties":["id","textBody","htmlBody","attachments","bodyValues"],"bodyProperties":["partId","blobId","size","name","type","charset","disposition","cid"],"fetchTextBodyValues":true,"fetchHTMLBodyValues":true,"fetchAllBodyValues":false,"maxBodyValueBytes":131072})");
}

TEST_CASE("email content get responses parse typed body sections and values",
          "[jmap][method][mail]")
{
    const auto result = javelin::jmap::api::parseEmailContentGetResponse(
        R"({"accountId":"u1","state":"email-state-1","list":[{"id":"eml-1","textBody":[{"partId":"1","blobId":"blob-text","size":24,"type":"text/plain","charset":"utf-8"}],"htmlBody":[{"partId":"2","blobId":"blob-html","size":48,"type":"text/html","charset":"utf-8","cid":"inline-1"}],"attachments":[{"partId":"3","blobId":"blob-pdf","size":5120,"name":"report.pdf","type":"application/pdf","disposition":"attachment"}],"bodyValues":{"1":{"isEncodingProblem":false,"isTruncated":false,"value":"Plain text body"},"2":{"isEncodingProblem":false,"isTruncated":true,"value":"<p>HTML body</p>"}}}],"notFound":[]})");

    REQUIRE(result.ok());
    REQUIRE(result.value.has_value());
    CHECK(result.value->accountId == "u1");
    CHECK(result.value->state == "email-state-1");
    REQUIRE(result.value->list.size() == 1);
    const auto& content = result.value->list.front();
    CHECK(content.id == "eml-1");
    REQUIRE(content.textBody.size() == 1);
    CHECK(content.textBody.front().partId == std::optional<std::string>{"1"});
    CHECK(content.textBody.front().type == "text/plain");
    REQUIRE(content.htmlBody.size() == 1);
    CHECK(content.htmlBody.front().cid == std::optional<std::string>{"inline-1"});
    REQUIRE(content.attachments.size() == 1);
    CHECK(content.attachments.front().name == std::optional<std::string>{"report.pdf"});
    REQUIRE(content.bodyValues.contains("1"));
    CHECK(content.bodyValues.at("1").value == "Plain text body");
    REQUIRE(content.bodyValues.contains("2"));
    CHECK(content.bodyValues.at("2").isTruncated);
}

TEST_CASE("email set requests serialize typed mailbox and keyword updates", "[jmap][method][mail]")
{
    const auto json = javelin::jmap::api::serializeEmailSetRequest({
        .accountId = "u1",
        .ifInState = "state-before-history",
        .create = {},
        .update =
            {
                {"eml-1",
                 javelin::jmap::api::EmailSetUpdate{
                     .patch = {{"mailboxIds/mbx-archive", true}, {"keywords/$seen", true}},
                 }},
            },
        .destroy = {},
    });

    REQUIRE(json.has_value());
    CHECK(json->find(R"("ifInState":"state-before-history")") != std::string::npos);
    CHECK(json->find(R"("mailboxIds/mbx-archive":true)") != std::string::npos);
    CHECK(json->find(R"("keywords/$seen":true)") != std::string::npos);
    CHECK(json->find(R"("mailboxIds":{)") == std::string::npos);
    CHECK(json->find(R"("keywords":{)") == std::string::npos);
}

TEST_CASE("email set requests serialize nullable mailbox and keyword patch removals",
          "[jmap][method][mail]")
{
    const auto json = javelin::jmap::api::serializeEmailSetRequest({
        .accountId = "u1",
        .ifInState = std::nullopt,
        .create = {},
        .update =
            {
                {"eml-1",
                 javelin::jmap::api::EmailSetUpdate{
                     .patch =
                         {
                             {"mailboxIds/mbx-drafts", nullptr},
                             {"mailboxIds/mbx-sent", true},
                             {"keywords/$draft", nullptr},
                             {"keywords/$seen", true},
                         },
                 }},
            },
        .destroy = {},
    });

    REQUIRE(json.has_value());
    CHECK(json->find(R"("mailboxIds/mbx-drafts":null)") != std::string::npos);
    CHECK(json->find(R"("mailboxIds/mbx-sent":true)") != std::string::npos);
    CHECK(json->find(R"("keywords/$draft":null)") != std::string::npos);
    CHECK(json->find(R"("keywords/$seen":true)") != std::string::npos);
}

TEST_CASE("patch paths escape JSON Pointer map keys", "[jmap][method][patch]")
{
    CHECK(javelin::jmap::api::patchPath("mailboxIds", "team/ops~urgent") ==
          "mailboxIds/team~1ops~0urgent");
}

TEST_CASE("PatchObject projection applies escaped paths and null removals", "[jmap][method][patch]")
{
    const auto result = javelin::jmap::api::applyPatchObject(
        R"({"name":"old","nested":{"a":1,"x":true},"items":[1,2],"team/ops":{"~key":"old"}})",
        R"({"name":"new","nested/a":2,"nested/x":null,"team~1ops/~0key":"new"})");

    REQUIRE(std::holds_alternative<std::string>(result));
    CHECK(std::get<std::string>(result) ==
          R"({"name":"new","nested":{"a":2},"items":[1,2],"team/ops":{"~key":"new"}})");
}

TEST_CASE("PatchObject diff emits nested changes and explicit removals", "[jmap][method][patch]")
{
    const auto patch = javelin::jmap::api::makePatchObject(
        R"({"name":{"full":"After"},"organizations":{"javelin-1":{"name":"Example"}},"titles":{"javelin-1":{"name":"Engineer","kind":"title"}}})",
        R"({"name":{"full":"Before"}})");
    REQUIRE(std::holds_alternative<std::string>(patch));
    const auto& json = std::get<std::string>(patch);
    CHECK(json.find(R"("name/full":"Before")") != std::string::npos);
    CHECK(json.find(R"("organizations":null)") != std::string::npos);
    CHECK(json.find(R"("titles":null)") != std::string::npos);

    const auto restored = javelin::jmap::api::applyPatchObject(
        R"({"name":{"full":"After"},"organizations":{"javelin-1":{"name":"Example"}},"titles":{"javelin-1":{"name":"Engineer","kind":"title"}}})",
        json);
    REQUIRE(std::holds_alternative<std::string>(restored));
    CHECK(std::get<std::string>(restored).find("Before") != std::string::npos);
    CHECK(std::get<std::string>(restored).find("organizations") == std::string::npos);
    CHECK(std::get<std::string>(restored).find("titles") == std::string::npos);
}

TEST_CASE("PatchObject projection rejects paths forbidden by RFC 8620", "[jmap][method][patch]")
{
    const auto missing =
        javelin::jmap::api::applyPatchObject(R"({"nested":{}})", R"({"missing/value":1})");
    REQUIRE(std::holds_alternative<javelin::jmap::api::PatchObjectError>(missing));
    CHECK(std::get<javelin::jmap::api::PatchObjectError>(missing).code ==
          javelin::jmap::api::PatchObjectErrorCode::MissingParent);

    const auto array =
        javelin::jmap::api::applyPatchObject(R"({"items":[1,2]})", R"({"items/0":3})");
    REQUIRE(std::holds_alternative<javelin::jmap::api::PatchObjectError>(array));
    CHECK(std::get<javelin::jmap::api::PatchObjectError>(array).code ==
          javelin::jmap::api::PatchObjectErrorCode::ArrayTraversal);

    const auto conflict = javelin::jmap::api::applyPatchObject(R"({"nested":{"a":1}})",
                                                               R"({"nested":{},"nested/a":2})");
    REQUIRE(std::holds_alternative<javelin::jmap::api::PatchObjectError>(conflict));
    CHECK(std::get<javelin::jmap::api::PatchObjectError>(conflict).code ==
          javelin::jmap::api::PatchObjectErrorCode::ConflictingPointers);

    const auto invalid =
        javelin::jmap::api::applyPatchObject(R"({"nested":{}})", R"({"nested/~2":1})");
    REQUIRE(std::holds_alternative<javelin::jmap::api::PatchObjectError>(invalid));
    CHECK(std::get<javelin::jmap::api::PatchObjectError>(invalid).code ==
          javelin::jmap::api::PatchObjectErrorCode::InvalidPointer);
}

TEST_CASE("email set responses parse updated and failed ids", "[jmap][method][mail]")
{
    const auto result = javelin::jmap::api::parseEmailSetResponse(
        R"({"accountId":"u1","oldState":"old-1","newState":"new-1","updated":{"eml-1":null},"notUpdated":{"eml-2":{"type":"forbidden"}}})");

    REQUIRE(result.ok());
    REQUIRE(result.value.has_value());
    CHECK(result.value->accountId == "u1");
    CHECK(result.value->oldState == "old-1");
    CHECK(result.value->newState == "new-1");
    CHECK(result.value->updated == std::vector<std::string>{"eml-1"});
    CHECK(result.value->notUpdated == std::vector<std::string>{"eml-2"});
}

TEST_CASE("email set responses accept nullable oldState and null updated maps",
          "[jmap][method][mail]")
{
    const auto result = javelin::jmap::api::parseEmailSetResponse(
        R"({"accountId":"u1","oldState":null,"newState":"new-1","updated":null,"notUpdated":null})");

    REQUIRE(result.ok());
    REQUIRE(result.value.has_value());
    CHECK(result.value->accountId == "u1");
    CHECK(result.value->oldState.empty());
    CHECK(result.value->newState == "new-1");
    CHECK(result.value->updated.empty());
    CHECK(result.value->notUpdated.empty());
}
