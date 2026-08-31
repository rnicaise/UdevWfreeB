#ifndef UWB_PERSISTENT_SETTINGS_H
#define UWB_PERSISTENT_SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

#define UWB_SETTINGS_ARM_NONE          0u
#define UWB_SETTINGS_ARM_DISTANCE_2M   1u
#define UWB_SETTINGS_ARM_TILT_50       2u

#define UWB_SETTINGS_NAME_MAX          15u
#define UWB_SETTINGS_NAME_DEFAULT      "UWB"

typedef struct
{
    uint32_t arm_mode;
    uint32_t seq;
    char name[UWB_SETTINGS_NAME_MAX + 1u];
} uwb_persistent_settings_t;

void uwb_persistent_settings_init(void);
uwb_persistent_settings_t uwb_persistent_settings_get(void);
bool uwb_persistent_settings_write_arm(uint32_t arm_mode);
bool uwb_persistent_settings_write_name(const char *name);
bool uwb_persistent_settings_name_is_valid(const char *name);
bool uwb_persistent_settings_write_pending(void);
bool uwb_persistent_settings_consume_write_success(void);
bool uwb_persistent_settings_consume_write_error(void);

#endif
