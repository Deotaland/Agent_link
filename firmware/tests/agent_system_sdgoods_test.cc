#include "agent_system_stubs.h"
#include "agent_system.h"
#include "../components/agent_system/cover_cache.h"
#include "../components/agent_link/src/core/storage.h"
#include <cassert>

namespace {
std::vector<uint8_t> Image(const char* name) {
    std::vector<uint8_t> bytes(1024);
    bytes[0]=0xE9; bytes[1]=1; bytes[12]=9; bytes[23]=1;
    esp_app_desc_t idf{};
    idf.magic_word=ESP_APP_DESC_MAGIC_WORD;
    snprintf(idf.version,sizeof idf.version,"1.2.3");
    snprintf(idf.project_name,sizeof idf.project_name,"%s",name);
    memcpy(bytes.data()+32,&idf,sizeof idf);
    return bytes;
}
agent_system::CatalogApp Download(const char* id,const std::vector<uint8_t>& bytes) {
    fixture::response.assign(reinterpret_cast<const char*>(bytes.data()),bytes.size());
    agent_system::CatalogApp app{};
    snprintf(app.app_id,sizeof app.app_id,"%s",id);
    snprintf(app.name,sizeof app.name,"Reviewed app");
    snprintf(app.version,sizeof app.version,"1.2.3");
    snprintf(app.url,sizeof app.url,"https://example.test/firmware/app.bin");
    app.size=static_cast<uint32_t>(bytes.size());
    app.format=agent_system::ImageFormat::kSdgoods;
    uint8_t sum=0;for(auto b:bytes)sum+=b;
    for(int i=0;i<32;++i)snprintf(app.sha256+i*2,3,"%02x",sum);
    return app;
}
uint32_t Crc(const uint8_t* data,size_t size) {
    uint32_t crc=0xFFFFFFFF;
    for(size_t i=0;i<size;++i) {crc^=data[i]; for(int bit=0;bit<8;++bit)crc=(crc>>1)^((crc&1)?0xEDB88320u:0);}
    return crc^0xFFFFFFFF;
}
}
int main() {
    using namespace agent_system;
    fixture::partitions[1].size=0x500000;
    fixture::partitions[2].address=0x810000; fixture::partitions[2].size=0x500000;
    assert(kAppImageMaxBytes==0x4F2000 && Init()==ESP_OK);
    int handle;
    assert(agentlink::storage::Init()==ESP_OK);
    assert(agentlink::storage::Open("al_dev",NVS_READWRITE,&handle)==ESP_OK);
    assert(nvs_set_str(handle,"auth_key","private-key")==ESP_OK);
    fixture::store["nvs/vendor/settings"]={1};
    nvs_flash_erase();
    assert(fixture::store.count("agent_nvs/al_dev/auth_key")==1);
    assert(fixture::store.count("nvs/vendor/settings")==0);
    for(int error:{ESP_ERR_NVS_NO_FREE_PAGES,ESP_ERR_NVS_NEW_VERSION_FOUND}) {
        fixture::init_error=error;
        fixture::store["nvs/vendor/settings"]={1};
        assert(Init()==ESP_OK);
        assert(fixture::store.count("nvs/vendor/settings")==0);
        assert(fixture::store.count("agent_nvs/al_dev/auth_key")==1);
    }
    const auto saved=fixture::store;
    // Migrate the earlier V3 catalog once; a vendor NVS erase must not remove
    // the operator's service, nor may clearing it resurrect the legacy value.
    const char* legacy="https://example.test/legacy.json";
    fixture::store["nvs/al_system/catalog"]={legacy,legacy+strlen(legacy)+1};
    char catalog_url[256]{};
    assert(CatalogUrl(catalog_url,sizeof catalog_url)==ESP_OK && !strcmp(catalog_url,legacy));
    assert(fixture::store.count("agent_nvs/al_system/catalog")==1);
    assert(SetCatalogUrl("")==ESP_OK);
    assert(CatalogUrl(catalog_url,sizeof catalog_url)==ESP_OK && !catalog_url[0]);
    assert(SetCatalogUrl("https://example.test/catalog.json")==ESP_OK);
    nvs_flash_erase();
    assert(CatalogUrl(catalog_url,sizeof catalog_url)==ESP_OK);
    assert(!strcmp(catalog_url,"https://example.test/catalog.json"));
    fixture::response=R"({"schema":1,"layout":3,"board":"SDGOODS-ESP32S3","apps":[{"app_id":"CLOCK","name":"Clock","version":"1.2.3","image_format":"sdgoods","size":1024,"sha256":"0000000000000000000000000000000000000000000000000000000000000000","url":"/firmware/clock.bin"}]})";
    CatalogApp entries[8];size_t count=0;
    assert(FetchCatalog(entries,8,&count)==ESP_OK && count==1);
    assert(entries[0].format==ImageFormat::kSdgoods);
    assert(!strcmp(entries[0].url,"https://example.test/firmware/clock.bin"));
    // The backend's explicit format does not bypass native board/layout checks.
    auto bytes=Image("agent_link");
    Descriptor native{{'A','G','S','Y','S','0','0','1'},1,2,{},{},{},{}};
    snprintf(native.app_id,sizeof native.app_id,"agent.test");
    snprintf(native.board,sizeof native.board,"%s",kBoard);
    snprintf(native.sdk,sizeof native.sdk,"0.1.0");
    memcpy(bytes.data()+0x120,&native,sizeof native);
    auto app=Download("agent.test",bytes);
    assert(Install(app)==ESP_ERR_INVALID_VERSION);
    App apps[2];assert(List(apps,2,&count)==ESP_OK && count==0);
    bytes=Image("CLOCK");bytes[12]=5;
    app=Download("CLOCK",bytes);assert(Install(app)!=ESP_OK);
    app=Download("SDGOODS_LAUNCHER",Image("SDGOODS_LAUNCHER"));
    assert(Install(app)==ESP_ERR_INVALID_VERSION);
    assert(List(apps,2,&count)==ESP_OK && count==0);
    // Native catalog entries cannot silently accept unwrapped vendor binaries.
    app=Download("CLOCK",Image("CLOCK"));app.format=ImageFormat::kAgentLink;
    assert(Install(app)==ESP_ERR_INVALID_VERSION);
    app=Download("CLOCK",Image("CLOCK"));
    app.size=kAppImageMaxBytes+1;const auto opens=fixture::opens;
    assert(Install(app)==ESP_ERR_INVALID_ARG && fixture::opens==opens);
    app=Download("CLOCK",Image("CLOCK"));fixture::fail_after=600;
    assert(Install(app)!=ESP_OK);fixture::fail_after=SIZE_MAX;
    assert(List(apps,2,&count)==ESP_OK && count==0);
    app=Download("CLOCK",Image("CLOCK"));app.sha256[0]=app.sha256[0]=='0'?'1':'0';
    assert(Install(app)==ESP_ERR_INVALID_CRC);
    assert(List(apps,2,&count)==ESP_OK && count==0);
    app=Download("CLOCK",Image("CLOCK"));fixture::end_error=ESP_ERR_INVALID_CRC;
    assert(Install(app)==ESP_ERR_INVALID_CRC);fixture::end_error=ESP_OK;
    assert(List(apps,2,&count)==ESP_OK && count==0);
    struct Cancellation { bool requested = false; InstallStage stage; };
    const auto cancel = +[](void* context) { return static_cast<Cancellation*>(context)->requested; };
    const auto observe = +[](const InstallStatus& status, void* context) {
        auto& cancellation = *static_cast<Cancellation*>(context);
        if (status.stage == cancellation.stage &&
            (status.stage != InstallStage::kDownload || status.received > 0)) cancellation.requested = true;
    };
    for (const auto phase : {InstallStage::kDownload, InstallStage::kPublish}) {
        Cancellation cancellation{false, phase};
        InstallStatus result;
        assert(Install(app, nullptr, &cancellation, &result, observe, cancel) == kCancelled);
        assert(result.error == kCancelled && result.stage == phase && result.received > 0);
        assert(List(apps,2,&count)==ESP_OK && count==0 && fixture::boot==nullptr);
    }
    assert(Install(app)==ESP_OK && Revision()==1);
    assert(List(apps,2,&count)==ESP_OK && count==1);
    assert(apps[0].format==ImageFormat::kSdgoods && !strcmp(apps[0].descriptor.app_id,"CLOCK"));
    assert(fixture::boot==nullptr && fixture::restarts==0);
    assert(Install(app)==kAlreadyInstalled);
    app=Download("PHOTO",Image("PHOTO"));assert(Install(app)==ESP_OK);
    app=Download("NOTES",Image("NOTES"));assert(Install(app)==kSlotsFull);
    assert(Launch("ota_1")==ESP_OK && fixture::boot==&fixture::partitions[2]);
    std::vector<uint8_t> cover(kCoverBytes,0);
    CoverHeader h{};memcpy(h.magic,"SDGCOVER",8);
    h.version=1;h.format=1;h.width=h.height=132;h.data_len=kCoverPixelBytes;
    snprintf(h.app_id,sizeof h.app_id,"CLOCK");
    cover[64]=0xF8;cover[65]=0;cover[64+132*132*2]=0xA7;
    h.crc32=Crc(cover.data()+64,kCoverPixelBytes);memcpy(cover.data(),&h,sizeof h);
    const auto& part=fixture::partitions[1];
    memcpy(fixture::flash[part.address].data()+part.size-kCoverReserveBytes,cover.data(),cover.size());
    Cover token;assert(ReadCover("ota_0",&token)==ESP_OK);
    auto pixels=AcquireCover(token);assert(pixels && pixels->Data()[0]==0 && pixels->Data()[1]==0xF8);
    assert(pixels->Data()[132*132*2]==0xA7);
    cover_cache::Forget("ota_0");
    fixture::flash[part.address][part.size-kCoverReserveBytes+64]^=1;
    assert(ReadCover("ota_0",&token)==ESP_ERR_INVALID_CRC);
    assert(Remove("ota_0")==ESP_OK);
    app=Download("PHOTO",Image("PHOTO"));const auto revision=Revision();
    assert(Install(app)==kAlreadyInstalled && Revision()==revision);
    assert(fixture::store.at("agent_nvs/al_dev/auth_key")==saved.at("agent_nvs/al_dev/auth_key"));
    puts("SDGOODS: reviewed backend catalog, raw download, native/layout/chip rejection, interruption, SHA, duplicates, slot bounds, launch, covers and isolated settings passed.");
}
