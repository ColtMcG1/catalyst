/**
 * @file text.hpp
 * @brief Text entry: committed text, and the in-progress composition an input method shows before it.
 * @details This is deliberately not in keyboard.hpp. Text is not a physical key - what a key produces depends on the
 * layout, the modifiers, and whatever input method is running, and a Japanese IME commits whole strings from a sequence
 * of keys that means nothing on its own. Splitting the two makes room for composition events, which real i18n needs and
 * which have nothing to say about key_code.
 *
 * Text is carried inline as UTF-32 so an event is trivially copyable and never allocates. A platform that commits more
 * than `inline_capacity` code points at once splits it across consecutive events, so a consumer simply appends `text()`
 * to its buffer and never has to reassemble anything.
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#include <catalyst/input/device.hpp>
#include <catalyst/input/keyboard.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace catalyst::input
{

    /**
     * @typedef character_code
     * @brief One Unicode code point. char32_t rather than a narrow type so that anything outside the Basic Multilingual
     * Plane - emoji, historic scripts, most CJK extensions - survives without surrogate handling in every consumer.
     */
    using character_code = char32_t;

    /**
     * @struct text_buffer
     * @brief A fixed-capacity UTF-32 buffer, shared by the two text events so neither of them allocates.
     */
    struct text_buffer
    {
        /** @brief Maximum number of code points one event can carry. */
        static constexpr std::size_t inline_capacity = 16;

        /** @brief The text as UTF-32. The view is valid for the lifetime of the buffer. */
        [[nodiscard]] constexpr std::u32string_view text() const noexcept { return {m_buffer.data(), m_length}; }
        /** @brief Number of code points in text(). */
        [[nodiscard]] constexpr std::size_t size() const noexcept { return m_length; }
        /** @brief True if there is no text. */
        [[nodiscard]] constexpr bool empty() const noexcept { return m_length == 0; }
        /** @brief True if push_back() would drop the code point. */
        [[nodiscard]] constexpr bool full() const noexcept { return m_length == inline_capacity; }

        /** @brief Replaces the text with the first inline_capacity code points of @p input. */
        constexpr void assign(std::u32string_view input) noexcept
        {
            const std::size_t n = std::min<std::size_t>(input.size(), inline_capacity);
            std::copy_n(input.data(), n, m_buffer.data());
            m_length = static_cast<std::uint8_t>(n);
        }

        /**
         * @brief Appends a code point.
         * @return False, leaving the buffer unchanged, if it is already full.
         */
        constexpr bool push_back(character_code cp) noexcept
        {
            if (full())
                return false;
            m_buffer[m_length++] = cp;
            return true;
        }

        /** @brief Removes all text. */
        constexpr void clear() noexcept { m_length = 0; }

    private:
        std::array<char32_t, inline_capacity> m_buffer{};
        std::uint8_t m_length{0};
    };

    /**
     * @struct text_input_event
     * @brief Text the user committed: one event per code point for ordinary typing, several at once for an input method
     * that commits a whole string.
     * @note Control characters - backspace, escape, tab, enter, DEL - are never delivered as text. Handle those through
     * key_event, which is the only place they mean anything.
     */
    struct text_input_event : device_event<tags::text_input>, text_buffer
    {
        text_input_event() noexcept = default;

        /** @brief An event holding the first inline_capacity code points of @p input. */
        explicit text_input_event(std::u32string_view input) noexcept { assign(input); }

        /** @brief An event holding a single code point. */
        explicit text_input_event(character_code cp) noexcept { push_back(cp); }

        /** @brief The platform::window_id of the window that received the text. */
        std::uint64_t window{0};
        /** @brief The modifiers active when the text was generated. */
        key_modifiers modifiers{key_modifiers::none};
    };

    /**
     * @enum composition_state
     * @brief Where an input method is in composing a run of text.
     */
    enum class composition_state : std::uint8_t
    {
        /** @brief Composition began; a candidate window is now up. */
        started,
        /** @brief The provisional text changed. Draw text() underlined at the caret; it is not committed yet. */
        updated,
        /** @brief Composition finished. The committed text arrives separately as a text_input_event. */
        finished,
        /** @brief The user cancelled. Discard the provisional text; no text_input_event follows. */
        cancelled
    };

    /**
     * @struct text_composition_event
     * @brief The provisional text an input method is showing before the user commits it.
     * @details An editor that ignores these still works for Latin typing - committed text arrives as text_input_event
     * either way - but shows nothing while a CJK user is mid-word, which is the difference between usable and not.
     * `cursor` and `selection_length` position the caret inside the provisional run so the editor can draw it where the
     * IME expects.
     */
    struct text_composition_event : device_event<tags::text_composition>, text_buffer
    {
        /** @brief The platform::window_id of the window that owns the composition. */
        std::uint64_t window{0};
        composition_state state{composition_state::started};
        /** @brief Caret position within text(), in code points. */
        std::uint8_t cursor{0};
        /** @brief Length of the selected clause within text(), in code points; 0 when nothing is selected. */
        std::uint8_t selection_length{0};
    };

} // namespace catalyst::input
