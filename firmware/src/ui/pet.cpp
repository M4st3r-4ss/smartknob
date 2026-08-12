#include "pet.h"

// Textures come first: the moods below pick from these rather than spelling out
// their own numbers, so two moods can share a coat and differ only in how they
// answer a pat, and a texture can be re-tuned in one place.
//
// snap_point is the subtle one. 1.1 is what the rest of the UI uses and reads as
// neutral. Below 1 the knob tips into the next step before reaching centre,
// which feels eager; above 1 it has to be pushed past centre, which feels
// reluctant. Combined with snap_point_bias - which shifts that threshold by
// direction - it is what lets a pet lean into a stroke or push back against it.

const PetTextureDescriptor PET_TEXTURES[] = {
    {
        "SMOOTH",
        // Even, unhurried, nothing in the way.
        8, 0.55f, 1.1f, 0,
        0, 0,
        0, 0,
    },
    {
        "FUR",
        // Fine and light, with a grain on almost every step: the knob feels like
        // it is running through something soft rather than clicking.
        4, 0.35f, 1.0f, 0,
        5, 0.22f,
        0, 0,
    },
    {
        "CRISP",
        // Firm, and it tips over early. Reads as a pet that is paying attention.
        6, 0.80f, 0.75f, 0,
        0, 0,
        0, 0,
    },
    {
        "SLIPPERY",
        // Weak hold, long snap: it gets ahead of the hand and keeps going.
        5, 0.20f, 1.35f, 0,
        14, 0.30f,
        0, 0,
    },
    {
        "CLINGY",
        // Leans into the stroke: biased so one direction gives way easily while
        // the other holds, and purring against the hand the whole time.
        6, 0.60f, 1.0f, 0.22f,
        0, 0,
        130, 0.28f,
    },
    {
        "TREMBLY",
        // Light and fine with a fast shiver running through it, on its own clock
        // rather than on the steps, so it does not feel like part of the detents.
        4, 0.30f, 0.9f, 0,
        0, 0,
        70, 0.34f,
    },
    {
        "NOTCHY",
        // Stiffening, with a hard catch every few steps: patience wearing thin.
        9, 1.20f, 1.25f, 0.15f,
        27, 0.95f,
        0, 0,
    },
    {
        "STIFF",
        // Wide, heavy, and biased against the stroke: every step is worked for.
        12, 1.90f, 1.4f, 0.3f,
        0, 0,
        0, 0,
    },
    {
        "HEAVY",
        // Very wide and slow, with a long slow swell rather than any resistance.
        22, 1.00f, 1.2f, 0,
        0, 0,
        620, 0.30f,
    },
};

// The two feedback channels are tuned as a pair for each mood: a fine light coat
// with a fluttery pattern reads as a happy animal, a wide stiff one with two
// hard shoves reads as one that wants to be left alone.
//
// Hues stay in the warm family the rest of the UI uses, except at the ends:
// the cross moods run red and sleepy runs blue. The pet page is the one place
// where the ring colour is the message rather than decoration.

const PetMoodDescriptor PET_MOODS[] = {
    {
        "CALM",
        "settling",
        "Breathes",
        PetTexture::SMOOTH,
        // Three unhurried thumps.
        3, 70, 240, 0.5f,
        32,
    },
    {
        "CURIOUS",
        "leaning in",
        "Perks up",
        PetTexture::CRISP,
        // Two quick enquiring taps.
        2, 40, 120, 0.6f,
        38,
    },
    {
        "HAPPY",
        "wriggling",
        "Wriggles",
        PetTexture::FUR,
        // A quick run of small beats.
        6, 45, 85, 0.7f,
        44,
    },
    {
        "LOVING",
        "purring",
        "Purrs",
        PetTexture::CLINGY,
        // A long, even, unmistakable purr.
        9, 55, 70, 0.55f,
        50,
    },
    {
        "PLAYFUL",
        "squirming",
        "Bounces",
        PetTexture::SLIPPERY,
        // Fast and sharp, like something skittering away.
        4, 35, 55, 1.15f,
        20,
    },
    {
        "SKITTISH",
        "flinching",
        "Startles",
        PetTexture::TREMBLY,
        // One hard flinch, then a scurry.
        5, 30, 45, 1.30f,
        14,
    },
    {
        "ANNOYED",
        "tolerating",
        "Twitches",
        PetTexture::NOTCHY,
        // Three sharp warning taps.
        3, 90, 150, 1.20f,
        8,
    },
    {
        "GRUMPY",
        "resisting",
        "Grumbles",
        PetTexture::STIFF,
        // Two hard shoves back.
        2, 120, 220, 1.60f,
        2,
    },
    {
        "SLEEPY",
        "stirring",
        "Dozes off",
        PetTexture::HEAVY,
        // One long, low sigh.
        1, 220, 500, 0.40f,
        150,
    },
};

