#include "agent_system_stubs.h"
#include "agent_system.h"
#include "amoled_ui_controls.h"
#include "amoled_launcher_model.h"
#include "../components/agent_system/cover_cache.h"
#include <fstream>
#include <cassert>
#include <cstdio>
#include <future>
#include <chrono>

namespace {
std::vector<uint8_t> Image(const char* id,const char* board=agent_system::kBoard) {
    std::vector<uint8_t> bytes(1024,0);
    bytes[0]=0xE9;bytes[12]=9;
    const uint32_t magic=ESP_APP_DESC_MAGIC_WORD;memcpy(bytes.data()+0x20,&magic,sizeof magic);
    memcpy(bytes.data()+0x30,"2.0.0",6);
    agent_system::Descriptor d{{'A','G','S','Y','S','0','0','1'},1,1,{},{},{},{}};
    snprintf(d.app_id,sizeof d.app_id,"%s",id);snprintf(d.name,sizeof d.name,"Example");
    snprintf(d.board,sizeof d.board,"%s",board);snprintf(d.sdk,sizeof d.sdk,"0.1.0");
    memcpy(bytes.data()+0x120,&d,sizeof d);return bytes;
}
agent_system::CatalogApp Download(const std::vector<uint8_t>& bytes) {
    fixture::response.assign(reinterpret_cast<const char*>(bytes.data()),bytes.size());
    agent_system::CatalogApp app{};
    snprintf(app.app_id,sizeof app.app_id,"agent.new");snprintf(app.name,sizeof app.name,"Example");
    snprintf(app.version,sizeof app.version,"2.0.0");snprintf(app.url,sizeof app.url,"http://192.168.1.2:8765/app.bin");
    app.size=static_cast<uint32_t>(bytes.size());
    uint8_t sum=0;for(auto b:bytes)sum+=b;
    for(int i=0;i<32;++i)snprintf(app.sha256+i*2,3,"%02x",sum);
    return app;
}
void Seed(int index,const char* id,const char* board=agent_system::kBoard) {
    const auto& part=fixture::partitions[index];auto image=Image(id,board);
    fixture::flash[part.address].assign(part.size,0xFF);
    memcpy(fixture::flash[part.address].data(),image.data(),image.size());
    fixture::image_lengths[part.address]=image.size();fixture::valid[part.address]=true;
}
void PauseHttp() {fixture::open_entered=false;fixture::release_open=false;fixture::pause_open=true;}
void WaitHttp() {
    const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(!fixture::open_entered.load() && std::chrono::steady_clock::now()<until)std::this_thread::yield();
    assert(fixture::open_entered.load());
}
void ReleaseHttp() {fixture::release_open=true;fixture::pause_open=false;}
}
int main() {
    using namespace agent_system;
    AmoledLauncherModel carousel;
    assert(carousel.IsAdd() && carousel.Pages()==1 && !carousel.Move(1) && !carousel.Move(-1));
    carousel.SetCount(2);assert(!carousel.IsAdd() && carousel.Pages()==3);
    assert(carousel.Move(1) && carousel.Index()==1 && !carousel.IsAdd());
    assert(carousel.Move(1) && carousel.Index()==2 && carousel.IsAdd() && !carousel.Move(1));
    assert(carousel.Move(-1) && carousel.Index()==1);
    carousel.SetCount(0);assert(carousel.Index()==0 && carousel.IsAdd());
    assert(ValidServiceUrl("http://192.168.1.2:8765/api/apps"));
    assert(ValidServiceUrl("https://example.com/api/apps"));
    assert(!ValidServiceUrl("http://8.8.8.8/api/apps"));
    assert(!ValidServiceUrl("http://192.168.1.2@evil.test/api/apps"));
    assert(!ValidServiceUrl("http://192.168.1.2\r\nX:1"));
    assert(Init()==ESP_OK);
    CoverStatus status;
    fixture::store["nvs/al_wifi/ssid"]={'h','o','m','e',0};
    fixture::store["nvs/al_dev/auth_key"]={'k','e','y',0};
    const auto shared=fixture::store;
    fixture::init_error=ESP_FAIL;assert(Init()==ESP_FAIL);assert(fixture::store==shared);fixture::init_error=0;
#ifdef SYSTEM_TEST_APP
    assert(!IsLauncher() && !CanReturnToLauncher());
    // Regression: ordinary USB voice image at 0x10000 must start without
    // confirming OTA state, selecting itself as a launcher or rebooting.
    Seed(0,"agent.voice");fixture::running=&fixture::partitions[0];
    assert(Ready()==ESP_OK && !CanReturnToLauncher());
    assert(fixture::confirms==0 && fixture::boot==nullptr && fixture::restarts==0);
    assert(ReturnToLauncher()==ESP_ERR_INVALID_STATE && fixture::restarts==0);
    // Missing, non-launcher, foreign-board and corrupt factory images must not
    // become return targets. A ready OTA app can continue without a launcher.
    fixture::running=&fixture::partitions[1];fixture::boot=fixture::running;
    for(int invalid=0;invalid<4;++invalid) {
        if(invalid==0)fixture::flash.erase(fixture::partitions[0].address);
        if(invalid==1)Seed(0,"agent.voice");
        if(invalid==2)Seed(0,"agent.launcher","wrong-board");
        if(invalid==3) {Seed(0,"agent.launcher");fixture::valid[fixture::partitions[0].address]=false;}
        fixture::ota_state=ESP_OTA_IMG_PENDING_VERIFY;
        const int confirms=fixture::confirms;
        assert(Ready()==ESP_OK && !CanReturnToLauncher());
        assert(fixture::confirms==confirms+1 && fixture::boot==fixture::running);
        assert(ReturnToLauncher()==ESP_ERR_NOT_FOUND && fixture::restarts==0);
    }
    // All three supported install slots confirm readiness and arm the verified
    // launcher without interrupting the current app.
    Seed(0,"agent.launcher");
    for(int slot=1;slot<=3;++slot) {
        fixture::running=&fixture::partitions[slot];fixture::boot=nullptr;
        fixture::ota_state=ESP_OTA_IMG_PENDING_VERIFY;
        const int confirms=fixture::confirms;
        assert(Ready()==ESP_OK && CanReturnToLauncher());
        assert(fixture::confirms==confirms+1 && fixture::boot==&fixture::partitions[0]);
        assert(Ready()==ESP_OK && fixture::confirms==confirms+1); // Idempotent OTA confirmation.
        assert(fixture::restarts==0);
    }
    // A later flash failure/corruption must not make ReturnToLauncher restart
    // into an invalid image, and optional boot arming must not abort a ready app.
    fixture::valid[fixture::partitions[0].address]=false;
    assert(ReturnToLauncher()==ESP_ERR_NOT_FOUND && !CanReturnToLauncher() && fixture::restarts==0);
    Seed(0,"agent.launcher");fixture::boot_error=ESP_FAIL;fixture::boot=nullptr;
    assert(Ready()==ESP_OK && CanReturnToLauncher() && fixture::boot==nullptr);
    assert(ReturnToLauncher()==ESP_FAIL && fixture::restarts==0);
    fixture::boot_error=ESP_OK;
    fixture::ota_state=ESP_OTA_IMG_PENDING_VERIFY;fixture::confirm_error=ESP_FAIL;
    assert(Ready()==ESP_FAIL && !CanReturnToLauncher() && fixture::boot==nullptr);
    fixture::confirm_error=ESP_OK;
    fixture::running=nullptr;assert(Ready()==ESP_ERR_INVALID_STATE && !CanReturnToLauncher());
    fixture::running=&fixture::partitions[1];assert(Ready()==ESP_OK && CanReturnToLauncher());
    assert(ReturnToLauncher()==ESP_OK && fixture::restarts==1);
    for(const auto& value:shared)assert(fixture::store[value.first]==value.second);
    Seed(1,"agent.voice");fixture::store["app_nvs/al_apps/ota_0"]={2};
    assert(Ready()==ESP_OK && fixture::store["app_nvs/al_apps/ota_0"]==std::vector<uint8_t>{1});
    App visible[kAppSlotCount];size_t visible_count=0;
    assert(List(visible,kAppSlotCount,&visible_count)==ESP_OK && visible_count==1);
    assert(Remove("ota_1")==ESP_ERR_INVALID_STATE);
    puts("Startup modes: standalone USB, all OTA slots, launcher identity/integrity, rollback confirmation and return failures passed.");
#else
    assert(IsLauncher() && Ready()==ESP_OK && !CanReturnToLauncher());
    assert(ReturnToLauncher()==ESP_ERR_INVALID_STATE && fixture::restarts==0);
    assert(SetCatalogUrl("http://192.168.1.2:8765/api/apps")==ESP_OK);
    CatalogApp catalog[8];size_t count=0;
    const std::string valid=R"({"schema":1,"layout":1,"board":"ESP32-S3-Touch-AMOLED-1.75-16M","apps":[{"app_id":"agent.new","name":"Example","version":"2.0.0","size":1024,"sha256":"0000000000000000000000000000000000000000000000000000000000000000","url":"/firmware/app.bin"}]})";
    fixture::response=valid;assert(FetchCatalog(catalog,8,&count)==ESP_OK&&count==1);
    assert(!strcmp(catalog[0].url,"http://192.168.1.2:8765/firmware/app.bin"));
    auto bad=valid;bad.replace(bad.find("16M"),3,"32M");fixture::response=bad;
    assert(FetchCatalog(catalog,8,&count)!=ESP_OK&&count==0);
    bad=valid;bad.replace(bad.find("1024"),4,"-1");fixture::response=bad;assert(FetchCatalog(catalog,8,&count)!=ESP_OK);
    bad=valid;bad.replace(bad.find("1024"),4,std::to_string(kAppImageMaxBytes+1));fixture::response=bad;
    assert(FetchCatalog(catalog,8,&count)!=ESP_OK);
    fixture::response=valid;assert(FetchCatalog(catalog,0,&count)!=ESP_OK);
    fixture::response="{";assert(FetchCatalog(catalog,8,&count)!=ESP_OK);
    Seed(1,"agent.voice");Seed(2,"test.notes");
    Cover cover;assert(ReadCover("ota_0",&cover)==ESP_ERR_NOT_FOUND);
#ifdef TEST_COVER_FILE
    std::ifstream cover_file(TEST_COVER_FILE,std::ios::binary);
    const std::string raw((std::istreambuf_iterator<char>(cover_file)),{});
    assert(raw.size()==kCoverBytes);
    CatalogApp image_app{};snprintf(image_app.app_id,sizeof image_app.app_id,"agent.voice");
    snprintf(image_app.cover_url,sizeof image_app.cover_url,"https://example.test/voice.raw");
    snprintf(image_app.cover_format,sizeof image_app.cover_format,"%s",kCoverFormat);
    uint8_t sum=0;for(unsigned char b:raw)sum+=b;
    for(int i=0;i<32;++i)snprintf(image_app.cover_sha256+i*2,3,"%02x",sum);
    image_app.cover_size=static_cast<uint32_t>(raw.size());
    fixture::responses[image_app.cover_url]=raw;
    const auto firmware_before=Image("agent.voice");
    assert(cover_cache::Update("ota_0",image_app)==ESP_OK);
    assert(GetCoverStatus("ota_0",&status)==ESP_OK && status.state==CoverState::kReady && status.cached);
    assert(ReadCover("ota_0",&cover)==ESP_OK && cover.slot==0);
    auto retained=AcquireCover(cover);assert(retained && !memcmp(retained->blob.get(),raw.data(),raw.size()));
    assert(!memcmp(fixture::flash[0x310000].data(),firmware_before.data(),firmware_before.size()));
    assert(!memcmp(fixture::flash[0x310000].data()+kAppImageMaxBytes,raw.data(),raw.size()));
    cover_cache::Forget("ota_0");assert(!AcquireCover(cover));
    assert(!memcmp(retained->blob.get(),raw.data(),raw.size())); // UI owns its immutable snapshot.
    assert(ReadCover("ota_0",&cover)==ESP_OK); // Simulated reboot: reload the verified slot tail.
    fixture::responses[image_app.cover_url]="network unavailable";
    assert(cover_cache::Update("ota_0",image_app)==ESP_OK); // Cache hit needs no HTTP.
    auto invalid=raw;invalid[10]=1;uint8_t badsum=0;for(unsigned char b:invalid)badsum+=b;
    for(int i=0;i<32;++i)snprintf(image_app.cover_sha256+i*2,3,"%02x",badsum);
    fixture::responses[image_app.cover_url]=invalid;
    assert(cover_cache::Update("ota_0",image_app)!=ESP_OK);
    assert(GetCoverStatus("ota_0",&status)==ESP_OK && status.state==CoverState::kFailed && status.stage==CoverStage::kValidate && status.cached);
    assert(ReadCover("ota_0",&cover)==ESP_OK); // Bad update preserves previous image.
    PauseHttp();
    std::thread updating([&]{assert(cover_cache::Update("ota_0",image_app)==ESP_ERR_INVALID_CRC);});
    WaitHttp();
    auto reading=std::async(std::launch::async,[&]{Cover current;return ReadCover("ota_0",&current);});
    const bool responsive=reading.wait_for(std::chrono::milliseconds(500))==std::future_status::ready;
    ReleaseHttp();updating.join();assert(reading.get()==ESP_OK && responsive);
    fixture::open_error=ESP_FAIL;
    assert(cover_cache::Update("ota_0",image_app)==ESP_FAIL);
    assert(GetCoverStatus("ota_0",&status)==ESP_OK && status.stage==CoverStage::kConnect);
    fixture::open_error=ESP_OK;fixture::status=403;
    assert(cover_cache::Update("ota_0",image_app)==ESP_ERR_INVALID_RESPONSE);
    assert(GetCoverStatus("ota_0",&status)==ESP_OK && status.stage==CoverStage::kHeaders);
    fixture::status=200;fixture::responses[image_app.cover_url]=raw.substr(0,raw.size()-1);
    assert(cover_cache::Update("ota_0",image_app)==ESP_ERR_INVALID_SIZE);
    assert(GetCoverStatus("ota_0",&status)==ESP_OK && status.stage==CoverStage::kHeaders);
    fixture::responses[image_app.cover_url]=raw;
    for(int i=0;i<32;++i)snprintf(image_app.cover_sha256+i*2,3,"%02x",sum);
    // Partial flash write: invalidate the old tail, keep app bytes intact, then recover.
    fixture::flash[0x310000][kAppImageMaxBytes]=0xFF;cover_cache::Forget("ota_0");
    fixture::partition_write_error=ESP_FAIL;
    assert(cover_cache::Update("ota_0",image_app)==ESP_FAIL);
    assert(GetCoverStatus("ota_0",&status)==ESP_OK && status.stage==CoverStage::kPublish);
    assert(ReadCover("ota_0",&cover)!=ESP_OK);
    assert(!memcmp(fixture::flash[0x310000].data(),firmware_before.data(),firmware_before.size()));
    fixture::partition_write_error=ESP_OK;assert(cover_cache::Update("ota_0",image_app)==ESP_OK);
    // Reject an older oversized app before touching its tail or opening HTTP.
    fixture::image_lengths[0x310000]=0x300000;
    const int before_http=fixture::opens,before_erase=fixture::erases;
    assert(cover_cache::Update("ota_0",image_app)==ESP_ERR_INVALID_SIZE);
    assert(fixture::opens==before_http && fixture::erases==before_erase);
    fixture::image_lengths[0x310000]=firmware_before.size();
    snprintf(image_app.version,sizeof image_app.version,"2.0.0");
    CatalogApp sync_apps[2]={image_app,{}};
    snprintf(sync_apps[1].app_id,sizeof sync_apps[1].app_id,"test.notes");
    snprintf(sync_apps[1].version,sizeof sync_apps[1].version,"2.0.0");
    assert(SyncCovers(sync_apps,2)==ESP_OK);
    assert(GetCoverStatus("ota_0",&status)==ESP_OK && status.state==CoverState::kReady);
    assert(GetCoverStatus("ota_1",&status)==ESP_OK && status.state==CoverState::kNoImage);
    assert(SyncCovers(sync_apps,0)==ESP_OK);
    assert(GetCoverStatus("ota_0",&status)==ESP_OK && status.state==CoverState::kNotListed && status.cached);
    puts("Raw slot cover: backend-generated input, flash address, app boundaries, offline reload, snapshot lifetime, partial writes and recovery passed.");
#endif
    assert(ReadCover("factory",&cover)==ESP_ERR_INVALID_ARG);
    App installed[3];assert(List(installed,3,&count)==ESP_OK&&count==2);
    auto duplicate=Download(Image("agent.voice"));snprintf(duplicate.app_id,sizeof duplicate.app_id,"agent.voice");
    int opens=fixture::opens,erases=fixture::erases;
    assert(Install(duplicate)==kAlreadyInstalled && fixture::opens==opens && fixture::erases==erases);
    snprintf(duplicate.version,sizeof duplicate.version,"3.0.0");
    assert(Install(duplicate)==kAlreadyInstalled && fixture::opens==opens); // Any version of the same app ID.
    assert(Remove("factory")==ESP_ERR_INVALID_ARG&&fixture::erases==erases);
    const auto voice=fixture::flash[0x310000],info=fixture::flash[0x610000];
    auto bytes=Image("agent.new");auto app=Download(bytes);
    app.size=kAppImageMaxBytes+1;opens=fixture::opens;
    assert(Install(app)==ESP_ERR_INVALID_ARG && fixture::opens==opens);
    app=Download(bytes);
    fixture::fail_after=300;assert(Install(app)!=ESP_OK);fixture::fail_after=static_cast<size_t>(-1);
    assert(List(installed,3,&count)==ESP_OK&&count==2);
    assert(fixture::flash[0x310000]==voice&&fixture::flash[0x610000]==info);
    app=Download(bytes);app.sha256[0]=app.sha256[0]=='0'?'1':'0';assert(Install(app)==ESP_ERR_INVALID_CRC);
    auto wrong=Image("agent.new","wrong-board");app=Download(wrong);assert(Install(app)==ESP_ERR_INVALID_VERSION);
    app=Download(bytes);fixture::end_error=ESP_FAIL;assert(Install(app)==ESP_FAIL);fixture::end_error=0;
    app=Download(bytes);PauseHttp();esp_err_t install_result=ESP_FAIL;
    std::thread installing([&]{install_result=Install(app);});WaitHttp();
    opens=fixture::opens;
    assert(Install(app)==ESP_ERR_INVALID_STATE && fixture::opens==opens); // An in-flight install owns the transaction.
    ReleaseHttp();installing.join();assert(install_result==ESP_OK);
    assert(fixture::boot==nullptr&&fixture::restarts==0);
    assert(List(installed,3,&count)==ESP_OK&&count==3);
    assert(Install(app)==kAlreadyInstalled);
    auto other=Download(Image("agent.other"));snprintf(other.app_id,sizeof other.app_id,"agent.other");
    assert(Install(other)==kSlotsFull);
    assert(Launch("factory")==ESP_ERR_NOT_FOUND);assert(Launch("ota_2")==ESP_OK);
    assert(fixture::boot==&fixture::partitions[3]&&fixture::restarts==1);
    assert(Remove("ota_2")==ESP_OK);assert(List(installed,3,&count)==ESP_OK&&count==2);
    assert(Remove("ota_0")==ESP_OK); // Hole before an installed app must not bypass the identity check.
    duplicate=Download(Image("test.notes"));snprintf(duplicate.app_id,sizeof duplicate.app_id,"test.notes");
    opens=fixture::opens;erases=fixture::erases;
    assert(Install(duplicate)==kAlreadyInstalled && fixture::opens==opens && fixture::erases==erases);
    app=Download(bytes);assert(Install(app)==ESP_OK); // Removing an app allows it to be installed again.
    assert(fixture::store["nvs/al_wifi/ssid"]==shared.at("nvs/al_wifi/ssid"));
    assert(fixture::store["nvs/al_dev/auth_key"]==shared.at("nvs/al_dev/auth_key"));
#endif
    amoled_ui::SwipeTracker gesture;
    assert(gesture.Update(true,230,60,1,0)==amoled_ui::Swipe::kNone);
    assert(gesture.Update(true,230,125,1,100000)==amoled_ui::Swipe::kSystem);
    gesture.Update(false,230,125,1,110000);
    gesture.Update(true,230,200,1,200000);
    assert(gesture.Update(true,230,280,1,300000)==amoled_ui::Swipe::kNone);
    puts("System transaction, shared settings, startup and system gesture checks passed.");
}
