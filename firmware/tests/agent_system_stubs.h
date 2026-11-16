#pragma once
#ifdef _MSC_VER
#define __attribute__(...)
#endif
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_LOG_LEVEL_LOCAL(...) ((void)0)
constexpr int ESP_LOG_INFO = 3, ESP_LOG_DEBUG = 4;
#define esp_err_to_name(...) "fixture-error"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>
inline int64_t esp_timer_get_time() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
#include <cstdlib>
#define pdMS_TO_TICKS(value) (value)
namespace fixture { inline unsigned erase_yields = 0; }
inline void vTaskDelay(unsigned) { ++fixture::erase_yields; std::this_thread::yield(); }
using esp_err_t = int;
constexpr int ESP_OK=0, ESP_FAIL=-1, ESP_ERR_NO_MEM=1, ESP_ERR_INVALID_ARG=2,
    ESP_ERR_INVALID_STATE=3, ESP_ERR_INVALID_SIZE=4, ESP_ERR_NOT_FOUND=5,
    ESP_ERR_INVALID_RESPONSE=6, ESP_ERR_INVALID_CRC=7, ESP_ERR_INVALID_VERSION=8,
    ESP_ERR_NVS_NOT_FOUND=9, ESP_ERR_NVS_NO_FREE_PAGES=10, ESP_ERR_NVS_NEW_VERSION_FOUND=11,
    ESP_ERR_NOT_SUPPORTED=12, ESP_ERR_TIMEOUT=13,
    ESP_ERR_HTTP_CONNECT=0x7002, ESP_ERR_HTTP_EAGAIN=0x7007;
using esp_partition_subtype_t = int;
constexpr int ESP_PARTITION_TYPE_APP=0, ESP_PARTITION_SUBTYPE_APP_FACTORY=0,
    ESP_PARTITION_SUBTYPE_APP_OTA_0=16;
