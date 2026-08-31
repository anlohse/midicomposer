#pragma once

#include "base/error.hpp"
#include "playback/sample_bank.hpp"

#include <memory>
#include <string>

namespace midi_composer::io {

/**
 * Reads a SoundFont 2 file into a SampleBank.
 *
 * ── Why SF2 and not the `.spc` this output is named after ────────────────────
 *
 * An `.spc` is a savestate of the SNES audio chip: 64KB of ARAM plus the DSP
 * registers. The samples really are in there, and the DSP's DIR register points
 * at the directory that locates them, so ripping BRR out of one is a solved
 * problem. What is *not* in there is which sample is which instrument, what
 * pitch each was recorded at, or how each one is meant to fade -- all of that
 * lives in the game's own music driver, which differs per game. A snapshot
 * gives the envelope of the eight voices at one instant, not a table.
 *
 * So an `.spc` yields a bag of samples that a human then has to map by ear.
 * That is a ripping workflow, and a good one to have later. SF2 is the format
 * that answers the question this loader is being asked: presets addressed by
 * bank and program -- exactly what a program change selects -- with loop points,
 * root keys, tuning and envelopes already stated.
 *
 * ── What of the format is read, and what is skipped ──────────────────────────
 *
 * Enough to play a composition. A preset resolves to its first instrument, and
 * that instrument to its first zone naming a sample. Key ranges, velocity
 * layers, stereo pairs, modulators and the filter are all read past. That makes
 * a layered orchestral bank sound thinner than it should, and is the honest
 * shape of "one sample per program" (see SampleBank) rather than a bug in the
 * parsing.
 */
base::Result<std::shared_ptr<playback::SampleBank>> load_sf2(const std::string& utf8_path);

/** The same, from bytes already in memory. Exposed so a test can build a file
    rather than needing one on disk. */
base::Result<std::shared_ptr<playback::SampleBank>> parse_sf2(const std::string& bytes,
                                                              const std::string& name);

} // namespace midi_composer::io
