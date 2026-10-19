#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include "cst820_touch.h"
#include "m5ioe1.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
struct FakeI2cDevice { uint16_t addr; };
FakeI2cDevice touch{0x15}, expander{0x4F};
uint8_t touch_regs[256]={}, ioe_regs[256]={};
bool bus_present=true, touch_present=true, read_failure=false, write_failure=false, supply_failure=false;
int removed=0, reads=0;
unsigned delayed_ms=0;
std::vector<int> speaker_steps;
int i2c_master_get_bus_handle(int,void** out) { *out=reinterpret_cast<void*>(1); return bus_present ? ESP_OK : ESP_FAIL; }
int i2c_master_probe(void*,uint16_t addr,int timeout) { assert(timeout==50); return addr==expander.addr ? ESP_OK : ESP_FAIL; }
int i2c_master_bus_add_device(void*,const i2c_device_config_t* c,FakeI2cDevice** out) {
    assert(c->dev_addr_length==7 && c->scl_speed_hz==100000);
    if(c->device_address==0x15 && touch_present) { *out=&touch; return ESP_OK; }
    if(c->device_address==expander.addr) { *out=&expander; return ESP_OK; }
    return ESP_FAIL;
}
int i2c_master_bus_rm_device(FakeI2cDevice*) { ++removed; return ESP_OK; }
int i2c_master_transmit_receive(FakeI2cDevice* d,const uint8_t* reg,size_t tx,uint8_t* bytes,size_t rx,int timeout) {
    ++reads; assert(tx==1 && timeout>0 && timeout<=50);
    if(read_failure) return ESP_FAIL;
    if(d==&touch) {
        assert((*reg==0xA7 && rx==1)||(*reg==0xA9 && rx==1)||(*reg==0 && rx==7));
        memcpy(bytes,touch_regs+*reg,rx);
    } else {
        if(*reg==7) { ioe_regs[7]=ioe_regs[5]; ioe_regs[8]=ioe_regs[6]; if(supply_failure) ioe_regs[7]&=~0x80; }
        memcpy(bytes,ioe_regs+*reg,rx);
    }
    return ESP_OK;
}
int i2c_master_transmit(FakeI2cDevice* d,const uint8_t* bytes,size_t size,int timeout) {
    assert(d==&expander && timeout==50 && size>=2 && size<=3);
    if(write_failure) return ESP_FAIL;
    if(bytes[0]==5) speaker_steps.push_back(2);
    memcpy(ioe_regs+bytes[0],bytes+1,size-1); return ESP_OK;
}
constexpr int SYS_I2C_PORT=0,IOE_ADDR=0x4F,IOE_MUX_CTR=1,IOE_AUDIO_EN=3,IOE_SPK_EN=10;
constexpr int IOE_PANEL_PWR=8,IOE_TP_RST=4,IOE_PANEL_RST=5;
using gpio_num_t=int;
constexpr int GPIO_NUM_14=14,GPIO_MODE_INPUT=1,GPIO_MODE_OUTPUT=2;
int gpio_set_direction(int pin,int mode) { assert(pin==14); speaker_steps.push_back(mode==GPIO_MODE_INPUT?1:4); return ESP_OK; }
int gpio_set_level(int pin,bool) { assert(pin==14); speaker_steps.push_back(3); return ESP_OK; }
class M5StopWatchHardware {
public:
    M5Ioe1 ioe_; bool i2c_ok_=true,ioe_ok_=false;
    void InitExpander();
    int SetSpeakerEnabled(bool);
};
// INSERT_EXPANDER_METHOD
// INSERT_SPEAKER_METHOD
int main() {
    bus_present=false; Cst820Touch missing_bus; assert(missing_bus.Init(0,466,466)==ESP_FAIL);
    bus_present=true; touch_present=false; Cst820Touch missing_device; assert(missing_device.Init(0,466,466)==ESP_FAIL);
    touch_present=true; Cst820Touch invalid_id; int before=reads;
    assert(invalid_id.Init(0,466,466)!=ESP_OK && !invalid_id.Ready() && reads==before+10 && removed==1);
    touch_regs[0xA7]=0xB5; touch_regs[0xA9]=1;
    Cst820Touch valid; assert(valid.Init(0,466,466)==ESP_OK && valid.Ready());
    assert(valid.Init(0,466,466)==ESP_ERR_INVALID_STATE);
    uint8_t frame[]={0,0,1,0,137,1,94}; memcpy(touch_regs,frame,7);
    cst820::Point p; assert(valid.Poll(&p)==ESP_OK && p.pressed && p.x==137 && p.y==350);
    read_failure=true; assert(valid.Poll(&p)==ESP_FAIL && !p.pressed); read_failure=false;
    assert(valid.Poll(&p)==ESP_OK && p.pressed); touch_regs[4]=255; touch_regs[3]=15;
    assert(valid.Poll(&p)==ESP_ERR_INVALID_RESPONSE && !p.pressed);
    puts("PASS actual CST820 driver: address/speed/registers, bounded ID retries, bus/device/read failure and recovery");
    for(auto addr:{0x4F,0x6F}) {
        memset(ioe_regs,0,sizeof ioe_regs); expander.addr=static_cast<uint16_t>(addr);
        M5StopWatchHardware hardware; hardware.InitExpander(); assert(hardware.ioe_ok_);
        assert(ioe_regs[0x23]==0 && (ioe_regs[5]&0x98)==0x98); // panel power, touch/panel reset high
        assert((ioe_regs[6]&2)==0); // speaker initially low
        speaker_steps.clear(); assert(hardware.SetSpeakerEnabled(true)==ESP_OK);
        assert((speaker_steps==std::vector<int>{1,2,3,4}) && (ioe_regs[6]&2));
        speaker_steps.clear(); assert(hardware.SetSpeakerEnabled(false)==ESP_OK);
        assert((speaker_steps==std::vector<int>{1,2,3,4}) && !(ioe_regs[6]&2));
        write_failure=true; speaker_steps.clear(); assert(hardware.SetSpeakerEnabled(true)!=ESP_OK);
        assert((speaker_steps==std::vector<int>{1})); write_failure=false; // no conflicting GPIO output on failure
    }
    supply_failure=true; unsigned start=delayed_ms;
    M5StopWatchHardware no_supply; no_supply.InitExpander(); assert(!no_supply.ioe_ok_ && delayed_ms-start<1000);
    puts("PASS actual IOE/hardware: both addresses, power/readback/reset sequence, bounded supply failure and speaker pin ordering");
}
