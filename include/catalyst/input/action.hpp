/**
 * @file action.hpp
 * @brief A named thing the player can do, and the bindings that make it happen.
 * @details This is the layer the rest of the module exists to support. Game code should ask "is the player moving?",
 * not "is W down, or the left stick pushed, or the d-pad, and is this the pad we care about". An action answers the
 * first question; its bindings answer the second, and can be changed at runtime without the game code knowing.
 *
 * An action is read either way round, and both are always current:
 *
 *     if (jump.was_performed()) player.jump();          // polled, once per frame
 *     camera.turn(look.vec2() * dt);                    // polled, continuous
 *
 *     bus.add_listener<action_event>([](auto &e) {      // pushed
 *         if (e.name == "jump" && e.phase == action_phase::performed) ...
 *     });
 *
 * Phases are what let an interaction mean something. A plain button is `started` and `performed` together on the way
 * down and `cancelled` on the way up; a `hold` is `started` on the way down and `performed` only once the time has
 * elapsed, so "hold F to revive" needs no timer in the game code. An axis is `started` when it leaves rest, `performed`
 * every frame it is away from it, and `cancelled` when it comes back.
 *
 * An action with several bindings takes its value from whichever is being pushed hardest, so binding both a stick and
 * WASD to "move" behaves the way a player expects when they switch between them mid-game.
 * License: MIT (see LICENSE).
 */

#pragma once

