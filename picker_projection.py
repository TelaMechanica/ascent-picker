"""Host draft projection/serialization; no receiver I/O or ordinary switch owner.

The caller supplies this receiver's established membership. Enabled is the
configuration intent, not an observation of installed/running receiver state.
Host naming data remains with its canonical owner; only supplied aircraft
presentation labels are serialized. No credential or membership store is added.
"""
from __future__ import annotations
import cells_projection as pc
SCHEMA = 'ascent.picker.rt.v1'
_BB_SENTINELS = {':'.join([value] * 4) for value in ('00', 'FF')}

def _norm(bb) -> str | None:
    if not isinstance(bb, str):
        return None
    s = bb.strip().upper().replace('-', ':')
    parts = s.split(':')
    if len(parts) != 4:
        return None
    try:
        for p in parts:
            if len(p) != 2:
                return None
            int(p, 16)
    except ValueError:
        return None
    if s in _BB_SENTINELS:
        return None
    return s

def build_projection(cells, t2_bbids) -> dict:
    """cells: ordered list of {bb_id, nickname?, glyph?} (configured, low->high PWM order).
    t2_bbids: iterable of BbIds in this VRX's T2 candidate library.
    Returns the full projection for the UI (every configured cell annotated) plus the
    active-valid summary and recommended centers."""
    t2 = set()
    for b in t2_bbids or []:
        n = _norm(b)
        if n:
            t2.add(n)
    annotated = []
    valid_flags = []
    for i, c in enumerate(cells or []):
        bb = _norm((c or {}).get('bb_id'))
        valid = bool(bb and bb in t2)
        valid_flags.append(valid)
        annotated.append({'slot': i, 'bb_id': bb, 'nickname': (c or {}).get('nickname'), 'glyph': (c or {}).get('glyph'), 'valid': valid})
    active_idx = pc.active_valid_indices(valid_flags)
    n = len(active_idx)
    enabled = pc.picker_active(n)
    configured_count = len(annotated)
    active_order = {slot: order for order, slot in enumerate(active_idx)}
    for slot, cell in enumerate(annotated):
        cell['active_index'] = active_order.get(slot)
        cell['center_us'] = pc.center_us(slot + 1, configured_count)
        cell['lower_us'] = pc.lower_us(slot + 1, configured_count)
        cell['upper_us'] = pc.upper_us(slot + 1, configured_count)
        if not cell['valid']:
            cell['unavailable_reason'] = 'NOT IN THIS RECEIVER FLEET'
    reason = None
    if not enabled:
        reason = 'PICKER DISABLED — NEED AT LEAST 2 VALID VTX' if cells else 'PICKER DISABLED — NO AIRCRAFT CONFIGURED'
    return {'schema': SCHEMA, 'enabled': enabled, 'active_valid_count': n, 'min_active': pc.MIN_ACTIVE, 'reason': reason, 'cells': annotated, 'configured_count': configured_count, 'centers': pc.centers(configured_count)}

def deployable_config(projection: dict) -> dict:
    """Summarize active targets; serialization retains empty configured slots."""
    active = [c for c in projection.get('cells', []) if c.get('valid')]
    active.sort(key=lambda c: c['active_index'])
    return {'schema': projection.get('schema', SCHEMA), 'enabled': bool(projection.get('enabled')), 'active_valid_count': len(active), 'cells': [{'order': c['active_index'], 'bb_id': c['bb_id'], 'label': c.get('nickname') or c['bb_id'], 'glyph': c.get('glyph')} for c in active]}

def serialize_config(projection: dict, *, hold_ms=120, cooldown_ms=0, rc_channel=5, rc_enabled=False, theme='dark', icon_style='vector', icon_scale=100, visible=True) -> str:
    """Exact freestanding parser wire format; labels cannot inject directives."""
    hold_ms, rc_channel = (int(hold_ms), int(rc_channel))
    if not 0 <= hold_ms <= 5000 or not 1 <= rc_channel <= 16:
        raise ValueError('invalid Picker hold/channel')
    lines = ['ascent_picker_rt v1', f'hold_ms {hold_ms}', f'rc_channel {rc_channel}', f'rc_enabled {int(bool(rc_enabled))}']
    cooldown_ms = int(cooldown_ms)
    if not 0 <= cooldown_ms <= 300000:
        raise ValueError('invalid Picker advisory cooldown')
    lines.append(f'cooldown_ms {cooldown_ms}')
    import re
    if theme not in ('dark', 'light') or not re.fullmatch('[a-z][a-z0-9_-]{0,23}', icon_style):
        raise ValueError('invalid Picker display style')
    icon_scale = int(icon_scale)
    if not 25 <= icon_scale <= 400:
        raise ValueError('invalid Picker icon scale')
    lines += [f'iconscale {icon_scale}', f'theme {int(theme == 'light')}', f'iconstyle {icon_style}']
    if not isinstance(visible, bool):
        raise ValueError('invalid Picker display visibility/count')
    lines.append(f'display_hidden {int(not visible)}')
    cells = projection.get('cells', [])
    if len(cells) > 8:
        raise ValueError('invalid Picker configured cell count')
    seen = set()
    for c in cells:
        if not c.get('valid'):
            lines.append('empty')
            continue
        bb = _norm(c['bb_id'])
        if bb is None or bb in seen or len(seen) >= 8:
            raise ValueError('invalid BBID')
        seen.add(bb)
        label = ''.join((ch if 32 <= ord(ch) <= 126 else ' ' for ch in (c.get('nickname') or bb)))[:23].strip()
        lines.append(f'cell {bb} {label or bb}')
        glyph = c.get('glyph') or '-'
        if not re.fullmatch('[a-z-]{1,23}', glyph):
            raise ValueError('invalid Picker glyph')
        lines.append(f'glyph {bb} {glyph}')
    return '\n'.join(lines) + '\n'
