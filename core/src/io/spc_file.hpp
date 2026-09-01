#pragma once

#include "base/error.hpp"
#include "playback/sample_bank.hpp"

#include <memory>
#include <string>

namespace midi_composer::io {

/**
 * Rips the instruments out of an `.spc`.
 *
 * ── What an .spc actually is ─────────────────────────────────────────────────
 *
 * A savestate of the SNES audio subsystem, frozen mid-song: 64KB of the sound
 * chip's RAM, the SPC700's registers, and the 128 DSP registers. Not a score
 * and not a sample library -- a photograph of a machine that was playing.
 *
 * The samples are in there, and they can be found without guessing: DSP
 * register $5D holds the page of the *sample directory*, a table of four-byte
 * entries giving each sample's start address and loop address. Walk it and
 * every BRR sample the song had loaded is locatable.
 *
 * ── What is not in there ─────────────────────────────────────────────────────
 *
 * Which sample is which instrument, what pitch each was recorded at, and how
 * each is meant to fade. All of that lives in the game's own music driver,
 * which differs per game and is 65816 code rather than data. The DSP registers
 * give the eight voices' pitch and envelope *at the instant of the snapshot* --
 * one moment, not a table.
 *
 * So this loader does what can honestly be done: it decodes every sample it
 * finds and lays them out across the programs in directory order. The result is
 * a bag of instruments, correctly decoded and correctly looped, that a person
 * then has to audition and assign by ear. That is the real workflow for ripping
 * a console, and pretending otherwise would mean inventing a mapping.
 *
 * Pitch is the one thing guessed at, and it is guessed consistently rather than
 * per sample: a rip has no root key, so every sample is treated as recorded at
 * the note the chip plays it at when its pitch register reads 1.0. Anything
 * else would be a different arbitrary number wearing a justification.
 */
base::Result<std::shared_ptr<playback::SampleBank>> load_spc(const std::string& utf8_path);

/** The same, from bytes already in memory, so a test can build a file. */
base::Result<std::shared_ptr<playback::SampleBank>> parse_spc(const std::string& bytes,
                                                              const std::string& name);

} // namespace midi_composer::io
