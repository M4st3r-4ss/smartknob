#if SK_DISPLAY

#include "ui_icons.h"

#include "ui_theme.h"

namespace ui {

// strokeArc() and stroke() live in ui_theme now: the pet face needs the same two
// primitives, and both are in namespace ui, so the calls below are unchanged.

static void iconVolume(TFT_eSprite& spr, int16_t cx, int16_t cy, float s, uint16_t color, float value_unit) {
    float body_left = cx - 0.72f * s;
    float cone_x = cx - 0.30f * s;
    spr.fillRect(body_left, cy - 0.20f * s, cone_x - body_left, 0.40f * s, color);
    spr.fillTriangle(cone_x, cy - 0.20f * s, cone_x, cy + 0.20f * s, cx + 0.06f * s, cy + 0.62f * s, color);
    spr.fillTriangle(cone_x, cy - 0.20f * s, cx + 0.06f * s, cy + 0.62f * s, cx + 0.06f * s, cy - 0.62f * s, color);

    uint8_t waves = value_unit <= 0.005f ? 0 : 1 + (uint8_t)(value_unit * 2.99f);
    for (uint8_t i = 0; i < 3; i++) {
        uint16_t wave_color = i < waves ? color : dim(color, 0.22f);
        strokeArc(spr, cx + 0.06f * s, cy, (0.42f + 0.26f * i) * s, 2, -46, 46, wave_color);
    }
}

static void iconBrightness(TFT_eSprite& spr, int16_t cx, int16_t cy, float s, uint16_t color, float value_unit) {
    spr.fillCircle(cx, cy, 0.34f * s, color);
    spr.drawCircle(cx, cy, 0.34f * s + 2, dim(color, 0.35f));
    float inner = 0.52f * s;
    float outer = inner + (0.16f + 0.26f * value_unit) * s;
    for (uint8_t i = 0; i < 8; i++) {
        float rad = (i * 45 + 22.5f) * PI / 180;
        stroke(spr, cx + inner * cosf(rad), cy - inner * sinf(rad), cx + outer * cosf(rad), cy - outer * sinf(rad), color);
    }
}

static void iconBulb(TFT_eSprite& spr, int16_t cx, int16_t cy, float s, uint16_t color, float value_unit) {
    bool on = value_unit > 0.5f;
    float glass_y = cy - 0.22f * s;
    float glass_r = 0.52f * s;
    if (on) {
        spr.fillCircle(cx, glass_y, glass_r - 1, dim(color, 0.30f));
        for (uint8_t i = 0; i < 6; i++) {
            float rad = (i * 60 + 30) * PI / 180;
            stroke(spr,
                cx + (glass_r + 0.14f * s) * cosf(rad), glass_y - (glass_r + 0.14f * s) * sinf(rad),
                cx + (glass_r + 0.34f * s) * cosf(rad), glass_y - (glass_r + 0.34f * s) * sinf(rad),
                dim(color, 0.75f));
        }
    }
    strokeArc(spr, cx, glass_y, glass_r, 2, -35, 215, color);
    stroke(spr, cx - glass_r * 0.82f, glass_y + glass_r * 0.58f, cx - 0.24f * s, cy + 0.42f * s, color);
    stroke(spr, cx + glass_r * 0.82f, glass_y + glass_r * 0.58f, cx + 0.24f * s, cy + 0.42f * s, color);
    for (uint8_t i = 0; i < 3; i++) {
        float y = cy + (0.42f + 0.17f * i) * s;
        stroke(spr, cx - 0.24f * s, y, cx + 0.24f * s, y, i == 2 ? dim(color, 0.6f) : color);
    }
    // Filament, brighter when lit.
    uint16_t filament = on ? COLOR_SPECULAR : dim(color, 0.4f);
    strokeArc(spr, cx, glass_y + 0.1f * s, 0.20f * s, 1, 20, 160, filament);
}

static void iconFan(TFT_eSprite& spr, int16_t cx, int16_t cy, float s, uint16_t color, float value_unit, float anim_deg) {
    for (uint8_t i = 0; i < 3; i++) {
        float rad = (anim_deg + i * 120) * PI / 180;
        float tip_x = cx + 0.62f * s * cosf(rad);
        float tip_y = cy - 0.62f * s * sinf(rad);
        float left = rad + 0.30f;
        float right = rad - 0.30f;
        spr.fillTriangle(cx, cy,
            cx + 0.66f * s * cosf(left), cy - 0.66f * s * sinf(left),
            cx + 0.66f * s * cosf(right), cy - 0.66f * s * sinf(right),
            color);
        spr.fillCircle(tip_x, tip_y, 0.26f * s, color);
    }
    spr.fillCircle(cx, cy, 0.19f * s, COLOR_BG);
    spr.drawCircle(cx, cy, 0.19f * s, color);
    spr.drawCircle(cx, cy, 0.94f * s, dim(color, 0.30f + 0.4f * value_unit));
}

static void iconScroll(TFT_eSprite& spr, int16_t cx, int16_t cy, float s, uint16_t color, float anim_deg) {
    strokeArc(spr, cx, cy, 0.68f * s, 2, anim_deg + 28, anim_deg + 300, color);
    float rad = (anim_deg + 300) * PI / 180;
    float hx = cx + 0.68f * s * cosf(rad);
    float hy = cy - 0.68f * s * sinf(rad);
    float head = rad - PI / 2;
    float wing = 0.24f * s;
    spr.fillTriangle(
        hx + wing * cosf(head), hy - wing * sinf(head),
        hx + wing * cosf(head + 2.3f), hy - wing * sinf(head + 2.3f),
        hx + wing * cosf(head - 2.3f), hy - wing * sinf(head - 2.3f),
        color);
    spr.fillCircle(cx, cy, 0.13f * s, dim(color, 0.55f));
}

static void iconTimer(TFT_eSprite& spr, int16_t cx, int16_t cy, float s, uint16_t color, float value_unit) {
    strokeArc(spr, cx, cy, 0.78f * s, 2, 0, 360, dim(color, 0.55f));
    for (uint8_t i = 0; i < 12; i++) {
        float rad = i * 30 * PI / 180;
        float inner = (i % 3 == 0) ? 0.58f * s : 0.66f * s;
        spr.drawLine(cx + inner * cosf(rad), cy - inner * sinf(rad),
            cx + 0.72f * s * cosf(rad), cy - 0.72f * s * sinf(rad), color);
    }
    float hand = (90 - 360 * value_unit) * PI / 180;
    stroke(spr, cx, cy, cx + 0.52f * s * cosf(hand), cy - 0.52f * s * sinf(hand), color);
    spr.fillCircle(cx, cy, 0.10f * s, color);
}

static void iconSettings(TFT_eSprite& spr, int16_t cx, int16_t cy, float s, uint16_t color, float anim_deg) {
    // A gear, rather than the spokes-with-dots wheel this used to be: that read
    // as a sunburst at list size, because the spokes were hairlines and the dots
    // that ended them were the heaviest thing in the glyph. A gear is instead a
    // solid body with teeth cut into its outside, so the silhouette carries it.
    const float r_body = 0.58f * s;
    const float r_tip = 0.94f * s;
    const float r_hub = 0.26f * s;

    // Teeth first, then the body over their bases, so no seam shows where a
    // tooth meets the rim.
    for (uint8_t i = 0; i < 8; i++) {
        float mid = (anim_deg + i * 45) * PI / 180;
        // Tapered, so each one reads as a tooth instead of a peg.
        const float base_half = 0.30f;
        const float tip_half = 0.19f;
        float bx0 = cx + r_body * cosf(mid - base_half);
        float by0 = cy - r_body * sinf(mid - base_half);
        float bx1 = cx + r_body * cosf(mid + base_half);
        float by1 = cy - r_body * sinf(mid + base_half);
        float tx0 = cx + r_tip * cosf(mid - tip_half);
        float ty0 = cy - r_tip * sinf(mid - tip_half);
        float tx1 = cx + r_tip * cosf(mid + tip_half);
        float ty1 = cy - r_tip * sinf(mid + tip_half);
        spr.fillTriangle(bx0, by0, bx1, by1, tx1, ty1, color);
        spr.fillTriangle(bx0, by0, tx1, ty1, tx0, ty0, color);
    }

    spr.fillCircle(cx, cy, r_body, color);
    // Hub punched back out to the face, which is what keeps a solid disc of gold
    // reading as a ring of metal. Skipped when there is no room to punch it.
    if (r_hub >= 2) {
        spr.fillCircle(cx, cy, r_hub, COLOR_BG);
        spr.drawCircle(cx, cy, r_hub, dim(color, 0.55f));
    }
}

static void iconReset(TFT_eSprite& spr, int16_t cx, int16_t cy, float s, uint16_t color) {
    // Arrow curving back to where it started, with the gap at the top. Static,
    // unlike the scroll icon's spinning arc: a reset happens once.
    const float r = 0.74f * s;
    strokeArc(spr, cx, cy, r, 2, 130, 410, color);

    float end = 50 * PI / 180;
    float hx = cx + r * cosf(end);
    float hy = cy - r * sinf(end);
    // Tangent at the open end, pointing the way the arc was travelling.
    float apex = end + PI / 2;
    float wing = 0.30f * s;
    spr.fillTriangle(
        hx + wing * cosf(apex), hy - wing * sinf(apex),
        hx + wing * cosf(apex + 2.3f), hy - wing * sinf(apex + 2.3f),
        hx + wing * cosf(apex - 2.3f), hy - wing * sinf(apex - 2.3f),
        color);
}

static void iconStrength(TFT_eSprite& spr, int16_t cx, int16_t cy, float s, uint16_t color, float value_unit) {
    // Rising bars, filled up to the current level.
    uint8_t lit = 1 + (uint8_t)(value_unit * 3.99f);
    for (uint8_t i = 0; i < 4; i++) {
        float h = (0.28f + 0.30f * i) * s;
        float x = cx + (i - 1.5f) * 0.36f * s;
        uint16_t bar = i < lit ? color : dim(color, 0.25f);
        spr.fillRect(x - 0.12f * s, cy + 0.62f * s - h, 0.24f * s, h, bar);
    }
}

static void iconPress(TFT_eSprite& spr, int16_t cx, int16_t cy, float s, uint16_t color, float value_unit) {
    float plate_y = cy + 0.52f * s;
    spr.fillRect(cx - 0.72f * s, plate_y, 1.44f * s, 0.14f * s, color);
    float tip = plate_y - (0.16f + 0.30f * (1 - value_unit)) * s;
    stroke(spr, cx, cy - 0.66f * s, cx, tip, color);
    spr.fillTriangle(cx, tip + 0.04f * s,
        cx - 0.22f * s, tip - 0.26f * s,
        cx + 0.22f * s, tip - 0.26f * s, color);
    strokeArc(spr, cx, plate_y, 0.30f * s + 0.34f * s * value_unit, 1, 200, 340, dim(color, 0.5f));
}

static void iconClick(TFT_eSprite& spr, int16_t cx, int16_t cy, float s, uint16_t color, float value_unit) {
    spr.fillCircle(cx, cy, 0.20f * s, color);
    for (uint8_t i = 0; i < 3; i++) {
        float r = (0.40f + 0.22f * i) * s;
        uint16_t ring = dim(color, 0.9f - 0.22f * i * (1.2f - value_unit));
        strokeArc(spr, cx, cy, r, 2, -52 - 14 * i, 52 + 14 * i, ring);
        strokeArc(spr, cx, cy, r, 2, 128 - 14 * i, 232 + 14 * i, ring);
    }
}

static void iconLed(TFT_eSprite& spr, int16_t cx, int16_t cy, float s, uint16_t color, float value_unit) {
    strokeArc(spr, cx, cy, 0.72f * s, 1, 0, 360, dim(color, 0.35f));
    uint8_t lit = (uint8_t)(value_unit * 8.0f + 0.5f);
    for (uint8_t i = 0; i < 8; i++) {
        float rad = (90 - i * 45) * PI / 180;
        uint16_t led = i < lit ? color : dim(color, 0.28f);
        spr.fillCircle(cx + 0.72f * s * cosf(rad), cy - 0.72f * s * sinf(rad), 0.15f * s, led);
    }
    spr.fillCircle(cx, cy, 0.16f * s, dim(color, 0.45f + 0.45f * value_unit));
}

static void iconCalibrate(TFT_eSprite& spr, int16_t cx, int16_t cy, float s, uint16_t color, float anim_deg) {
    strokeArc(spr, cx, cy, 0.74f * s, 2, 0, 360, dim(color, 0.55f));
    for (uint8_t i = 0; i < 4; i++) {
        float rad = i * 90 * PI / 180;
        stroke(spr, cx + 0.46f * s * cosf(rad), cy - 0.46f * s * sinf(rad),
            cx + 0.92f * s * cosf(rad), cy - 0.92f * s * sinf(rad), color);
    }
    float rad = anim_deg * PI / 180;
    stroke(spr, cx, cy, cx + 0.48f * s * cosf(rad), cy - 0.48f * s * sinf(rad), color);
    spr.fillCircle(cx, cy, 0.11f * s, color);
}

static void iconSpring(TFT_eSprite& spr, int16_t cx, int16_t cy, float s, uint16_t color, float value_unit) {
    // A coil between two plates, wound tighter as the stiffness goes up. The
    // glyph changes in the same direction the setting does, so the row can be
    // read at a glance without looking at the number.
    const float half_w = 0.60f * s;
    const float plate = max(2.0f, 0.12f * s);
    const float top = cy - 0.70f * s;
    const float bottom = cy + 0.70f * s;
    spr.fillRect(cx - half_w, top, 2 * half_w, plate, color);
    spr.fillRect(cx - half_w, bottom - plate, 2 * half_w, plate, color);

    uint8_t legs = 6 + 2 * (uint8_t)(value_unit * 3.99f);
    float span = (bottom - plate) - (top + plate);
    float step = span / legs;
    float reach = half_w * 0.74f;
    float x = cx - reach;
    float y = top + plate;
    for (uint8_t i = 0; i < legs; i++) {
        float nx = (i % 2 == 0) ? cx + reach : cx - reach;
        stroke(spr, x, y, nx, y + step, color);
        x = nx;
        y += step;
    }
}

static void iconDroop(TFT_eSprite& spr, int16_t cx, int16_t cy, float s, uint16_t color, float value_unit) {
    // A span that sags when the term is off and is drawn level as it comes up.
    // Droop is the thing this setting removes, so the glyph shows it going out.
    const float half_w = 0.76f * s;
    const float line_y = cy - 0.26f * s;
    const float sag = (1 - value_unit) * 0.62f * s;

    // Where it should sit, dashed and dim: a sag is only legible against level.
    for (int8_t i = -3; i < 3; i++) {
        float x0 = cx + half_w * (i / 3.0f);
        stroke(spr, x0, line_y, x0 + half_w * 0.20f, line_y, dim(color, 0.32f));
    }

    // The span itself, hanging between its two posts.
    const uint8_t steps = 10;
    float px = cx - half_w;
    float py = line_y;
    for (uint8_t i = 1; i <= steps; i++) {
        float t = (float)i / steps;
        float x = cx - half_w + 2 * half_w * t;
        // Parabola through both posts, deepest in the middle.
        float y = line_y + sag * 4 * t * (1 - t);
        stroke(spr, px, py, x, y, color);
        px = x;
        py = y;
    }

    float post = max(1.5f, 0.11f * s);
    spr.fillCircle(cx - half_w, line_y, post, color);
    spr.fillCircle(cx + half_w, line_y, post, color);
}

static void iconDamping(TFT_eSprite& spr, int16_t cx, int16_t cy, float s, uint16_t color, float value_unit) {
    // A ringing that dies away, and dies away sooner the higher the setting goes.
    // At the bottom of the range it barely decays at all, which is exactly what
    // too little damping feels like in the hand.
    const float half_w = 0.82f * s;
    const float amp = 0.54f * s;
    const float decay = lerpf(0.5f, 4.5f, value_unit);

    stroke(spr, cx - half_w, cy, cx + half_w, cy, dim(color, 0.30f));

    const uint8_t steps = 24;
    float px = cx - half_w;
    float py = cy;
    for (uint8_t i = 1; i <= steps; i++) {
        float t = (float)i / steps;
        float x = cx - half_w + 2 * half_w * t;
        float y = cy - amp * expf(-decay * t) * sinf(t * 3 * PI);
        stroke(spr, px, py, x, y, color);
        px = x;
        py = y;
    }
}

static void iconTrace(TFT_eSprite& spr, int16_t cx, int16_t cy, float s, uint16_t color, float value_unit) {
    // A scope window. The frame is there either way and only the signal comes and
    // goes, so the row reads as one thing being switched on rather than as two
    // different glyphs.
    bool on = value_unit > 0.5f;
    const float half_w = 0.84f * s;
    const float half_h = 0.60f * s;
    spr.drawRect(cx - half_w, cy - half_h, 2 * half_w, 2 * half_h, dim(color, on ? 0.60f : 0.30f));

    if (!on) {
        stroke(spr, cx - half_w * 0.78f, cy, cx + half_w * 0.78f, cy, dim(color, 0.35f));
        return;
    }

    // Something with corners in it, so it reads as sampled data rather than as
    // the smooth ring the damping glyph next door already owns.
    const uint8_t points = 8;
    const float xs[points] = {-0.78f, -0.46f, -0.30f, -0.05f, 0.14f, 0.38f, 0.56f, 0.78f};
    const float ys[points] = {0.10f, 0.10f, -0.62f, 0.34f, -0.30f, 0.52f, -0.08f, -0.08f};
    for (uint8_t i = 1; i < points; i++) {
        stroke(spr,
            cx + half_w * xs[i - 1], cy + half_h * ys[i - 1],
            cx + half_w * xs[i], cy + half_h * ys[i],
            color);
    }
    spr.fillCircle(cx + half_w * xs[2], cy + half_h * ys[2], max(1.5f, 0.10f * s), COLOR_SPECULAR);
}

static void iconPet(TFT_eSprite& spr, int16_t cx, int16_t cy, float s, uint16_t color, float value_unit) {
    // The page in miniature: the rim of the glass, and the pair of tall ovals
    // inside it. No ears, no head, no mouth - the mouth only exists on the page
    // while the pet is answering, and what makes this read as the pet at
    // carousel size is the eyes, so they take most of the weight the icon has.
    //
    // The rim is the icon's outline, so it is drawn at full colour and two to
    // three pixels thick, not as a hairline: a 1px ring at a third brightness
    // washed out into the bezel at carousel size. strokeArc walks the ring as
    // radial segments, which keeps it solid - concentric drawCircle calls leave
    // gaps between neighbouring Bresenham radii.
    strokeArc(spr, cx, cy, 0.90f * s, max(2.0f, 0.10f * s), -180, 180, color);

    float rx = 0.15f * s;
    float ry = rx * lerpf(2.0f, 2.6f, value_unit);
    if (rx < 1 || ry < 1) {
        return;
    }
    for (int8_t side = -1; side <= 1; side += 2) {
        float ex = cx + side * 0.34f * s;
        spr.fillEllipse((int32_t)ex, (int32_t)cy, (int32_t)rx, (int32_t)ry, color);
        // The same catchlight as the face, once there is room to punch it out.
        if (rx >= 3) {
            spr.fillCircle((int32_t)(ex + rx * 0.30f), (int32_t)(cy - ry * 0.64f), 1, COLOR_BG);
        }
    }
}

void icon(TFT_eSprite& spr, AppIcon which, int16_t cx, int16_t cy, float size, uint16_t color, float value_unit, float anim_deg) {
    if (size < 3) {
        return;
    }
    value_unit = clamp01(value_unit);
    switch (which) {
        case AppIcon::VOLUME:
            iconVolume(spr, cx, cy, size, color, value_unit);
            break;
        case AppIcon::BRIGHTNESS:
            iconBrightness(spr, cx, cy, size, color, value_unit);
            break;
        case AppIcon::BULB:
            iconBulb(spr, cx, cy, size, color, value_unit);
            break;
        case AppIcon::FAN:
            iconFan(spr, cx, cy, size, color, value_unit, anim_deg);
            break;
        case AppIcon::SCROLL:
            iconScroll(spr, cx, cy, size, color, anim_deg);
            break;
        case AppIcon::TIMER:
            iconTimer(spr, cx, cy, size, color, value_unit);
            break;
        case AppIcon::SETTINGS:
            iconSettings(spr, cx, cy, size, color, anim_deg);
            break;
        case AppIcon::STRENGTH:
            iconStrength(spr, cx, cy, size, color, value_unit);
            break;
        case AppIcon::PRESS:
            iconPress(spr, cx, cy, size, color, value_unit);
            break;
        case AppIcon::CLICK:
            iconClick(spr, cx, cy, size, color, value_unit);
            break;
        case AppIcon::LED:
            iconLed(spr, cx, cy, size, color, value_unit);
            break;
        case AppIcon::CALIBRATE:
            iconCalibrate(spr, cx, cy, size, color, anim_deg);
            break;
        case AppIcon::RESET:
            iconReset(spr, cx, cy, size, color);
            break;
        case AppIcon::PET:
            iconPet(spr, cx, cy, size, color, value_unit);
            break;
        case AppIcon::SPRING:
            iconSpring(spr, cx, cy, size, color, value_unit);
            break;
        case AppIcon::DROOP:
            iconDroop(spr, cx, cy, size, color, value_unit);
            break;
        case AppIcon::DAMPING:
            iconDamping(spr, cx, cy, size, color, value_unit);
            break;
        case AppIcon::TRACE:
            iconTrace(spr, cx, cy, size, color, value_unit);
            break;
    }
}

}  // namespace ui

#endif
