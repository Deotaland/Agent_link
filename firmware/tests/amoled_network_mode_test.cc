// The runner inserts production role selection, WiFi startup and status functions.
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
using esp_err_t=int;
constexpr int ESP_OK=0,ESP_FAIL=-1,ESP_ERR_INVALID_STATE=1;
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define taskENTER_CRITICAL(...) ((void)0)
#define taskEXIT_CRITICAL(...) ((void)0)
#define CLOUD_BASE_URL "https://example.test"
#define CLOUD_PRODUCT_ID 10
#define CLOUD_CHIP_TYPE "esp32s3"
struct agent_platform_t {const char* base_url;int product_id;const char* chip_type;};
class Application {public:virtual const agent_platform_t* Platform() const=0;};
class SystemRole : public Application {public:
// INSERT_SYSTEM_PLATFORM
};
class VoiceRole : public Application {public:
// INSERT_VOICE_PLATFORM
};
enum agent_link_phase_t {AGENT_LINK_PHASE_IDLE,AGENT_LINK_PHASE_SETUP,AGENT_LINK_PHASE_CONNECTING,
    AGENT_LINK_PHASE_PAIRING,AGENT_LINK_PHASE_BLOCKED,AGENT_LINK_PHASE_CONNECTED,AGENT_LINK_PHASE_READY};
constexpr int AGENT_TRANSPORT_WIFI=1;
struct agent_link_status_t {agent_link_phase_t phase;int transport,detail;char title[64],hint[128],code[8];uint32_t expires_s;};
enum {AGENT_CLOUD_IDLE,AGENT_CLOUD_NO_NETWORK,AGENT_CLOUD_CLAIMING,AGENT_CLOUD_BOUND,
    AGENT_CLOUD_ONLINE,AGENT_CLOUD_FAULT,AGENT_CLOUD_ALREADY_BOUND,AGENT_CLOUD_NO_AGENT};
