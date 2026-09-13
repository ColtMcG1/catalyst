/**
 * @file state.hpp
 * @brief The polled view: "is this held?" and "was it pressed this frame?", for code that would rather ask than listen.
 * @details Levels - which keys are down, where the cursor is, what an axis reads - come straight out of the registry,
 * so this class never keeps a second copy of them and can never disagree with it. What it *does* keep is edges, which
 * the registry cannot answer because they are a property of the frame rather than of the device.
 *
 * Edges are accumulated from the event stream rather than diffed against a start-of-frame snapshot, and the difference
 * matters: a key tapped and released inside one frame is invisible to a snapshot diff, and at 30fps that happens to
 * real players. So `was_pressed()` is true if a press arrived at any point since `new_frame()`, even if the key is
 * already back up.
 *
 * Call `new_frame()` once per frame, *before* pumping events:
 *
 *     in.new_frame();            // clear edges and per-frame text
 *     platform::pump_events();   // window messages -> feed -> registry -> bus -> here
 *     in.poll();                 // gamepads, HID, MIDI -> registry -> bus -> here
 *
 * Threading: not synchronised. Its listeners run on whichever thread publishes, so publish and read from one thread.
 * License: MIT (see LICENSE).
 */

#pragma once

#include <catalyst/events/token.hpp>
#include <catalyst/input/gamepad.hpp>
#include <catalyst/input/keyboard.hpp>
#include <catalyst/input/mouse.hpp>
#include <catalyst/input/registry.hpp>
#include <catalyst/input/text.hpp>
#include <catalyst/input/touch.hpp>
#include <catalyst/math/vector.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace catalyst::input
{

    /**
     * @class input_state
     * @brief Frame-oriented queries over a device_registry.
     * @details Constructed with the registry it reads and the bus it hears about changes on - normally both from
     * `input::context`, which owns one of these already and hands it out through `context::state()`. Construct your own
     * only for a second, independently-framed view (a replay window, an editor viewport).
     */
    class input_state
    {
    public:
        /** @brief Priority for the tracker's listeners; above anything an application should register, so that a
         *  consuming middleware cannot hide input from it. */
        static constexpr int default_priority = 1'000'000;

        input_state() = default;

        /** @brief Constructs and immediately attaches to @p registry and its bus. */
        explicit input_state(device_registry &registry, int priority = default_priority) { attach(registry, priority); }

        input_state(const input_state &) = delete;
        input_state &operator=(const input_state &) = delete;

        ~input_state() = default;

        /** @brief Starts tracking @p registry, detaching from any previous one first. */
        void attach(device_registry &registry, int priority = default_priority);

        /** @brief Stops tracking and forgets everything. */
        void detach() noexcept;

        /** @brief True while attached. */
        [[nodiscard]] bool attached() const noexcept { return m_registry != nullptr; }

        /** @brief The registry being read, or nullptr. */
        [[nodiscard]] device_registry *registry() const noexcept { return m_registry; }

        /**
         * @brief Clears every edge and the frame's typed text. Levels are untouched.
         * @note Call once per frame, before pumping events. Does *not* clear the registry's delta controls; that is
         * input::context::new_frame(), which calls this and then does it.
         */
        void new_frame() noexcept;

        // --------------------------------------------------------------------------------------------------------------
        // Keyboard
        // --------------------------------------------------------------------------------------------------------------

        /** @brief True while the physical key is held on any keyboard. */
        [[nodiscard]] bool is_key_down(key_code code) const noexcept;
        /** @brief True if the key went down since new_frame(). Auto-repeats do not count. */
        [[nodiscard]] bool was_key_pressed(key_code code) const noexcept;
        /** @brief True if the key came up since new_frame(). */
        [[nodiscard]] bool was_key_released(key_code code) const noexcept;
        /** @brief True if the key went down *or* auto-repeated since new_frame(). What text navigation wants. */
        [[nodiscard]] bool was_key_repeated(key_code code) const noexcept;
        /** @brief True if any key is held. */
        [[nodiscard]] bool any_key_down() const noexcept;
        /** @brief How many keys are held. */
        [[nodiscard]] std::size_t keys_down_count() const noexcept;
        /** @brief Modifier state from the most recent keyboard or mouse event. */
        [[nodiscard]] key_modifiers modifiers() const noexcept { return m_modifiers; }
        /** @brief Text committed since new_frame(), as UTF-32. */
        [[nodiscard]] std::u32string_view text() const noexcept { return m_text; }

        // --------------------------------------------------------------------------------------------------------------
        // Mouse
        // --------------------------------------------------------------------------------------------------------------

        [[nodiscard]] bool is_mouse_button_down(mouse_button b) const noexcept;
        [[nodiscard]] bool was_mouse_button_pressed(mouse_button b) const noexcept;
        [[nodiscard]] bool was_mouse_button_released(mouse_button b) const noexcept;
        /** @brief True if a press with `clicks >= 2` arrived since new_frame(). */
        [[nodiscard]] bool was_mouse_button_double_clicked(mouse_button b) const noexcept;
        /** @brief The set of held buttons. */
        [[nodiscard]] mouse_buttons mouse_buttons_down() const noexcept;
        /** @brief Cursor position in client pixels of mouse_window(). */
        [[nodiscard]] math::vec2<std::int32_t> mouse_position() const noexcept;
        /** @brief Cursor motion since new_frame(), in pixels. */
        [[nodiscard]] math::vec2<std::int32_t> mouse_delta() const noexcept;
        /** @brief Device motion since new_frame(), in counts. Non-zero only while the cursor is captured. */
        [[nodiscard]] math::vec2<std::int32_t> raw_mouse_delta() const noexcept;
        /** @brief Wheel movement since new_frame(), in notches. */
        [[nodiscard]] math::vec2<float> wheel_delta() const noexcept;
        /** @brief The window that produced the most recent mouse event, or 0. */
        [[nodiscard]] std::uint64_t mouse_window() const noexcept { return m_mouse_window; }
        /** @brief True if the cursor is inside mouse_window()'s client area. */
        [[nodiscard]] bool mouse_inside() const noexcept { return m_mouse_inside; }

        // --------------------------------------------------------------------------------------------------------------
        // Gamepads
        // --------------------------------------------------------------------------------------------------------------
        // Addressed by slot, which is what game code names: slot 0 is player one's pad whichever device_id it happens
        // to have this session.

        /** @brief True if a pad is connected in the slot. */
        [[nodiscard]] bool is_gamepad_connected(std::uint32_t slot) const noexcept;
        [[nodiscard]] bool is_gamepad_button_down(std::uint32_t slot, gamepad_button b) const noexcept;
        [[nodiscard]] bool was_gamepad_button_pressed(std::uint32_t slot, gamepad_button b) const noexcept;
        [[nodiscard]] bool was_gamepad_button_released(std::uint32_t slot, gamepad_button b) const noexcept;
        /** @brief Current axis value; 0 for an empty slot. */
        [[nodiscard]] double gamepad_axis_value(std::uint32_t slot, gamepad_axis a) const noexcept;
        /** @brief The whole pad, assembled from the registry. A disconnected state for an empty slot. */
        [[nodiscard]] gamepad_state gamepad(std::uint32_t slot) const noexcept;
        /** @brief True if a pad connected in the slot since new_frame(). */
        [[nodiscard]] bool was_gamepad_connected(std::uint32_t slot) const noexcept;
        /** @brief True if the pad in the slot disconnected since new_frame(). */
        [[nodiscard]] bool was_gamepad_disconnected(std::uint32_t slot) const noexcept;
        /** @brief The device handle for a slot, or no_device. */
        [[nodiscard]] device_id gamepad_device(std::uint32_t slot) const noexcept;

        // --------------------------------------------------------------------------------------------------------------
        // Touch
        // --------------------------------------------------------------------------------------------------------------

        /** @brief How many contacts are down. */
        [[nodiscard]] std::size_t touch_count() const noexcept;
        /** @brief True if contact @p index is down. */
        [[nodiscard]] bool is_touch_down(std::size_t index) const noexcept;
        /** @brief Contact @p index's position in client pixels; zero if it is not down. */
        [[nodiscard]] math::vec2<float> touch_position(std::size_t index) const noexcept;

        // --------------------------------------------------------------------------------------------------------------
        // Generic
        // --------------------------------------------------------------------------------------------------------------
        // The same questions for a control the module has no enum for - a HOTAS's eleventh axis, a MIDI note.

        /** @brief A control's current value. */
        [[nodiscard]] float value(device_id id, control_id control) const noexcept;
        /** @brief True while a control reads at or above @p press_point. */
        [[nodiscard]] bool is_down(device_id id, control_id control, float press_point = 0.5f) const noexcept;
        /** @brief True if a control crossed up through its press point since new_frame(). */
        [[nodiscard]] bool was_pressed(device_id id, control_id control) const noexcept;
        /** @brief True if a control crossed down through its press point since new_frame(). */
        [[nodiscard]] bool was_released(device_id id, control_id control) const noexcept;

        /** @brief The first device @p sel picks, or no_device. */
        [[nodiscard]] device_id device(const device_selector &sel) const noexcept;

    private:
        /** @brief Per-device edge flags for the current frame. Indexed by control slot. */
        struct edges
        {
            std::uint16_t generation{0};
            std::vector<std::uint8_t> pressed;
            std::vector<std::uint8_t> released;
            std::vector<std::uint8_t> repeated;
            bool connected{false};
            bool disconnected{false};
        };

        void on_control_changed(const control_changed_event &e);
        void on_device_connected(const device_connected_event &e);
        void on_device_disconnected(const device_disconnected_event &e);
        void on_key(const key_event &e);
        void on_text(const text_input_event &e);
        void on_mouse_button(const mouse_button_event &e) noexcept;
        void on_mouse_move(const mouse_move_event &e) noexcept;
        void on_mouse_enter(const mouse_enter_event &e) noexcept;
        void on_mouse_leave(const mouse_leave_event &e) noexcept;

        [[nodiscard]] edges *edges_for(device_id id) noexcept;
        [[nodiscard]] const edges *edges_for(device_id id) const noexcept;
        [[nodiscard]] bool edge_flag(device_id id, control_id control,
                                     std::vector<std::uint8_t> edges::*which) const noexcept;

        /** @brief The keyboard/mouse/touch device the un-suffixed queries read, or no_device. */
        [[nodiscard]] device_id first_of(device_kind kind) const noexcept;

        device_registry *m_registry{nullptr};
        std::vector<events::scoped_token> m_tokens;
        std::vector<edges> m_edges;

        key_modifiers m_modifiers{key_modifiers::none};
        std::u32string m_text;

        /** @brief Kind and slot of every device that went away this frame. A disconnected device cannot be looked up
         *  any more, so the slot has to be remembered here for was_gamepad_disconnected() to answer. */
        std::vector<std::pair<device_kind, std::uint32_t>> m_disconnected;

        std::uint64_t m_mouse_window{0};
        bool m_mouse_inside{false};
        /** @brief Buttons whose press this frame carried clicks >= 2. */
        mouse_buttons m_double_clicked{mouse_buttons::none};
    };

} // namespace catalyst::input
