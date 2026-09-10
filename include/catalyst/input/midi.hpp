/**
 * @file midi.hpp
 * @brief MIDI input: one device per input port, notes and controllers as ordinary controls, plus the raw messages.
 * @details A MIDI keyboard is an input device, so it lives here. A MIDI *synthesiser* is an audio one and belongs to
 * `catalyst::audio`; this header stops at "what did the player press", and deliberately does not model voices,
 * instruments or timing beyond a timestamp.
 *
 * Two views of the same input, because both are genuinely wanted:
 *
 *   - **As controls.** 128 note slots and 128 controller slots, each a ratio in [0, 1]. A note's value is its velocity
 *     while held and 0 once released, so binding "middle C" to an action is the same operation as binding a key, and a
 *     mod wheel is an axis like any other. This is what makes a MIDI pad usable as a game controller.
 *   - **As messages.** `midi_event` carries the status, channel and data bytes. This is what a sequencer, a recorder,
 *     or anything that needs the parts the control model flattens away - the channel, note-off velocity, running
 *     status - should listen to.
 *
 * Channels are merged in the control view: a note-on for middle C sets the same slot whichever of the sixteen channels
 * it arrived on, because a player pressing one key should actuate one control. Code that needs the channel reads
 * `midi_event::channel`.
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#include <catalyst/input/device.hpp>

#include <cstddef>
#include <cstdint>

namespace catalyst::input
{
    /** @brief MIDI notes, 0-127. Note 60 is middle C. */
    inline constexpr std::size_t midi_note_count = 128;
    /** @brief MIDI continuous controllers, 0-127. Controller 1 is the mod wheel, 7 volume, 64 the sustain pedal. */
    inline constexpr std::size_t midi_controller_count = 128;
    /** @brief MIDI channels, 0-15 (displayed to musicians as 1-16). */
    inline constexpr std::size_t midi_channel_count = 16;

    /**
     * @enum midi_status
     * @brief The channel-voice message types, by their high nibble. System messages (0xF0 and up) are delivered with
     * `midi_status::system` and their full status byte in `midi_event::status_byte`.
     */
    enum class midi_status : std::uint8_t
    {
        note_off = 0x80,
        note_on = 0x90,
        /** @brief Per-note aftertouch ("polyphonic key pressure"). */
        poly_pressure = 0xA0,
        control_change = 0xB0,
        program_change = 0xC0,
        /** @brief Whole-channel aftertouch. */
        channel_pressure = 0xD0,
        pitch_bend = 0xE0,
        /** @brief Anything from 0xF0 up: clock, transport, system exclusive. */
        system = 0xF0
    };

    /** @brief A short, stable name for a status ("note on", "control change", ...). */
    [[nodiscard]] std::string_view midi_status_name(midi_status status) noexcept;

    /** @brief The note name for a MIDI note number, in scientific pitch notation ("C4", "F#5"). */
    [[nodiscard]] std::string_view midi_note_name(std::uint8_t note) noexcept;

    /**
     * @struct midi_message
     * @brief One channel-voice message, unpacked.
     * @note A note-on with velocity 0 is a note-off; the standard permits it and half the hardware in the world uses it
     * for running status. The registry normalises this, so `status` here is already `note_off` in that case and no
     * consumer has to know.
     */
    struct midi_message
    {
        midi_status status{midi_status::note_on};
        /** @brief 0-15. Always 0 for midi_status::system. */
        std::uint8_t channel{0};
        /** @brief Note number, controller number, or program number, depending on `status`. */
        std::uint8_t data1{0};
        /** @brief Velocity, controller value, or pressure, depending on `status`. */
        std::uint8_t data2{0};
        /** @brief The raw status byte, which is the only place a system message's type survives. */
        std::uint8_t status_byte{0};

        /** @brief The 14-bit pitch-bend value as a signed ratio in [-1, 1]. Meaningful for midi_status::pitch_bend. */
        [[nodiscard]] constexpr float bend() const noexcept
        {
            const int raw = static_cast<int>(data1) | (static_cast<int>(data2) << 7);
            return static_cast<float>(raw - 8192) / 8192.0f;
        }

        /** @brief `data2` as a ratio in [0, 1] - a velocity, a controller value, a pressure. */
        [[nodiscard]] constexpr float value() const noexcept { return static_cast<float>(data2) / 127.0f; }
    };

    // ------------------------------------------------------------------------------------------------------------------
    // Controls
    // ------------------------------------------------------------------------------------------------------------------

    /** @brief Slot of note 0. */
    inline constexpr std::size_t midi_note_control_base = 0;
    /** @brief Slot of controller 0. */
    inline constexpr std::size_t midi_controller_control_base = midi_note_control_base + midi_note_count;
    /** @brief Slot of the channel-wide pitch bend, an axis in [-1, 1]. */
    inline constexpr std::size_t midi_bend_control = midi_controller_control_base + midi_controller_count;
    /** @brief Slot of channel pressure (aftertouch), a ratio in [0, 1]. */
    inline constexpr std::size_t midi_pressure_control = midi_bend_control + 1;
    /** @brief Total number of controls a MIDI device has. */
    inline constexpr std::size_t midi_control_count = midi_pressure_control + 1;

    /** @brief The slot note @p note occupies, or no_control if @p note is out of range. Value is velocity while held. */
    [[nodiscard]] inline constexpr control_id midi_note_control(std::size_t note) noexcept
    {
        return note < midi_note_count ? control_at(midi_note_control_base + note) : no_control;
    }

    /** @brief The slot controller @p cc occupies, or no_control if @p cc is out of range. */
    [[nodiscard]] inline constexpr control_id midi_controller_control(std::size_t cc) noexcept
    {
        return cc < midi_controller_count ? control_at(midi_controller_control_base + cc) : no_control;
    }

    /**
     * @fn midi_layout
     * @brief The layout every MIDI device shares: 128 notes, 128 controllers, pitch bend, channel pressure.
     */
    [[nodiscard]] const layout_ref &midi_layout();

    // ------------------------------------------------------------------------------------------------------------------
    // Events
    // ------------------------------------------------------------------------------------------------------------------

    /**
     * @struct midi_event
     * @brief One MIDI message from an input port.
     * @details Published for every message, including the ones the control view flattens - so a recorder can listen to
     * these alone and reproduce the stream exactly. `time` is stamped from the port's own timestamp where the backend
     * offers one, which matters for anything that has to keep musical time.
     */
    struct midi_event : device_event<tags::midi>
    {
        /** @brief The input port's index, i.e. device_info::slot. */
        std::uint32_t port{0};
        midi_message message{};

        /** @brief Shorthand for `message.status`. */
        [[nodiscard]] constexpr midi_status status() const noexcept { return message.status; }
        /** @brief The slot this message changed, or no_control for messages that map to none. */
        [[nodiscard]] constexpr control_id control() const noexcept
        {
            switch (message.status)
            {
            case midi_status::note_on:
            case midi_status::note_off:
            case midi_status::poly_pressure:
                return midi_note_control(message.data1);
            case midi_status::control_change:
                return midi_controller_control(message.data1);
            case midi_status::pitch_bend:
                return control_at(midi_bend_control);
            case midi_status::channel_pressure:
                return control_at(midi_pressure_control);
            default:
                return no_control;
            }
        }
    };

} // namespace catalyst::input
