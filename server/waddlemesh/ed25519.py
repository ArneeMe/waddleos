"""Ed25519 signatures (RFC 8032) in plain Python, for the admin's signed roster.

Slow (a few milliseconds) but needs nothing installed. tools/waddle-mesh has
the same code, so the tool runs on its own.
"""

import hashlib

_Q = 2**255 - 19
_L = 2**252 + 27742317777372353535851937790883648493
_D = -121665 * pow(121666, _Q - 2, _Q) % _Q
_SQRT_M1 = pow(2, (_Q - 1) // 4, _Q)


def _add(p, q):
    """Point addition in extended coordinates (complete, so it doubles too)."""
    x1, y1, z1, t1 = p
    x2, y2, z2, t2 = q
    a = (y1 - x1) * (y2 - x2) % _Q
    b = (y1 + x1) * (y2 + x2) % _Q
    c = 2 * t1 * t2 * _D % _Q
    d = 2 * z1 * z2 % _Q
    e, f, g, h = b - a, d - c, d + c, b + a
    return e * f % _Q, g * h % _Q, f * g % _Q, e * h % _Q


def _mul(s, p):
    out = (0, 1, 1, 0)
    while s:
        if s & 1:
            out = _add(out, p)
        p = _add(p, p)
        s >>= 1
    return out


def _x_from_y(y, sign):
    if y >= _Q:
        return None
    x2 = (y * y - 1) * pow(_D * y * y + 1, _Q - 2, _Q) % _Q
    if x2 == 0:
        return None if sign else 0
    x = pow(x2, (_Q + 3) // 8, _Q)
    if (x * x - x2) % _Q:
        x = x * _SQRT_M1 % _Q
    if (x * x - x2) % _Q:
        return None
    return _Q - x if (x & 1) != sign else x


_GY = 4 * pow(5, _Q - 2, _Q) % _Q
_GX = _x_from_y(_GY, 0)
_G = (_GX, _GY, 1, _GX * _GY % _Q)


def _encode(p):
    x, y, z, _ = p
    zi = pow(z, _Q - 2, _Q)
    x, y = x * zi % _Q, y * zi % _Q
    return (y | ((x & 1) << 255)).to_bytes(32, "little")


def _decode(b):
    y = int.from_bytes(b, "little")
    sign, y = y >> 255, y & ((1 << 255) - 1)
    x = _x_from_y(y, sign)
    return None if x is None else (x, y, 1, x * y % _Q)


def _equal(p, q):
    return (p[0] * q[2] - q[0] * p[2]) % _Q == 0 and (p[1] * q[2] - q[1] * p[2]) % _Q == 0


def _h(*parts):
    return int.from_bytes(hashlib.sha512(b"".join(parts)).digest(), "little")


def _expand(seed):
    h = hashlib.sha512(seed).digest()
    a = int.from_bytes(h[:32], "little")
    a = (a & ((1 << 254) - 8)) | (1 << 254)
    return a, h[32:]


def public_key(seed):
    """The 32-byte public key of a 32-byte secret seed."""
    return _encode(_mul(_expand(seed)[0], _G))


def sign(seed, message):
    a, prefix = _expand(seed)
    public = _encode(_mul(a, _G))
    r = _h(prefix, message) % _L
    big_r = _encode(_mul(r, _G))
    s = (r + _h(big_r, public, message) * a) % _L
    return big_r + s.to_bytes(32, "little")


def verify(public, message, signature):
    if len(public) != 32 or len(signature) != 64:
        return False
    a = _decode(public)
    r = _decode(signature[:32])
    s = int.from_bytes(signature[32:], "little")
    if a is None or r is None or s >= _L:
        return False
    k = _h(signature[:32], public, message) % _L
    return _equal(_mul(s, _G), _add(r, _mul(k, a)))
