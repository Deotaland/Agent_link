#include "dynamic_layout_config.h"
#include "bootloader_flash_priv.h"
#include "bootloader_utility.h"
#include "esp_rom_md5.h"
#include "esp_log.h"

const alpt_boot_marker agent_pt_guard_marker
    __attribute__((used, aligned(4))) = {"ALPTGUARD01", 1, ALPT_POLICY_ID};
static uint8_t scratch[ALPT_TABLE_BYTES] __attribute__((aligned(4)));
static int read_flash(void* context, uint32_t offset, void* out, size_t size) {
    return bootloader_flash_read(offset, out, size, true);
}
static int erase_flash(void* context, uint32_t offset, size_t size) {
    return bootloader_flash_erase_range(offset, size);
}
static int write_flash(void* context, uint32_t offset, const void* bytes, size_t size) {
    return bootloader_flash_write(offset, (void*)bytes, size, false);
}
static int md5(const void* bytes, size_t size, uint8_t out[16]) {
    md5_context_t context;
    esp_rom_md5_init(&context);
    esp_rom_md5_update(&context, bytes, size);
    esp_rom_md5_final(out, &context);
    return 0;
}
void bootloader_after_init(void) {
    const alpt_io io = {NULL, read_flash, erase_flash, write_flash, md5};
    const int result = alpt_restore(&alpt_config, &io, scratch);
    if (result < 0) {
        ESP_LOGE("agent_pt_guard", "Partition recovery failed (%d); refusing an unchecked boot",
                 result);
        bootloader_reset();
    }
}
