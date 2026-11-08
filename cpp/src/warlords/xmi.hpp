/**
 * @file
 * @brief XMIDI, the music format of Miles' Audio Interface Library (the .XMI files in SOUND).
 *
 * An IFF file: FORM XDIR (how many sequences), then CAT XMID holding one
 * FORM XMID per sequence, each with a TIMB chunk (the timbres it uses) and
 * EVNT, the events. EVNT is MIDI with two differences:
 *
 *   - time is counted in intervals of AIL's 120 Hz timer, and a delay is a
 *     run of bytes below 0x80 that are simply added up;
 *   - a note-on carries its own duration, a MIDI variable-length number
 *     after the velocity, and there are no note-offs.
 *
 * Tempo meta events are left in by the converter but mean nothing: the
 * delays already have the tempo baked in.
 *
 * Parse() flattens the first sequence into time order, turning each note's
 * duration into a note-off. At a tick where notes end and others begin the
 * ends come first, as AIL's timer handler retires expired notes before it
 * reads on in the stream -- which matters when voices are short.
 */
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "absl/types/span.h"

namespace w2::xmi {

constexpr int kTickRate = 120;  ///< Ticks a second: AIL's timer rate.

/** @brief One MIDI message at its tick. */
struct Event {
    int t;       ///< The tick.
    uint8_t st;  ///< MIDI status; note-offs are 0x80 | channel.
    uint8_t a;   ///< First data byte.
    uint8_t b;   ///< Second data byte, 0 when the message has one.
};

/** @brief A sequence flattened into time order. */
struct Sequence {
    int length = 0;             ///< In ticks, to the end of the last note.
    std::vector<Event> events;  ///< In the order they are played.
};

/**
 * @brief The first sequence of an XMI file's bytes.
 * @return The sequence, or std::nullopt without an EVNT chunk. Meta events
 *         are dropped.
 */
std::optional<Sequence> Parse(absl::Span<const uint8_t> bytes);

}  // namespace w2::xmi