struct esp_partition_t { uint32_t address, size; int type, subtype; char label[17]; };
constexpr uint32_t ESP_APP_DESC_MAGIC_WORD=0xABCD5432;
struct esp_app_desc_t { uint32_t magic_word; uint8_t prefix[12]; char version[32], project_name[32]; char padding[176]; };
struct esp_partition_pos_t { uint32_t offset, size; };
using esp_partition_mmap_handle_t = uint32_t;
constexpr int ESP_PARTITION_MMAP_DATA = 0;
struct esp_image_metadata_t { uint32_t image_len; };
constexpr int ESP_IMAGE_VERIFY_SILENT=0;
using esp_ota_handle_t = int;
using esp_ota_img_states_t = int;
constexpr int ESP_OTA_IMG_PENDING_VERIFY=1, ESP_OTA_IMG_VALID=2;
constexpr int MBEDTLS_ERR_X509_CERT_VERIFY_FAILED = -0x2700;
constexpr int MBEDTLS_ERR_X509_FATAL_ERROR = -0x3000;
namespace fixture {
inline esp_partition_t partitions[] = {{0x10000,0x300000,0,0,"factory"},
    {0x310000,0x300000,0,16,"ota_0"},{0x610000,0x300000,0,17,"ota_1"},
    {0x910000,0x300000,0,18,"ota_2"}};
inline std::map<uint32_t,std::vector<uint8_t>> flash;
inline std::map<uint32_t,bool> valid;
inline std::map<uint32_t,uint32_t> image_lengths;
inline size_t write_offset=0;
inline int partition_write_error=ESP_OK;
inline const esp_partition_t* running=&partitions[0];
inline const esp_partition_t* boot=nullptr;
inline const esp_partition_t* writing=nullptr;
inline int restarts=0, confirms=0, erases=0, init_error=0;
inline int ota_state=ESP_OTA_IMG_PENDING_VERIFY, confirm_error=ESP_OK, boot_error=ESP_OK;
inline std::string response;
inline std::map<std::string,std::string> responses;
inline std::map<std::string,int> http_status;
inline std::map<std::string, int> open_errors;
inline std::map<std::string, std::pair<int, int>> tls_errors;
inline std::map<std::string,std::map<std::string,std::string>> http_headers;
inline std::map<std::string,std::vector<int64_t>> header_results;
inline std::map<std::string,std::vector<int>> header_timeouts;
inline std::map<std::string,int> header_delay_ms;
inline std::map<std::string,std::vector<int>> read_results, read_timeouts;
inline int active_http_clients = 0;
struct Request { std::string url; int method; std::map<std::string,std::string> headers; };
inline std::vector<Request> requests;
inline size_t fail_after=static_cast<size_t>(-1);
inline int end_error=0;
inline int status=200;
inline int opens=0, open_error=ESP_OK, commit_error=ESP_OK;
inline std::atomic<bool> pause_open{false},open_entered{false},release_open{false};
inline std::map<std::string,std::vector<uint8_t>> store;
inline std::map<int,std::string> handles;
inline int next_handle=1;
}
inline const esp_partition_t* esp_partition_find_first(int type,int subtype,const char* name) {
    for(const auto& p: fixture::partitions) if(p.type==type&&p.subtype==subtype&&(!name||!strcmp(name,p.label))) return &p;
    return nullptr;
}
inline int esp_partition_read(const esp_partition_t* p,size_t offset,void* out,size_t size) {
    auto& bytes=fixture::flash[p->address]; if(offset+size>bytes.size()) return ESP_FAIL;
    memcpy(out,bytes.data()+offset,size); return ESP_OK;
}
inline int esp_partition_mmap(const esp_partition_t* p,size_t offset,size_t size,int,const void** out,uint32_t* handle) {
    auto& bytes=fixture::flash[p->address];if(offset+size>bytes.size())return ESP_ERR_INVALID_SIZE;
    *out=bytes.data()+offset;*handle=p->address;return ESP_OK;
}
inline int esp_partition_erase_range(const esp_partition_t* p,size_t offset,size_t size) {
    ++fixture::erases;
    if(offset+size>p->size)return ESP_ERR_INVALID_SIZE;
    auto& bytes=fixture::flash[p->address];bytes.resize(p->size,0xFF);
    std::fill(bytes.begin()+offset,bytes.begin()+offset+size,0xFF);
    if(offset==0) {fixture::valid[p->address]=false;fixture::image_lengths[p->address]=0;}
    return ESP_OK;
}
inline int esp_partition_write(const esp_partition_t* p,size_t offset,const void* data,size_t size) {
    auto& bytes=fixture::flash[p->address];bytes.resize(p->size,0xFF);
    if(offset+size>p->size)return ESP_ERR_INVALID_SIZE;
    memcpy(bytes.data()+offset,data,fixture::partition_write_error ? std::min<size_t>(17,size) : size);
    return fixture::partition_write_error;
}
inline int esp_ota_get_partition_description(const esp_partition_t* p,esp_app_desc_t* d) {
    const int err=esp_partition_read(p,32,d,sizeof *d);
    return err==ESP_OK && d->magic_word==ESP_APP_DESC_MAGIC_WORD ? ESP_OK : ESP_ERR_INVALID_VERSION;
}
inline int esp_image_verify(int,const esp_partition_pos_t* p,esp_image_metadata_t* image) {
    image->image_len=fixture::image_lengths[p->offset];
    return fixture::valid[p->offset]?ESP_OK:ESP_FAIL;
}
inline const esp_partition_t* esp_ota_get_running_partition(){return fixture::running;}
inline int esp_ota_get_state_partition(const esp_partition_t*,int* s){*s=fixture::ota_state;return ESP_OK;}
inline int esp_ota_mark_app_valid_cancel_rollback(){
    ++fixture::confirms;
    if(fixture::confirm_error!=ESP_OK)return fixture::confirm_error;
    fixture::ota_state=ESP_OTA_IMG_VALID;return ESP_OK;
}
inline int esp_ota_set_boot_partition(const esp_partition_t* p){
    if(fixture::boot_error!=ESP_OK)return fixture::boot_error;
    fixture::boot=p;return ESP_OK;
}
inline void esp_restart(){++fixture::restarts;}
inline int esp_ota_begin(const esp_partition_t* p,size_t,int* handle) {
    fixture::writing=p;fixture::flash[p->address].resize(p->size,0xFF);fixture::write_offset=0;
    fixture::valid[p->address]=false;fixture::image_lengths[p->address]=0;*handle=1;return ESP_OK;
}
inline int esp_ota_write(int,const void* data,size_t size) {
    auto& bytes=fixture::flash[fixture::writing->address];auto* p=static_cast<const uint8_t*>(data);
    memcpy(bytes.data()+fixture::write_offset,p,size);fixture::write_offset+=size;
    fixture::image_lengths[fixture::writing->address]=fixture::write_offset;return ESP_OK;
}
inline int esp_ota_abort(int){return ESP_OK;}
inline int esp_ota_end(int){
    if(fixture::end_error)return fixture::end_error;
    const auto& data=fixture::flash[fixture::writing->address];
    if(data.size()<24 || data[0]!=0xE9 || data[12]!=9 || data[13])return ESP_ERR_INVALID_VERSION;
    fixture::valid[fixture::writing->address]=true;return ESP_OK;
}
using nvs_handle_t=int;
using nvs_open_mode_t=int;
constexpr int NVS_READONLY=0,NVS_READWRITE=1;
inline int nvs_flash_init(){return fixture::init_error;}
inline int nvs_flash_init_partition(const char*){return ESP_OK;}
inline int nvs_flash_erase_partition(const char* partition){
    if(!strcmp(partition,"nvs"))fixture::init_error=ESP_OK;
    const std::string prefix=std::string(partition)+"/";
    for(auto i=fixture::store.begin();i!=fixture::store.end();) {
        if(i->first.compare(0,prefix.size(),prefix)==0)i=fixture::store.erase(i);else ++i;
    }
    return ESP_OK;
}
inline int nvs_flash_erase(){return nvs_flash_erase_partition("nvs");}
inline int nvs_open_from_partition(const char* p,const char* ns,int,int* h){*h=fixture::next_handle++;fixture::handles[*h]=std::string(p)+"/"+ns+"/";return ESP_OK;}
inline int nvs_open(const char* ns,int mode,int* h){return nvs_open_from_partition("nvs",ns,mode,h);}
inline int nvs_get_u8(int h,const char* key,uint8_t* out){auto i=fixture::store.find(fixture::handles[h]+key);if(i==fixture::store.end())return ESP_ERR_NVS_NOT_FOUND;*out=i->second[0];return ESP_OK;}
inline int nvs_set_u8(int h,const char* key,uint8_t value){fixture::store[fixture::handles[h]+key]={value};return ESP_OK;}
inline int nvs_get_str(int h,const char* key,char* out,size_t* size){auto i=fixture::store.find(fixture::handles[h]+key);if(i==fixture::store.end())return ESP_ERR_NVS_NOT_FOUND;if(i->second.size()>*size)return ESP_ERR_INVALID_SIZE;memcpy(out,i->second.data(),i->second.size());*size=i->second.size();return ESP_OK;}
inline int nvs_set_str(int h,const char* key,const char* value){auto* p=reinterpret_cast<const uint8_t*>(value);fixture::store[fixture::handles[h]+key]={p,p+strlen(value)+1};return ESP_OK;}
inline int nvs_set_blob(int h,const char* key,const void* value,size_t size){auto* p=static_cast<const uint8_t*>(value);fixture::store[fixture::handles[h]+key]={p,p+size};return ESP_OK;}
inline int nvs_get_blob(int h,const char* key,void* value,size_t* size){auto i=fixture::store.find(fixture::handles[h]+key);if(i==fixture::store.end())return ESP_ERR_NVS_NOT_FOUND;if(*size<i->second.size())return ESP_ERR_INVALID_SIZE;memcpy(value,i->second.data(),i->second.size());*size=i->second.size();return ESP_OK;}
inline int nvs_commit(int){return fixture::commit_error;}
inline void nvs_close(int){}
struct esp_http_client_event_t;
struct HttpClient {size_t offset=0;std::string body,url;int method=0,timeout_ms=0;void* user_data=nullptr;
    esp_err_t(*event)(esp_http_client_event_t*)=nullptr;std::map<std::string,std::string> headers;};
