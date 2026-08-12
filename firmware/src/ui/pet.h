#pragma once

#include <Arduino.h>

#include "../proto_gen/smartknob.pb.h"

/**
 * The pet page: a creature that is patted by rotating the knob.
 *
 * Every mood answers a pat in two ways, and the pair is what makes the moods
 * feel like different animals rather than different pictures:
 *   1. while the knob turns, through the page's texture (see PetTexture), and
 *   2. once the hand stops, through a played haptic pattern - a purr, a
 *      wriggle, a single deep sigh, a shove.
 * The face carries the same mood on screen, and changes again while it is being
 * patted and for a beat afterwards.
 *
 * How the pet is patted matters as much as how much: strokes are measured in
 * degrees turned rather than detents crossed, so a mood with wide detents does
 * not bank affection faster than a fine one, and stroke speed is tracked
 * separately. Slow strokes win the pet over, frantic ones wind it up.
 */
enum class PetMood : uint8_t {
    CALM,
    CURIOUS,
    HAPPY,
    LOVING,
    PLAYFUL,
    SKITTISH,
    ANNOYED,
    GRUMPY,
    SLEEPY,
};

constexpr uint8_t PET_MOOD_COUNT = 9;

/**
 * How the knob feels under the hand, which is the pet's coat and its temper at
 * the same time. Each texture is a whole recipe rather than one number:
 *
 *   detent width      how far apart the steps sit, the grain of the coat
 *   detent strength   how firmly each step holds
 *   snap point        hysteresis. Below 1 the knob tips into the next step
 *                     early and reads crisp; above 1 it has to be pushed past
 *                     centre and reads sticky or reluctant.
 *   snap bias         asymmetry between the two directions, so a pet can lean
 *                     into the hand and resist being pushed back
 *   grain             an extra pulse every so many degrees: a catch in the fur
 *   tremble           pulses on a clock rather than on movement, felt as a purr
 *                     or a shiver that runs on regardless of how you stroke
 *
 * Grain and tremble are what make two moods with the same detents feel unalike,
 * and they only play while a hand is actually moving the knob.
 */
enum class PetTexture : uint8_t {
    SMOOTH,     // even and unhurried, nothing in the way
    FUR,        // fine, light, a continuous soft grain
    CRISP,      // firm and quick to tip over, alert
    SLIPPERY,   // weak hold with a long snap: it slides ahead of the hand
    CLINGY,     // leans into the stroke and purrs against it
    TREMBLY,    // light, with a fast shiver running through it
    NOTCHY,     // stiffening, with a hard catch every few steps
    STIFF,      // wide, heavy, and it fights back
    HEAVY,      // very wide and slow, breathing more than resisting
};

constexpr uint8_t PET_TEXTURE_COUNT = 9;

struct PetTextureDescriptor {
    /** Shown on the page, so the feel under the hand has a name. */
    const char* name;

    float detent_width_degrees;
    float detent_strength;
    /** Hysteresis; must stay >= 0.5 or the motor rejects the config. */
    float snap_point;
    /** Directional asymmetry; must stay >= 0. */
    float snap_point_bias;

    /** Degrees between grain pulses, or 0 for none. */
    float grain_every_degrees;
    float grain_strength;

    /** Milliseconds between tremble pulses, or 0 for none. */
    uint16_t tremble_ms;
    float tremble_strength;
};

struct PetMoodDescriptor {
    const char* name;
    /** Line shown while the pat is happening. */
    const char* while_petting;
    /** Line shown for the reaction beat once the hand stops. */
    const char* after_petting;

    /** Feedback 1: the feel of the knob while it is being patted. */
    PetTexture texture;

    // Feedback 2: the pattern played after the pat, as beats of kick-and-release.
    // Beat count, kick length and gap set the rhythm; strength sets the weight.
    uint8_t beats;
    uint16_t beat_kick_ms;
    uint16_t beat_gap_ms;
    float beat_strength;

    /** LED ring hue for the mood. */
    uint8_t led_hue;
};

extern const PetMoodDescriptor PET_MOODS[];
extern const PetTextureDescriptor PET_TEXTURES[];

const PetMoodDescriptor& petMoodAt(PetMood mood);
const PetTextureDescriptor& petTextureAt(PetTexture texture);
/** The texture a mood is patted through. */
const PetTextureDescriptor& petTextureFor(PetMood mood);

/** Haptic config for the pet page in a given mood. Free-spinning, no endstops. */
PB_SmartKnobConfig petConfig(PetMood mood);

/** Stillness that ends a pat and starts the reaction. */
constexpr uint32_t PET_SETTLE_MS = 320;
/** Left alone this long with the page open, the pet dozes off. */
constexpr uint32_t PET_IDLE_SLEEP_MS = 20000;

/** Affection banked per degree patted, and how fast it bleeds away per second. */
constexpr float PET_CHARGE_PER_DEG = 0.0038f;
constexpr float PET_CHARGE_DECAY_PER_S = 0.06f;

/**
 * Agitation: what rough handling builds up. It decays faster than affection,
 * so a pet that was wound up settles sooner than one that was won over.
 */
constexpr float PET_AGITATION_DECAY_PER_S = 0.13f;
/** Stroke speed at which patting starts to read as frantic, in degrees/second. */
constexpr float PET_ROUGH_DEG_PER_S = 300;
/** Agitation banked per degree, at twice the rough speed. */
constexpr float PET_AGITATION_PER_DEG = 0.0025f;
/** Degrees in a single unbroken pat that read as too much of a good thing. */
constexpr float PET_OVERPET_DEG = 540;

/**
 * Mood after a pat ends. Affection walks the pet up one ladder and agitation
 * pushes it down another, so the same amount of patting lands somewhere quite
 * different depending on how it was done.
 */
PetMood nextPetMood(PetMood mood, float charge, float agitation, float pat_degrees);

/** Affection and agitation left over once the pet settles into a new mood. */
float petChargeAfterMood(PetMood mood);
float petAgitationAfterMood(PetMood mood);
