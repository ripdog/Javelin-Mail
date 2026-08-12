# Transitional storage module ownership. Sources remain in their current
# locations until the corresponding storage boundaries are extracted.
add_library(javelin_cache_read STATIC
    ${PROJECT_SOURCE_DIR}/src/app/CacheAccessBarrier.cpp
    ${PROJECT_SOURCE_DIR}/src/app/CacheAccessBarrier.h
    ${PROJECT_SOURCE_DIR}/src/app/CacheLocationProvider.cpp
    ${PROJECT_SOURCE_DIR}/src/app/CacheLocationProvider.h
    ${PROJECT_SOURCE_DIR}/src/jmap/cache/Database.cpp
    ${PROJECT_SOURCE_DIR}/src/jmap/cache/Database.h
    ${PROJECT_SOURCE_DIR}/src/jmap/cache/EmailReadRepository.cpp
    ${PROJECT_SOURCE_DIR}/src/jmap/cache/EmailReadRepository.h
    ${PROJECT_SOURCE_DIR}/src/jmap/cache/MessageContentTypes.h
    ${PROJECT_SOURCE_DIR}/src/jmap/cache/MessageViewService.cpp
    ${PROJECT_SOURCE_DIR}/src/jmap/cache/MessageViewService.h
    ${PROJECT_SOURCE_DIR}/src/jmap/cache/MessageViewReader.h
    ${PROJECT_SOURCE_DIR}/src/jmap/cache/MimeMessageParser.cpp
    ${PROJECT_SOURCE_DIR}/src/jmap/cache/MimeMessageParser.h
    ${PROJECT_SOURCE_DIR}/src/jmap/cache/MailVault.cpp
    ${PROJECT_SOURCE_DIR}/src/jmap/cache/MailVault.h
    ${PROJECT_SOURCE_DIR}/src/jmap/cache/RawMessageSourceReadRepository.cpp
    ${PROJECT_SOURCE_DIR}/src/jmap/cache/RawMessageSourceReadRepository.h
    ${PROJECT_SOURCE_DIR}/src/jmap/render/HtmlMessageDocumentBuilder.cpp
    ${PROJECT_SOURCE_DIR}/src/jmap/render/HtmlMessageDocumentBuilder.h
    ${PROJECT_SOURCE_DIR}/src/jmap/render/HtmlTextExtractor.cpp
    ${PROJECT_SOURCE_DIR}/src/jmap/render/HtmlTextExtractor.h
    ${PROJECT_SOURCE_DIR}/src/jmap/render/InlineMessageUrl.cpp
    ${PROJECT_SOURCE_DIR}/src/jmap/render/InlineMessageUrl.h
)
target_include_directories(javelin_cache_read PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
)
target_link_libraries(javelin_cache_read
    PUBLIC
        KPim6::Mime
        Qt::Concurrent
        Qt::Core
        Qt::Sql
    PRIVATE
        KF6::Codecs
)
javelin_set_project_warnings(javelin_cache_read)
javelin_enable_static_analysis(javelin_cache_read)
javelin_enable_sanitizers(javelin_cache_read)