using esp_http_client_handle_t=HttpClient*;
inline void esp_crt_bundle_attach(){}
constexpr int HTTP_EVENT_ON_HEADER=1, HTTP_METHOD_GET=0, HTTP_METHOD_POST=1;
struct esp_http_client_event_t {int event_id;void* user_data;const char* header_key;const char* header_value;};
struct esp_http_client_config_t {const char* url;const char* user_agent;int timeout_ms,buffer_size;void(*crt_bundle_attach)();bool disable_auto_redirect;
    esp_err_t(*event_handler)(esp_http_client_event_t*);void* user_data;};
inline HttpClient* esp_http_client_init(const esp_http_client_config_t* c){auto* client=new HttpClient;auto i=fixture::responses.find(c->url);client->body=i==fixture::responses.end()?fixture::response:i->second;
    ++fixture::active_http_clients;client->timeout_ms=c->timeout_ms;
    client->url=c->url;client->event=c->event_handler;client->user_data=c->user_data;return client;}
inline int esp_http_client_set_timeout_ms(HttpClient* c,int value){c->timeout_ms=value;return ESP_OK;}
inline int esp_http_client_set_header(HttpClient* c,const char* key,const char* value){c->headers[key]=value;return ESP_OK;}
inline int esp_http_client_set_method(HttpClient* c,int method){c->method=method;return ESP_OK;}
inline void esp_fill_random(void* out,size_t size){static uint8_t seed=0;memset(out,++seed,size);}
inline int esp_http_client_open(HttpClient* c,int){
    ++fixture::opens;
    fixture::requests.push_back({c->url,c->method,c->headers});
    if(fixture::pause_open.load()) {
        fixture::open_entered.store(true);
        while(!fixture::release_open.load())std::this_thread::yield();
    }
    const auto found = fixture::open_errors.find(c->url);
    return found == fixture::open_errors.end() ? fixture::open_error : found->second;
}
inline int esp_http_client_get_and_clear_last_tls_error(HttpClient* c, int* code, int* flags) {
    *code = 0;
    *flags = 0;
    const auto found = fixture::tls_errors.find(c->url);
    if (found == fixture::tls_errors.end())
        return ESP_OK;
    *code = found->second.first;
    *flags = found->second.second;
    fixture::tls_errors.erase(found);
    return ESP_FAIL;
}
inline int64_t esp_http_client_fetch_headers(HttpClient* c){
    fixture::header_timeouts[c->url].push_back(c->timeout_ms);
    if(fixture::header_delay_ms[c->url]>c->timeout_ms)return -ESP_ERR_HTTP_EAGAIN;
    auto& results=fixture::header_results[c->url];
    if(!results.empty()) {const auto result=results.front();results.erase(results.begin());if(result<0)return result;}
    if(c->event) for(const auto& [key,value]:fixture::http_headers[c->url]){
    esp_http_client_event_t e{HTTP_EVENT_ON_HEADER,c->user_data,key.c_str(),value.c_str()};c->event(&e);}return c->body.size();}
