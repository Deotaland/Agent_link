// Reuse the deterministic HTTP fixture and its existing authentication regression suite.
#define main platform_existing_test_main
#include "platform_api_auth_test.cc"
#undef main
#include "agent_link_chat.h"
#include "chat_response.h"
#include "paper_protocol.h"

std::string JsonString(const char* key, const std::string& value) {
    cJSON* o = cJSON_CreateObject(); cJSON_AddStringToObject(o, key, value.c_str());
    char* text = cJSON_PrintUnformatted(o); cJSON_Delete(o);
    std::string result = text; cJSON_free(text); return result;
}
std::string Sse(const std::string& text) {
    cJSON* o = cJSON_CreateObject(); cJSON_AddStringToObject(o, "type", "content");
    cJSON_AddStringToObject(o, "delta", text.c_str());
    char* raw = cJSON_PrintUnformatted(o); cJSON_Delete(o);
    std::string result = std::string("data: ") + raw + "\r\n\r\ndata: {\"type\":\"done\"}\n\ndata: [DONE]\n\n";
    cJSON_free(raw); return result;
}
void ProtocolTests() {
    const std::string reply = R"({"text":"你好","tools":[{"name":"motor0","arguments":{"value":300}}]})";
    const std::string stream = Sse(reply);
    for (size_t split = 1; split < 25; ++split) {
        agentlink::ChatResponse parser(4096);
        for (size_t i = 0; i < stream.size(); i += split)
            CHECK(parser.Feed(stream.data() + i, std::min(split, stream.size() - i)));
        CHECK(parser.Finish() && parser.Text() == reply);
    }
    agentlink::ChatResponse incomplete(4096);
    const std::string partial = "data: {\"type\":\"content\",\"delta\":\"partial\"}\n\n";
    CHECK(incomplete.Feed(partial.data(), partial.size()) && !incomplete.Finish());
    agentlink::ChatResponse overflow(3);
    CHECK(!overflow.Feed(stream.data(), stream.size()));
    agentlink::ChatResponse error(4096);
    const std::string bad = "data: {\"type\":\"error\"}\n\n";
    CHECK(!error.Feed(bad.data(), bad.size()));
    paper::ToolReply plan;
    CHECK(plan.Parse(reply.c_str()));
    CHECK(!plan.Parse((reply + " trailing").c_str()));
    CHECK(!plan.Parse(R"({"text":"x","tools":[{"name":"motor0","arguments":{"value":300}},{"name":"reboot","arguments":{}}]})"));
    CHECK(!plan.Parse(R"({"text":"x","tools":[{"name":"motor0","arguments":{"value":2001}}]})"));
    CHECK(!plan.Parse(R"({"text":"x","tools":[{"name":"motor0","arguments":{"value":1.5}}]})"));
    CHECK(!plan.Parse(R"({"text":"x","tools":[{"name":"alarm0","arguments":{"hour":24,"minute":0,"enabled":true}}]})"));
    CHECK(!plan.Parse(R"({"text":"x","tools":[{"name":"music0","arguments":{"action":"play","file":"../private.wav"}}]})"));
    CHECK(plan.Parse(R"({"text":"x","tools":[{"name":"led0","arguments":{"value":"#00ff00"}},{"name":"alarm0","arguments":{"hour":7,"minute":30,"enabled":true}}]})"));
    std::string many = R"({"text":"x","tools":[)";
    for (int i = 0; i < 9; ++i) { if (i) many += ','; many += R"({"name":"motor0","arguments":{"value":1}})"; }
    many += "]}"; CHECK(!plan.Parse(many.c_str()));
    CHECK(paper::MonthDays(2024, 2) == 29 && paper::MonthDays(2100, 2) == 28 && paper::MonthDays(2000, 2) == 29);
    uint8_t wav[] = {'R','I','F','F',40,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,
        1,0,1,0,0x80,0x3e,0,0,0,0x7d,0,0,2,0,16,0,'d','a','t','a',4,0,0,0,0,0,1,0};
    FILE* file = std::tmpfile(); CHECK(file);
    CHECK(std::fwrite(wav, 1, sizeof wav, file) == sizeof wav);
    paper::Wav format; CHECK(paper::ReadWav(file, format));
    CHECK(format.rate == 16000 && format.channels == 1 && format.bytes == 4); fclose(file);
    wav[20] = 3; file = std::tmpfile(); CHECK(file); std::fwrite(wav, 1, sizeof wav, file);
    CHECK(!paper::ReadWav(file, format)); fclose(file);
    wav[20] = 1; wav[40] = 100; file = std::tmpfile(); CHECK(file); std::fwrite(wav, 1, sizeof wav, file);
    CHECK(!paper::ReadWav(file, format)); fclose(file);
    std::puts("PASS: SSE packet/UTF-8 boundaries, incomplete streams, atomic JSON tool validation, path bounds, calendar and malformed WAVs");
}
int main() {
    CHECK(platform_existing_test_main() == 0);
    ProtocolTests();
    char output[8192], id[80];
    CHECK(agent_link_chat_create(7, id, sizeof id) == ESP_ERR_INVALID_STATE);
    cJSON* auth = cJSON_Parse(R"({"agent":{"id":7,"name":"chat","capabilities":{"llm":{},"realtime_voice":{}}}})");
    cloud_platform_set_bound_agent(auth); cJSON_Delete(auth);
    Expect(HTTP_METHOD_POST, "/api/agents/7/actions/llm/conversations", R"({"code":0,"data":{"conversation_id":"chat-a"}})", "{}");
    CHECK(agent_link_chat_create(7, id, sizeof id) == ESP_OK && std::strcmp(id, "chat-a") == 0);
    Expect(HTTP_METHOD_GET, "/api/agents/7/actions/llm/conversations?limit=10&cursor=a%2B%2F%3D", R"({"data":{"items":[]}})");
    CHECK(agent_link_chat_conversations(7, "a+/=", output, sizeof output) == ESP_OK);
    Expect(HTTP_METHOD_GET, "/api/agents/7/actions/llm/conversations/chat-a/messages?limit=10", R"({"data":{"items":[{"role":"user","content":"hello"}]}})");
    CHECK(agent_link_chat_messages(7, id, nullptr, output, sizeof output) == ESP_OK);
    const std::string reply = R"({"text":"你好","tools":[]})";
    const std::string response = Sse(reply);
    Expect(HTTP_METHOD_POST, "/api/agents/7/actions/llm/execute", response.c_str(),
        R"({"conversation_id":"chat-a","stream":true,"messages":[{"role":"user","content":"hello"}]})");
    CHECK(agent_link_chat_send(7, id, "hello", output, sizeof output) == ESP_OK && reply == output);
    CHECK(agent_link_chat_send(8, id, "hello", output, sizeof output) == ESP_ERR_INVALID_STATE);
    CHECK(agent_link_chat_messages(7, "../bad", nullptr, output, sizeof output) == ESP_ERR_INVALID_ARG);
    Expect(HTTP_METHOD_POST, "/api/agents/7/actions/llm/execute", "data: {\"type\":\"content\",\"delta\":\"partial\"}\n\n");
    CHECK(agent_link_chat_send(7, id, "hello", output, sizeof output) == ESP_ERR_INVALID_RESPONSE && output[0] == 0);
    Expect(HTTP_METHOD_POST, "/api/agents/7/actions/llm/execute", "", "", 403);
    CHECK(agent_link_chat_send(7, id, "hello", output, sizeof output) == ESP_ERR_NOT_SUPPORTED);
    const std::string old = "user instructions\n[AGENT_LINK_DEVICE_BEGIN]\nold manifest\n[AGENT_LINK_DEVICE_END] tail";
    const std::string detail = "{\"data\":{\"id\":7," + JsonString("prompt", old).substr(1);
    // detail needs one additional closing brace for data.
    Expect(HTTP_METHOD_GET, "/api/agents/7", (detail + "}").c_str());
    const std::string updated = "user instructions tail\n[AGENT_LINK_DEVICE_BEGIN]\nnew manifest\n[AGENT_LINK_DEVICE_END]";
    Expect(HTTP_METHOD_PUT, "/api/agents/7", R"({"code":0,"success":true,"data":{}})", JsonString("prompt", updated).c_str());
    CHECK(agent_link_chat_sync_prompt(7, "new manifest") == ESP_OK);
    Expect(HTTP_METHOD_GET, "/api/agents/7", R"({"data":{"id":7,"prompt":"keep\n[AGENT_LINK_DEVICE_BEGIN]\nbroken"}})");
    CHECK(agent_link_chat_sync_prompt(7, "new manifest") == ESP_ERR_INVALID_RESPONSE);
    CHECK(consumed == expected.size());
    std::puts("PASS: device-authenticated chat/history, cursor escaping, partial-stream rejection and preserving user prompts");
    return 0;
}
