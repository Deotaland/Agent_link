#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include "realtime_end_log.h"
using agentlink::realtime::EndDiagnostic;
using agentlink::realtime::DescribeServerEnd;
constexpr const char* auth = "fixture-auth-secret-42";
constexpr const char* token = "fixture-connect-secret-77";
EndDiagnostic Parse(const char* payload) {
    cJSON* json = cJSON_Parse(payload); assert(json);
    EndDiagnostic result;
    assert(DescribeServerEnd(json, auth, token, &result));
    cJSON_Delete(json); return result;
}
void NoSecret(const EndDiagnostic& value) {
    const std::string all = std::string(value.event) + value.code + value.reason;
    assert(all.find(auth) == std::string::npos && all.find(token) == std::string::npos);
    assert(all.find('\n') == std::string::npos && all.find('\r') == std::string::npos);
}
int main() {
    auto value = Parse(R"({"type":"session.ended","code":408,"reason":"session timed out"})");
    assert(std::strcmp(value.event, "session.ended") == 0 && std::strcmp(value.code, "408") == 0);
    assert(std::strcmp(value.reason, "session timed out") == 0);
    value = Parse(R"({"type":"error","error":{"code":"session_expired","message":"token expired"},"data":{"connect_token":"must-not-serialize"},"transcript":"private speech"})");
    assert(std::strcmp(value.code, "session_expired") == 0 && std::strcmp(value.reason, "token expired") == 0);
    value = Parse(R"({"type":"session.error","data":{"error":{"code":"SESSION_DURATION_EXCEEDED","message":"maximum session duration exceeded"}}})");
    assert(std::strcmp(value.code, "SESSION_DURATION_EXCEEDED") == 0);
    assert(std::strstr(value.reason, "maximum session duration exceeded"));
    value = Parse(R"({"type":"session.ended","reason":{"code":"idle_timeout"}})");
    assert(std::strcmp(value.reason, "idle_timeout") == 0);
    value = Parse(R"({"type":"error","error":"upstream unavailable"})");
    assert(std::strcmp(value.reason, "upstream unavailable") == 0);
    value = Parse(R"({"type":"session.ended"})");
    assert(std::strcmp(value.code, "unspecified") == 0 && std::strcmp(value.reason, "unspecified") == 0);
    value = Parse(R"({"type":"session.error","code":"fixture-auth-secret-42","reason":"bad fixture-connect-secret-77\nAuthorization: Bearer shortkey token = abc123 session_id=sid123 wss://host/path?token=anothersecret"})");
    NoSecret(value);
    for (const char* secret : {"shortkey", "abc123", "sid123", "host/path", "anothersecret"})
        assert(!std::strstr(value.reason, secret));
    assert(std::strstr(value.reason, "[redacted]"));
    std::string long_reason(300, 'x');
    cJSON* json = cJSON_CreateObject(); cJSON_AddStringToObject(json, "type", "session.ended");
    cJSON_AddStringToObject(json, "reason", long_reason.c_str());
    assert(DescribeServerEnd(json, auth, token, &value));
    assert(std::strcmp(value.reason, "[redacted]") == 0); cJSON_Delete(json);
    char small[9];
    agentlink::realtime::SafeEndField(small, sizeof small, "fixture-connect-secret-77", auth, token);
    assert(std::strncmp(small, "fixture", 7) != 0 && std::strlen(small) < sizeof small);
    for (const char* payload : {R"({"type":"response.started"})", "{}", "null"}) {
        json = cJSON_Parse(payload);
        assert(!DescribeServerEnd(json, auth, token, &value)); cJSON_Delete(json);
    }
    std::puts("PASS: server terminal event/code/reason selection, missing fields, nested errors, bounded credential/URL/identifier redaction and single-line output");
}
