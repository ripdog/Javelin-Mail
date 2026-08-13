#pragma once

#include "jmap/api/MethodCaller.h"
#include "jmap/api/RequestBuilder.h"

#include <QCoroTask>

#include <string>

namespace javelin::jmap::api
{
    class JmapMethodTransport;
}

namespace javelin::jmap::contacts
{
    class ContactProtocolClient
    {
      public:
        explicit ContactProtocolClient(api::JmapMethodTransport& methodTransport);

        [[nodiscard]] QCoro::Task<api::MethodCallerResult>
        call(api::ApiRequestContext requestContext, api::RequestBuilder request) const;

      private:
        api::JmapMethodTransport& m_methodTransport;
    };
} // namespace javelin::jmap::contacts