#include <catalyst/events/bus.hpp>
#include <catalyst/input/binding.hpp>
#include <catalyst/input/registry.hpp>
#include <catalyst/math/vector.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace catalyst::input
{

    /**
     * @enum action_kind
     * @brief The shape of an action's value, fixed when it is created. Bindings of other shapes are adapted to it.
     */
    enum class action_kind : std::uint8_t
    {
        /** @brief On or off. */
        button,
        /** @brief One float, usually over [-1, 1]. */
        axis1d,
        /** @brief Two floats. Movement, look, a cursor. */
        axis2d,
        /** @brief Three floats. 6DOF controllers, spatial input. */
        axis3d
    };

    /** @brief How many components an action_kind carries. */
    [[nodiscard]] inline constexpr std::size_t action_dimensions(action_kind k) noexcept
    {
        switch (k)
        {
        case action_kind::axis2d: return 2;
        case action_kind::axis3d: return 3;
        default: return 1;
        }
    }

    /**
     * @enum action_phase
     * @brief Where an action is in its life this frame.
     */
    enum class action_phase : std::uint8_t
    {
        /** @brief Nothing is driving it. */
        idle,
        /** @brief It just began - the button went down, the stick left centre - but has not performed yet. */
        started,
        /** @brief It happened. For a plain button this is the same frame as `started`; for a hold it is later. */
        performed,
        /** @brief It began and then did not happen: released before the hold elapsed, held too long for a tap. */
        cancelled
    };

    /**
     * @struct action_value
     * @brief An action's current value, in whichever shape the action has.
     */
    struct action_value
    {
        float x{0.0f};
        float y{0.0f};
        float z{0.0f};
        action_kind kind{action_kind::button};

        /** @brief True if the value is at or above @p press_point in magnitude. */
        [[nodiscard]] bool as_bool(float press_point = 0.5f) const noexcept { return magnitude() >= press_point; }
        /** @brief The first component. */
        [[nodiscard]] float as_float() const noexcept { return x; }
        /** @brief The first two components. */
        [[nodiscard]] math::vec2<float> as_vec2() const noexcept { return {x, y}; }
        /** @brief All three components. */
        [[nodiscard]] math::vec3<float> as_vec3() const noexcept { return {x, y, z}; }

        /** @brief Length of the value, over as many components as `kind` has. */
        [[nodiscard]] float magnitude() const noexcept
        {
            switch (kind)
            {
            case action_kind::axis2d: return std::sqrt(x * x + y * y);
            case action_kind::axis3d: return std::sqrt(x * x + y * y + z * z);
            default: return std::fabs(x);
            }
        }

        /** @brief True if nothing is driving the action. */
        [[nodiscard]] bool is_zero() const noexcept { return x == 0.0f && y == 0.0f && z == 0.0f; }
    };

    /**
     * @struct action_event
     * @brief Published whenever an action changes phase.
     * @note `map` and `name` point at strings the action_map owns, so they are valid for as long as the map is - which
     * is longer than the dispatch. Copy them if you keep the event.
     */
    struct action_event : device_event<tags::action>
    {
        /** @brief The map the action belongs to. */
        std::string_view map{};
        /** @brief The action's name. */
        std::string_view name{};
        action_phase phase{action_phase::performed};
        action_value value{};
        /** @brief Which binding drove it, as an index into the action's bindings(). */
        std::uint16_t binding_index{0};
        /** @brief The control that drove it, for a rebinding screen or a "press any key" prompt. */
        control_id control{};
        /** @brief How long the action had been started when it performed. Zero for an instant one. */
        std::chrono::milliseconds elapsed{0};
    };

    /**
     * @class action
     * @brief One named action: its bindings, its current value, and its phase.
     * @details Owned by an action_map and referred to by reference; the map keeps its actions at stable addresses, so a
     * reference taken at setup stays good for the life of the map.
     */
    class action
    {
    public:
        action(std::string name, action_kind kind);

        action(const action &) = delete;
        action &operator=(const action &) = delete;

        // --------------------------------------------------------------------------------------------------------------
        // Identity
        // --------------------------------------------------------------------------------------------------------------

        [[nodiscard]] std::string_view name() const noexcept { return m_name; }
        [[nodiscard]] action_kind kind() const noexcept { return m_kind; }

        /** @brief A disabled action reads zero and never changes phase. Enable and disable are how a context switch
         *  silences a single action without touching its bindings. */
        [[nodiscard]] bool enabled() const noexcept { return m_enabled; }
        void set_enabled(bool on) noexcept;
        void enable() noexcept { set_enabled(true); }
        void disable() noexcept { set_enabled(false); }

        // --------------------------------------------------------------------------------------------------------------
        // Bindings
        // --------------------------------------------------------------------------------------------------------------

        /** @brief Adds a binding. Returns *this, so bindings chain. */
        action &bind(binding b);

        /** @brief Adds a binding from a builder. */
        action &bind(const binding_builder &b) { return bind(b.get()); }

        /** @brief Replaces binding @p index. False if there is no such binding. Used by interactive rebinding. */
        bool rebind(std::size_t index, binding b);

        /** @brief Removes binding @p index. False if there is no such binding. */
        bool unbind(std::size_t index);

        /** @brief Removes every binding. */
        void clear_bindings();

        /** @brief The bindings, in the order they were added. */
        [[nodiscard]] std::span<const binding> bindings() const noexcept { return m_bindings; }

        // --------------------------------------------------------------------------------------------------------------
        // Reading
        // --------------------------------------------------------------------------------------------------------------

        /** @brief The current value. */
        [[nodiscard]] action_value value() const noexcept { return m_value; }
        /** @brief The current phase. */
        [[nodiscard]] action_phase phase() const noexcept { return m_phase; }

        /** @brief True while the action is being driven - the button is down, the stick is off centre. */
        [[nodiscard]] bool is_held() const noexcept { return m_held; }
        /** @brief The value's first component. */
        [[nodiscard]] float as_float() const noexcept { return m_value.as_float(); }
        /** @brief The value's first two components. */
        [[nodiscard]] math::vec2<float> vec2() const noexcept { return m_value.as_vec2(); }
        /** @brief All three components. */
        [[nodiscard]] math::vec3<float> vec3() const noexcept { return m_value.as_vec3(); }

        /** @brief True if the action started this frame. */
        [[nodiscard]] bool was_started() const noexcept { return m_started_this_frame; }
        /** @brief True if the action performed this frame. The one most game code wants. */
        [[nodiscard]] bool was_performed() const noexcept { return m_performed_this_frame; }
        /** @brief True if the action was cancelled this frame - a hold released early, a tap held too long. */
        [[nodiscard]] bool was_cancelled() const noexcept { return m_cancelled_this_frame; }

        /** @brief The device that last drove the action, or no_device. */
        [[nodiscard]] device_id source_device() const noexcept { return m_source_device; }
        /** @brief The control that last drove the action, or no_control. */
        [[nodiscard]] control_id source_control() const noexcept { return m_source_control; }

        /** @brief How long the action has been held; zero when it is not. Drives a hold's progress bar. */
        [[nodiscard]] std::chrono::milliseconds held_for(input_time now) const noexcept;

        // --------------------------------------------------------------------------------------------------------------
        // Driving
        // --------------------------------------------------------------------------------------------------------------
        // Called by action_map; public so an action can be driven standalone in a test.

        /** @brief Clears the frame edges. Value, phase and interaction timing are kept. */
        void new_frame() noexcept;

        /**
         * @brief Re-reads every binding and updates the value, the phase and the frame edges.
         * @param registry Where control values come from.
         * @param now The frame's timestamp. Passed in rather than read here so a replay can drive actions off its own
         * clock and get identical results.
         * @param bus Where to publish action_event, or nullptr to evaluate silently.
         * @param map_name Goes into the published events.
         * @param suppressed Primary controls currently claimed by an active chord elsewhere in the map; a binding on one
         * of them with no modifiers of its own produces nothing this frame.
         */
        void evaluate(const device_registry &registry, input_time now, events::bus *bus,
                      std::string_view map_name, std::span<const control_id> suppressed);

        /** @brief The primary controls of every binding whose chord is currently satisfied. Fills the `suppressed` list
         *  action_map passes back into evaluate(). */
        void collect_active_chords(const device_registry &registry, std::vector<control_id> &out) const;

    private:
        /** @brief Per-binding interaction state, which is the only thing in an action that has memory. */
        struct binding_state
        {
            bool down{false};
            input_time down_at{};
            /** @brief True once this press has performed, so a hold performs once rather than every frame. */
            bool performed{false};
            /** @brief True while the interaction is between started and performed/cancelled. */
            bool started{false};
            std::uint8_t taps{0};
            input_time last_tap{};
        };

        struct evaluation
        {
            action_value value{};
            float magnitude{0.0f};
            bool down{false};
            control_id control{};
            device_id device{};
        };

        [[nodiscard]] evaluation read_binding(const device_registry &registry, const binding &b) const;
        [[nodiscard]] static float read_part(const device_registry &registry, const binding_part &p, device_id &device_out);
        [[nodiscard]] static bool modifiers_satisfied(const device_registry &registry, const binding &b);
        [[nodiscard]] action_value shape_to_kind(const action_value &in, std::size_t source_dims) const noexcept;

        void emit(events::bus *bus, std::string_view map_name, action_phase phase,
                  std::size_t binding_index, control_id control, device_id device,
                  std::chrono::milliseconds elapsed, input_time now);

        std::string m_name;
        action_kind m_kind{action_kind::button};
        bool m_enabled{true};

        std::vector<binding> m_bindings;
        std::vector<binding_state> m_states;

        action_value m_value{};
        action_phase m_phase{action_phase::idle};
        bool m_held{false};
        bool m_started_this_frame{false};
        bool m_performed_this_frame{false};
        bool m_cancelled_this_frame{false};

        device_id m_source_device{};
        control_id m_source_control{};
        input_time m_held_since{};
    };

} // namespace catalyst::input
