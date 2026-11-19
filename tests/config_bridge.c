/* Pure parser fixture only: no receiver process, RF command or transport. */
#include "ascent_picker_cfg.h"
int fixture_config_field(const char *text, ausize size, int field) {
    a_pcfg cfg;
    a_pcfg_parse(text, size, &cfg);
    if (!cfg.ok) return -1;
    switch (field) {
    case 0: return cfg.n;
    case 1: return (int)cfg.hold_ms;
    case 2: return cfg.rc_channel;
    case 3: return cfg.rc_enabled;
    case 4: return cfg.display_hidden;
    default: return -1;
    }
}
int fixture_config_id(const char *text, ausize size, int cell, int byte) {
    a_pcfg cfg;
    a_pcfg_parse(text, size, &cfg);
    if (!cfg.ok || cell < 0 || cell >= cfg.n || byte < 0 || byte > 3) return -1;
    return cfg.cells[cell].bbid[byte];
}
