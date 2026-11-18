#include "omnia_at32.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    omnia_spi_flash_info_t info;
    memset(&info, 0xA5, sizeof info);
    omnia_ext_flash_init(NULL);
    omnia_ext_flash_init(&info);
    assert(!info.present && info.bus_type == 0 && info.capacity_bytes == 0 && info.jedec_id == 0);
    assert(!omnia_ext_flash_get_info()->present);
    omnia_dsp_ctx_t dsp = { .sentinel = 0xA55AC33CU };
    char name[24]; memset(name, 'X', sizeof name);
    for (unsigned i = 0; i <= UINT8_MAX; ++i) {
        assert(!omnia_preset_save((uint8_t)i, "test", &dsp));
        assert(!omnia_preset_load((uint8_t)i, &dsp, name));
    }
    assert(!omnia_preset_save(0, NULL, NULL));
    assert(!omnia_preset_load(0, NULL, NULL));
    assert(dsp.sentinel == 0xA55AC33CU && name[0] == 'X');
    puts("[OK] AT32 flash/preset API fail closed");
    return 0;
}
