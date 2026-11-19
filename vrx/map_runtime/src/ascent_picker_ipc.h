/* ascent_picker_ipc.h — Picker runtime <-> ASCENT Control IPC: command parse + status format.
 * PURE + host-testable (the loopback socket accept/recv is the device adapter around this).
 * Status is RENDERED FROM the controller's own state -- no parallel state is kept here.
 *
 * Wire (newline-terminated text, loopback only):
 *   command:  "SELECT BBID <four-byte-hex-id>"  |  "SELECT CELL <n>"  |  "STATUS"
 *   status:   "STATUS enabled=<0|1> active=<N> cur=<i|-1> target=<<four-byte-hex-id>|-> "
 *             "outcome=<n> phase=<n> reason=<n>"
 */
#ifndef ASCENT_PICKER_IPC_H
#define ASCENT_PICKER_IPC_H
#include "ascent_types.h"
#include "ascent_picker_ctl.h"

typedef enum { A_PIPC_BAD = 0, A_PIPC_SELECT_BBID, A_PIPC_SELECT_CELL, A_PIPC_STATUS, A_PIPC_QUIESCE } a_pipc_kind;

typedef struct {
    a_pipc_kind kind;
    au8 bbid[4];     /* for SELECT_BBID */
    int cell;        /* for SELECT_CELL (0-based) */
} a_pipc_cmd;

/* Parse one command line. Returns 1 and fills *out on a recognized command, 0 otherwise. */
int a_pipc_parse(const char *line, ausize len, a_pipc_cmd *out);

/* Render the controller's state as a status line (NUL-terminated). Returns the string length. */
int a_pipc_format_status(const a_pctl *c, char *out, int cap);

#endif