inline int esp_http_client_get_status_code(HttpClient* c){auto i=fixture::http_status.find(c->url);return i==fixture::http_status.end()?fixture::status:i->second;}
inline int esp_http_client_get_errno(HttpClient*) {return 0;}
inline int64_t esp_http_client_get_content_length(HttpClient* c){return c->body.size();}
inline int esp_http_client_read(HttpClient* client,char* out,size_t size){
    fixture::read_timeouts[client->url].push_back(client->timeout_ms);
    auto& results=fixture::read_results[client->url];
    if(!results.empty()) {const int result=results.front();results.erase(results.begin());if(result<0)return result;}
    if(client->offset>=fixture::fail_after)return -1;
    size=std::min({size,client->body.size()-client->offset,size_t(211),fixture::fail_after-client->offset});
    memcpy(out,client->body.data()+client->offset,size);client->offset+=size;return static_cast<int>(size);
}
inline bool esp_http_client_is_complete_data_received(HttpClient* c){return c->offset==c->body.size();}
inline void esp_http_client_close(HttpClient*){}
inline void esp_http_client_cleanup(HttpClient* c){--fixture::active_http_clients;delete c;}
constexpr int MALLOC_CAP_SPIRAM=1,MALLOC_CAP_8BIT=2;
inline void* heap_caps_malloc(size_t size,int){return malloc(size);}
inline void* heap_caps_realloc(void* p,size_t size,int){return realloc(p,size);}
inline void* heap_caps_calloc(size_t n,size_t size,int){return calloc(n,size);}
inline void heap_caps_free(void* p){free(p);}
#if defined(AGENT_SYSTEM_REAL_SHA256)
#include "agent_system_real_sha256.h"
#else
// Deterministic hash double: tests verify digest enforcement and transaction order,
// while the firmware uses mbedTLS SHA-256. Cryptographic primitives are not reimplemented here.
struct mbedtls_sha256_context {uint8_t sum=0;};
inline void mbedtls_sha256_init(mbedtls_sha256_context* c){c->sum=0;}
inline int mbedtls_sha256_starts(mbedtls_sha256_context* c,int){c->sum=0;return 0;}
inline int mbedtls_sha256_update(mbedtls_sha256_context* c,const uint8_t* p,size_t size){for(size_t i=0;i<size;++i)c->sum+=p[i];return 0;}
inline int mbedtls_sha256_finish(mbedtls_sha256_context* c,uint8_t out[32]){memset(out,c->sum,32);return 0;}
inline void mbedtls_sha256_free(mbedtls_sha256_context*){}
#endif
using httpd_handle_t=void*;
struct httpd_req_t {int content_len;};
struct httpd_config_t {int server_port,ctrl_port,max_uri_handlers,stack_size;bool lru_purge_enable;};
#define HTTPD_DEFAULT_CONFIG() httpd_config_t{}
struct httpd_uri_t {const char* uri;int method;int(*handler)(httpd_req_t*);};
constexpr int HTTP_GET=0,HTTP_POST=1,HTTPD_400_BAD_REQUEST=400;
inline int httpd_start(void** h,const httpd_config_t*){*h=reinterpret_cast<void*>(1);return ESP_OK;}
inline int httpd_stop(void*){return ESP_OK;}
inline int httpd_register_uri_handler(void*,const httpd_uri_t*){return ESP_OK;}
inline int httpd_resp_set_type(httpd_req_t*,const char*){return ESP_OK;}
inline int httpd_resp_send(httpd_req_t*,const char*,size_t){return ESP_OK;}
inline int httpd_resp_send_err(httpd_req_t*,int,const char*){return ESP_FAIL;}
inline int httpd_req_recv(httpd_req_t*,char*,size_t){return -1;}
inline int httpd_query_key_value(const char*,const char*,char*,size_t){return ESP_FAIL;}
inline int httpd_resp_set_status(httpd_req_t*,const char*){return ESP_OK;}
inline int httpd_resp_set_hdr(httpd_req_t*,const char*,const char*){return ESP_OK;}
