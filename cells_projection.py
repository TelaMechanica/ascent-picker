"""Read-only configuration geometry; no PWM classifier or switching state.

The receiver runtime owns ordinary selection. These values are draft/UI helpers.
"""
from __future__ import annotations
PWM_LO = 1000
PWM_HI = 2000
MIN_ACTIVE = 2

def active_valid_indices(valid_flags) -> list[int]:
    """Indices of configured cells that are valid (BbId in this VRX T2), in configured
    (low->high PWM) order. `valid_flags` is an iterable of truthy/falsey per configured cell."""
    return [i for i, v in enumerate(valid_flags) if v]

def picker_active(active_valid_n: int) -> bool:
    return active_valid_n >= MIN_ACTIVE

def center_us(i: int, n: int) -> int:
    """Recommended EdgeTX center for 1-based cell i over n active cells, rounded.
    center_i = 1000 + (1000/N)*(i-0.5); integer form 1000 + (1000*(2i-1)+N)//(2N)."""
    if n < 1 or i < 1 or i > n:
        return 0
    return 1000 + (1000 * (2 * i - 1) + n) // (2 * n)

def lower_us(i: int, n: int) -> int:
    if n < 1 or i < 1 or i > n:
        return 0
    return 1000 + ((i - 1) * 1000 + n // 2) // n

def upper_us(i: int, n: int) -> int:
    if n < 1 or i < 1 or i > n:
        return 0
    return 1000 + (i * 1000 + n // 2) // n

def centers(n: int) -> list[int]:
    """All recommended centers for an N-cell layout (for the UI table)."""
    return [center_us(i, n) for i in range(1, n + 1)]
