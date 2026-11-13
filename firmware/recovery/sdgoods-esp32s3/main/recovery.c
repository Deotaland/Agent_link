#include "dynamic_layout_config.h"
#include "agent_launcher_boot.h"
#include "driver/gpio.h"
#include "esp_flash.h"
#include "esp_image_format.h"
#include "esp_ota_ops.h"
#include "esp_rom_md5.h"
#include "esp_system.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Keep the vendor-compatible identity and ABI in this immutable routing image. */
const struct {
    char magic[8];
    uint32_t schema, layout;
    char id[32], name[64], board[48], sdk[16];
} agent_recovery_identity __attribute__((used, section(".rodata_custom_desc"), aligned(4))) = {
    {'A', 'G', 'S', 'Y', 'S', '0', '0', '1'},
    1,
    3,
    "agent.launcher",
    "Agent_link Recovery",
    "SDGOODS-ESP32S3",
    "0.1.0"};
static uint8_t table[4096], scratch[4096];
static int read_flash(void* ctx, uint32_t at, void* out, size_t bytes) {
    return esp_flash_read(esp_flash_default_chip, out, at, bytes);
}
static int erase_flash(void* ctx, uint32_t at, size_t bytes) {
    return esp_flash_erase_region(esp_flash_default_chip, at, bytes);
}
static int write_flash(void* ctx, uint32_t at, const void* in, size_t bytes) {
    return esp_flash_write(esp_flash_default_chip, in, at, bytes);
}
static int md5(const void* bytes, size_t size, uint8_t out[16]) {
    md5_context_t ctx;
    esp_rom_md5_init(&ctx);
    esp_rom_md5_update(&ctx, bytes, size);
    esp_rom_md5_final(out, &ctx);
    return 0;
}
static const alpt_io io = {NULL, read_flash, erase_flash, write_flash, md5};
static const esp_partition_t* launcher(unsigned index) {
    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0 + 14 + index, NULL);
    alboot_capability cap;
    esp_image_metadata_t metadata;
    if (!part || part->address != (index ? ALBOOT_B_OFFSET : ALBOOT_A_OFFSET) ||
        part->size != ALBOOT_IMAGE_BYTES || esp_partition_read(part, 0x1E4, &cap, sizeof cap) ||
        memcmp(cap.magic, "ALAB0001", 8) || cap.policy != ALPT_POLICY_ID)
        return NULL;
    const esp_partition_pos_t position = {part->address, part->size};
    return esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &position, &metadata) == ESP_OK ? part : NULL;
}
void app_main(void) {
    gpio_hold_dis(GPIO_NUM_7);
    gpio_deep_sleep_hold_dis();
    gpio_set_direction(GPIO_NUM_7, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_7, 1);
    gpio_config_t key = {
        .pin_bit_mask = 1ULL << 6, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    gpio_config(&key);
    if (read_flash(NULL, alpt_config.table, table, sizeof table) ||
        !alpt_validate(&alpt_config, &io, table, 1))
        goto failed;
    if (!alpt_validate(&alpt_config, &io, table, 0)) {
        alpt_entry slots[16];
        for (unsigned i = 0; i < 16; ++i)
            alpt_empty(&alpt_config, i, &slots[i]);
        /* Initial combined USB packages may contain one verified application. */
        const esp_partition_t* seed =
            esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
        esp_image_metadata_t image;
        const esp_partition_pos_t pool = {alpt_config.pool_start,
                                          alpt_config.pool_end - alpt_config.pool_start};
        if (seed && esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pool, &image) == ESP_OK &&
            image.image_len <= pool.size - ALPT_COVER_RESERVE_BYTES) {
            slots[0].offset = pool.offset;
            slots[0].size = (image.image_len + ALPT_COVER_RESERVE_BYTES + 0xFFFFu) & ~0xFFFFu;
        }
        if (alpt_build(&alpt_config, &io, slots, table) ||
            alpt_commit(&alpt_config, &io, table, scratch))
            goto failed;
        esp_restart(); /* Reload the IDF partition cache before selecting A. */
    }
    alboot_record state;
    int bank;
    if (alboot_load(&io, ALBOOT_STATE_OFFSET, ALPT_POLICY_ID, &state, &bank) < 0)
        goto failed;
    unsigned target = state.pending < 2 ? state.pending : state.active;
    /* Holding the recovery key can select the previously confirmed Launcher. */
    if (!gpio_get_level(GPIO_NUM_6) && state.pending == ALBOOT_NO_SLOT &&
        (state.valid & (1u << (1u - state.active))) && launcher(1u - state.active)) {
        target = 1u - state.active;
        state.active = target;
        if (alboot_save(&io, ALBOOT_STATE_OFFSET, ALPT_POLICY_ID, &state))
            goto failed;
    }
    const esp_partition_t* part = launcher(target);
    if (!part && state.pending < 2) {
        state.valid &= (uint8_t)~(1u << state.pending);
        state.pending = ALBOOT_NO_SLOT;
        state.attempted = 0;
        state.result = ALBOOT_ROLLBACK;
        if (alboot_save(&io, ALBOOT_STATE_OFFSET, ALPT_POLICY_ID, &state))
            goto failed;
        target = state.active;
        part = launcher(target);
    }
    if (!part && (state.valid & (1u << (1u - target)))) {
        target = 1u - target;
        part = launcher(target);
        if (part) {
            state.active = target;
            if (alboot_save(&io, ALBOOT_STATE_OFFSET, ALPT_POLICY_ID, &state))
                goto failed;
        }
    }
    if (part && esp_ota_set_boot_partition(part) == ESP_OK)
        esp_restart();
failed:
    ESP_LOGE("launcher.recovery",
             "No verified Launcher. Flash the matching full USB package; data is preserved.");
    while (1)
        vTaskDelay(pdMS_TO_TICKS(1000));
}
