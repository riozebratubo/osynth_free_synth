#include "local_ui.h"

#include "esp_log.h"
#include "synth_config.h"

static const char* TAG = "local_ui";

esp_err_t local_ui_init(void) {
#if SYNTH_ENABLE_LOCAL_UI
    ESP_LOGW(TAG, "enabled, but hardware bring-up is future work — stub "
                  "(interface notes in local_ui.h)");
#else
    ESP_LOGI(TAG, "disabled via CONFIG_OSYNTH_ENABLE_LOCAL_UI (default)");
#endif
    return ESP_OK;
}
