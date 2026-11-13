#include "dynamic_layout_config.h"
#include "bootloader_flash_priv.h"
#include "bootloader_utility.h"
#include "esp_rom_md5.h"
#include "esp_log.h"
#ifdef AGENT_LAUNCHER_AB
#include "agent_launcher_boot.h"
static bootloader_state_t launcher_boot;
static esp_image_metadata_t launcher_image;
static void launcher_trial(const alpt_io* io) {
    if (!bootloader_utility_load_partition_table(&launcher_boot))
        bootloader_reset();
    const int selected = bootloader_utility_get_selected_boot_partition(&launcher_boot);
    if (selected != 14 && selected != 15)
        return;
    alboot_record state;
    int bank;
    if (alboot_load(io, ALBOOT_STATE_OFFSET, ALPT_POLICY_ID, &state, &bank) < 0)
        bootloader_reset();
    const unsigned slot = (unsigned)(selected - 14);
    const esp_partition_pos_t position = {slot ? ALBOOT_B_OFFSET : ALBOOT_A_OFFSET,
                                          ALBOOT_IMAGE_BYTES};
    alboot_capability capability = {0};
    int reject = io->read(NULL, position.offset + 0x1E4, &capability, sizeof capability) ||
                 memcmp(capability.magic, "ALAB0001", 8) || capability.policy != ALPT_POLICY_ID ||
                 esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &position, &launcher_image) != ESP_OK;
    if (state.pending != slot && !(state.valid & (1u << slot)))
        reject = 1;
    if (state.pending == slot) {
        if (capability.version_code != state.versions[slot])
            reject = 1;
        reject |= alboot_attempt(&state, slot);
        if (reject && state.pending != ALBOOT_NO_SLOT) {
            state.valid &= (uint8_t)~(1u << slot);
            state.pending = ALBOOT_NO_SLOT;
            state.attempted = 0;
            state.result = ALBOOT_ROLLBACK;
        }
        if (alboot_save(io, ALBOOT_STATE_OFFSET, ALPT_POLICY_ID, &state) != ALPT_OK)
            reject = 1;
    }
    if (reject) {
        ESP_LOGW("launcher_boot", "Launcher %c rejected; entering recovery", 'A' + slot);
        /* Only Launcher selections are affected; vendor application OTA state is untouched. */
        if (io->erase(NULL, launcher_boot.ota_info.offset, launcher_boot.ota_info.size))
            bootloader_reset();
    }
}
#endif

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
#ifdef AGENT_LAUNCHER_AB
    launcher_trial(&io);
#endif
}
