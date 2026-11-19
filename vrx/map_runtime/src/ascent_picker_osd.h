/* ascent_picker_osd.h — render the on-goggles Picker bar control text FROM controller state.
 * PURE + host-testable; the daemon writes the returned text to the overlay control file, which
 * the existing ascent_pickerbar renderer consumes. Addendum §8: when the Picker is not active
 * (< 2 active-valid) the bar is HIDDEN ("show 0"), never a one/zero-aircraft bar. The selected
 * cell is marked active; labels come from the deployed config. No parallel state. */
#ifndef ASCENT_PICKER_OSD_H
#define ASCENT_PICKER_OSD_H
#include "ascent_types.h"
#include "ascent_picker_ctl.h"
#include "ascent_picker_cfg.h"

/* Writes the pickerbar control text (NUL-terminated) for the current controller + config state.
 * Returns the length. Only the active-valid cells are drawn, in order; the current cell is
 * marked active. Disabled => "show 0\n" (hidden). */
int a_posd_format(const a_pctl *c, const a_pcfg *cfg, char *out, int cap);

#endif