struct agent_cloud_status_t {int state=AGENT_CLOUD_IDLE,api_code=0;char code[8]{};uint32_t expires_s=0;};
enum class Phase {kIdle,kProvisioning,kStaConnecting,kStaConnected};
enum class JoinFailure {kNone};
Phase s_phase=Phase::kIdle;
std::atomic<bool> s_local_radio{false};
unsigned s_join_left=0;
JoinFailure s_join_failure=JoinFailure::kNone;
bool s_waiting_ip=false,s_cloud_started=false,portal=false;
std::atomic<unsigned> s_join_attempt{0};
int s_status_lock=0,s_tick_timer=1;
char s_voice_hint[128]{};
char s_ap_ssid[33]="Amoled175-TEST";
const agent_platform_t* s_platform=nullptr;
struct WifiConfig {const char* ssid;const char* password;};
const WifiConfig* s_cfg=nullptr;
std::mutex s_wifi_mtx;
void* s_on_conn=nullptr;
void* s_on_stream=nullptr;
int clock_starts=0,cloud_starts=0,realtime_starts=0,sta_starts=0,portal_starts=0,init_error=0;
bool stored_wifi=false;
agent_link_status_t last_status{};
const char* JoinMessage(JoinFailure){return "Joining";}
bool al_wifi_prov_active(){return portal;}
bool cloud_realtime_ready(){return false;}
// INSERT_COMPOSE
void ReportStatus(){agent_cloud_status_t idle{};Compose(idle,&last_status);}
bool NetReady(){return s_phase==Phase::kStaConnected && !portal;}
void OnCloudState(){}
void OnDownlink(){}
void DeliverToCore(){}
void cloud_mqtt_publish_up(){}
void OnRealtimeGrant(){}
void OnRealtimeState(){}
void OnRealtimePcm(){}
void OnRealtimeText(){}
void OnRealtimeEnd(){}
void OnRealtimeReset(){}
void OnRealtimeFinished(){}
struct cloud_json_hooks_t {void(*a)();void(*b)();void(*c)();};
void cloud_json_set_hooks(const cloud_json_hooks_t*){}
struct cloud_realtime_callbacks_t {void(*a)();void(*b)();void(*c)();void(*d)();void(*e)();void(*f)();};
void cloud_realtime_set_callbacks(const cloud_realtime_callbacks_t*){}
int cloud_realtime_start(){++realtime_starts;return ESP_OK;}
void cloud_realtime_stop(){}
void cloud_platform_set_base_url(const char*){}
struct cloud_session_config_t {
    const char* base_url;int product_id;const char* chip_type;
    bool(*net_ready)();void(*on_state)();void(*on_downlink)();
};
int cloud_session_start(const cloud_session_config_t* cfg){
    assert(clock_starts==1 && cfg->product_id==10 && cfg->net_ready==NetReady);
    ++cloud_starts;return ESP_OK;
}
// INSERT_CLOUD_START
int WifiInitOnce(){return init_error;}
bool LoadCreds(char* ssid,size_t capacity,char*,size_t){
    if(stored_wifi)snprintf(ssid,capacity,"saved-ap");
    return stored_wifi;
}
void StartSta(const char*,const char*){++sta_starts;s_phase=Phase::kStaConnecting;}
void StartProvisioning(){++portal_starts;s_phase=Phase::kProvisioning;portal=true;}
int esp_timer_start_periodic(int,uint64_t){return ESP_OK;}
void cloud_clock_start(){++clock_starts;}
// INSERT_WIFI_START
void Boot(const agent_platform_t* platform){
    s_platform=platform;s_cloud_started=false;s_phase=Phase::kIdle;portal=false;
    clock_starts=cloud_starts=realtime_starts=sta_starts=portal_starts=0;
    assert(wifi_start(nullptr)==ESP_OK && clock_starts==1);
}
int main(){
    SystemRole system;VoiceRole voice;
    assert(system.Platform()==nullptr && voice.Platform()!=nullptr);
    Boot(system.Platform()); // Empty factory: WiFi setup must work without claiming.
    assert(portal_starts==1 && cloud_starts==0 && realtime_starts==0);
    assert(last_status.phase==AGENT_LINK_PHASE_SETUP);
    s_phase=Phase::kStaConnected;
    agent_cloud_status_t stale{AGENT_CLOUD_CLAIMING,401,"123456",600};
    Compose(stale,&last_status);
    assert(last_status.phase==AGENT_LINK_PHASE_CONNECTING && !strstr(last_status.hint,"platform"));
    portal=false;Compose(stale,&last_status);
    assert(last_status.phase==AGENT_LINK_PHASE_CONNECTED && !strcmp(last_status.title,"WiFi connected"));
    assert(last_status.code[0]==0 && last_status.expires_s==0 && last_status.detail==0);
    stored_wifi=true;
    Boot(system.Platform()); // Factory reuses credentials and starts no cloud clients.
    assert(sta_starts==1 && cloud_starts==0 && realtime_starts==0);
    Boot(voice.Platform());
    assert(sta_starts==1 && cloud_starts==1 && realtime_starts==1);
    StartCloudSession();assert(cloud_starts==1); // No duplicate session startup.
    s_phase=Phase::kStaConnected;Compose(stale,&last_status);
    assert(last_status.phase==AGENT_LINK_PHASE_PAIRING && !strcmp(last_status.code,"123456"));
    Boot(system.Platform()); // Voice -> factory reboot still retains WiFi and disables claiming.
    assert(stored_wifi && sta_starts==1 && cloud_starts==0 && realtime_starts==0);
    init_error=ESP_FAIL;clock_starts=0;
    assert(wifi_start(nullptr)==ESP_FAIL && clock_starts==0 && cloud_starts==0);
    puts("Production network modes: factory setup and reconnect, independent SNTP startup, no cloud clients, Voice activation and return-to-factory passed.");
}