const PetMoodDescriptor& petMoodAt(PetMood mood) {
    uint8_t index = (uint8_t)mood;
    return PET_MOODS[index < PET_MOOD_COUNT ? index : 0];
}

const PetTextureDescriptor& petTextureAt(PetTexture texture) {
    uint8_t index = (uint8_t)texture;
    return PET_TEXTURES[index < PET_TEXTURE_COUNT ? index : 0];
}

const PetTextureDescriptor& petTextureFor(PetMood mood) {
    return petTextureAt(petMoodAt(mood).texture);
}

PB_SmartKnobConfig petConfig(PetMood mood) {
    const PetMoodDescriptor& d = petMoodAt(mood);
    const PetTextureDescriptor& t = petTextureFor(mood);
    PB_SmartKnobConfig config = {
        0, 0, 0,
        0, -1,  // max < min: the pet can be patted for as long as you like
        (float)(t.detent_width_degrees * PI / 180),
        t.detent_strength, 1, t.snap_point,
        "",
        0, {}, t.snap_point_bias,
        d.led_hue,
    };
    strlcpy(config.text, d.name, sizeof(config.text));
    return config;
}

PetMood nextPetMood(PetMood mood, float charge, float agitation, float pat_degrees) {
    // Rough handling is read first: how the pet was patted outweighs how much,
    // and a pet that has been wound up does not care that the total was generous.
    if (agitation > 0.8f) {
        // Which way fright turns depends on the temper it already had: a pet that
        // was enjoying itself is startled, one that was already cross gets crosser.
        switch (mood) {
            case PetMood::ANNOYED:
            case PetMood::GRUMPY:
                return PetMood::GRUMPY;
            default:
                return PetMood::SKITTISH;
        }
    }
    if (agitation > 0.45f) {
        switch (mood) {
            case PetMood::SKITTISH:
                return PetMood::ANNOYED;
            case PetMood::ANNOYED:
            case PetMood::GRUMPY:
                return PetMood::GRUMPY;
            case PetMood::SLEEPY:
                // Woken roughly.
                return PetMood::SKITTISH;
            default:
                return PetMood::ANNOYED;
        }
    }
    if (pat_degrees >= PET_OVERPET_DEG) {
        // Even a happy pet has a limit; one endless pat crosses it. Gentle enough
        // not to frighten it, so this lands as impatience rather than fright.
        switch (mood) {
            case PetMood::ANNOYED:
            case PetMood::GRUMPY:
                return PetMood::GRUMPY;
            default:
                return PetMood::ANNOYED;
        }
    }

    // Nothing went wrong, so affection walks it up the ladder. Each rung needs
    // more than the last, and the sour moods have to be talked round first.
    switch (mood) {
        case PetMood::GRUMPY:
            return charge > 0.75f ? PetMood::ANNOYED : PetMood::GRUMPY;
        case PetMood::ANNOYED:
            return charge > 0.55f ? PetMood::CALM : PetMood::ANNOYED;
        case PetMood::SKITTISH:
            // Reassured more easily than a sulk, but it lands wary rather than calm.
            return charge > 0.40f ? PetMood::CURIOUS : PetMood::SKITTISH;
        case PetMood::SLEEPY:
            return charge > 0.45f ? PetMood::CURIOUS : PetMood::SLEEPY;
        case PetMood::CALM:
            return charge > 0.45f ? PetMood::CURIOUS : PetMood::CALM;
        case PetMood::CURIOUS:
            return charge > 0.60f ? PetMood::HAPPY : PetMood::CURIOUS;
        case PetMood::HAPPY:
            // Splits on how it is being patted: brisk play winds up into PLAYFUL,
            // calm attention deepens into LOVING. Same charge, different route.
            if (charge > 0.85f) {
                return agitation > 0.2f ? PetMood::PLAYFUL : PetMood::LOVING;
            }
            return PetMood::HAPPY;
        case PetMood::LOVING:
            // The top of the ladder; it stays as long as it is treated well.
            return PetMood::LOVING;
        case PetMood::PLAYFUL:
        default:
            // Playful burns itself out and settles back to happy.
            return PetMood::HAPPY;
    }
}

float petChargeAfterMood(PetMood mood) {
    switch (mood) {
        case PetMood::GRUMPY:
            // Arriving cross wipes the slate: it has to be won back from nothing.
            return 0;
        case PetMood::ANNOYED:
        case PetMood::SKITTISH:
            return 0.05f;
        case PetMood::LOVING:
            // Settled and content, so it does not immediately fall back down.
            return 0.5f;
        default:
            return 0.15f;
    }
}

float petAgitationAfterMood(PetMood mood) {
    switch (mood) {
        case PetMood::SKITTISH:
            // Still on edge, so a second fright comes easily.
            return 0.35f;
        case PetMood::ANNOYED:
            return 0.25f;
        case PetMood::GRUMPY:
            return 0.3f;
        default:
            return 0;
    }
}
