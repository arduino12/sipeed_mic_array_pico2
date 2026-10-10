#pragma once
// Mic positions in the array frame: x to the right, y towards mic 0, looking
// at the LED side. Azimuth az is clockwise from mic 0, so the unit vector
// towards a source at az is (sin az, cos az).
#include <math.h>

#include "config.h"

namespace geom {

constexpr float DEG = (float)M_PI / 180.0f;

inline void mic_xy(int k, float &x, float &y) {
    if (k >= 6) {
        x = y = 0.0f;  // centre mic
        return;
    }
    float a = 60.0f * k * DEG;
    x = MIC_RADIUS_M * sinf(a);
    y = MIC_RADIUS_M * cosf(a);
}

// Projection of mic k's position onto the direction az (metres). A plane
// wave from az reaches mic k proj/c seconds *earlier* than the array centre.
inline float proj(int k, float az_deg) {
    if (k >= 6) return 0.0f;
    return MIC_RADIUS_M * cosf((az_deg - 60.0f * k) * DEG);
}

inline float wrap360(float a) {
    a = fmodf(a, 360.0f);
    return a < 0 ? a + 360.0f : a;
}

// Signed smallest difference a - b in (-180, 180].
inline float angdiff(float a, float b) {
    float d = fmodf(a - b + 540.0f, 360.0f) - 180.0f;
    return d == -180.0f ? 180.0f : d;
}

}  // namespace geom
