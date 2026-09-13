/**
 * @file binding.hpp
 * @brief What an action reads from: which controls, on which devices, put through which processors.
 * @details A binding is a small, copyable, comparable description - no pointers into the registry, no device handles
 * unless you asked for one - which is what lets it be written to a config file, shown in a rebinding screen, and
 * compared for conflicts.
 *
 * Four shapes cover everything:
 *
 *     control   one control                      key(key_code::space)
 *     axis1d    two buttons, or one axis         compose1d().negative(key_code::a).positive(key_code::d)
 *     axis2d    four buttons, or two axes        stick(gamepad_axis::left_x, gamepad_axis::left_y)
 *     axis3d    six buttons, or three axes       for a 6DOF controller
 *
 * Each part carries its own processors, because a stick and a set of WASD keys bound to the same action need different
 * treatment: the stick wants a dead zone, the keys do not.
 *
 * An *interaction* turns a control's timing into meaning - hold to sprint, double-tap to dodge - and is the one place
 * where the binding layer needs a clock. Without one, a button binding is performed the moment it goes down.
 *
 * The builders read as prose and are the intended way in:
 *
 *     using namespace catalyst::input::bind;
 *     jump.bind(key(key_code::space));
 *     jump.bind(pad(gamepad_button::a));
 *     sprint.bind(key(key_code::left_shift).hold(200ms));
 *     fire.bind(pad(gamepad_axis::right_trigger).press_point(0.4f));
 *     look.bind(mouse_delta().scale(0.05f));
 *     move.bind(stick(gamepad_axis::left_x, gamepad_axis::left_y).deadzone(0.2f));
 *     move.bind(compose2d().up(key_code::w).down(key_code::s).left(key_code::a).right(key_code::d));
 *     screenshot.bind(key(key_code::p).with(key_code::left_control));
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#include <catalyst/input/device.hpp>
#include <catalyst/input/gamepad.hpp>
#include <catalyst/input/joystick.hpp>
#include <catalyst/input/keyboard.hpp>
#include <catalyst/input/midi.hpp>
#include <catalyst/input/mouse.hpp>
#include <catalyst/input/pen.hpp>
#include <catalyst/input/touch.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace catalyst::input
{

    // ------------------------------------------------------------------------------------------------------------------
    // Processors
    // ------------------------------------------------------------------------------------------------------------------

    /**
     * @struct processor_chain
     * @brief The transformations applied to one control's raw value, in a fixed order.
     * @details Fixed order rather than an arbitrary list of processor objects, because the order that makes sense is
     * always the same one - dead zone, then curve, then scale, then invert - and a fixed struct stays trivially
     * copyable, comparable, and serializable, which an array of polymorphic processors would not.
     */
    struct processor_chain
    {
        /**
         * @brief Magnitudes at or below this read as 0, and the remaining range is rescaled so the output still reaches
         * 1 rather than jumping at the boundary.
         */
        float deadzone{0.0f};
        /** @brief Magnitudes at or above this read as 1. Trims a stick that cannot quite reach its corners. */
        float saturation{1.0f};
        /** @brief Applied to the magnitude after the dead zone: 1 is linear, 2 gives fine control near centre. */
        float curve{1.0f};
        /** @brief Multiplies the result. This is where mouse sensitivity lives. */
        float scale{1.0f};
        /** @brief Negates the result. Bound to "invert Y" in every options menu ever shipped. */
        bool invert{false};
        /** @brief The value at which a control counts as pressed, when an action reads it as a button. */
        float press_point{0.5f};

        /**
         * @brief Applies the chain to a raw value.
         * @param bounded True for a control with a natural [0, 1] or [-1, 1] range - a button, a stick, a trigger.
         * False for one without - a mouse delta, a wheel notch, a pixel coordinate. An unbounded value skips the dead
         * zone, saturation and curve entirely and takes only the scale and the inversion, because normalising it would
         * make a fast flick of the mouse indistinguishable from a slow one.
         */
        [[nodiscard]] float apply(float value, bool bounded = true) const noexcept;

        /**
         * @brief Applies the chain to a vector, treating dead zone, saturation and curve *radially*.
         * @details Radially rather than per-component, so a stick pushed diagonally is not clipped into a square - the
         * bug that makes a character walk faster on the diagonal. It is also what normalises a two-button composite:
         * W and D together are (1, 1), which saturates back to length 1.
         * @param bounded As for apply().
         */
        void apply_radial(float &x, float &y, bool bounded = true) const noexcept;
        /** @brief The three-component form of apply_radial(). */
        void apply_radial(float &x, float &y, float &z, bool bounded = true) const noexcept;

        [[nodiscard]] friend bool operator==(const processor_chain &, const processor_chain &) noexcept = default;
    };

    // ------------------------------------------------------------------------------------------------------------------
    // Interactions
    // ------------------------------------------------------------------------------------------------------------------

    /**
     * @enum interaction_kind
     * @brief How a control's timing becomes meaning.
     */
    enum class interaction_kind : std::uint8_t
    {
        /** @brief Performed the moment the control goes down, and every frame it stays down for an axis. */
        none,
        /** @brief Performed on the way down only, once per press. */
        press,
        /** @brief Performed on the way up. */
        release,
        /** @brief Performed once the control has been held for `duration`. Cancelled if released before then. */
        hold,
        /** @brief Performed on release, if the control was down for less than `duration`. Cancelled otherwise. */
        tap,
        /** @brief Performed on the `count`-th tap, if each falls within `duration` of the last. */
        multi_tap
    };

    /**
     * @struct interaction
     * @brief An interaction_kind and its timing.
     */
    struct interaction
    {
        interaction_kind kind{interaction_kind::none};
        /** @brief Hold time, maximum tap length, or the window between taps, depending on `kind`. */
        std::chrono::milliseconds duration{400};
        /** @brief Taps required, for interaction_kind::multi_tap. Ignored otherwise. */
        std::uint8_t count{2};

        [[nodiscard]] friend bool operator==(const interaction &, const interaction &) noexcept = default;
    };

    // ------------------------------------------------------------------------------------------------------------------
    // Bindings
    // ------------------------------------------------------------------------------------------------------------------

    /**
     * @enum binding_shape
     * @brief How a binding's parts combine into a value.
     */
    enum class binding_shape : std::uint8_t
    {
        /** @brief One part; its value is the binding's value. */
        control,
        /** @brief Two parts, negative then positive. Either two buttons, or one axis in the first part. */
        axis1d,
        /** @brief Four parts - up, down, left, right - or two axis parts, x then y. */
        axis2d,
        /** @brief Six parts, or three axis parts. */
        axis3d
    };

    /**
     * @struct binding_part
     * @brief One control a binding reads, and how to treat it.
     */
    struct binding_part
    {
        /** @brief Which device. Defaults to "any device of any kind", which a builder always narrows. */
        device_selector device{};
        control_id control{};
        processor_chain processors{};

        [[nodiscard]] friend bool operator==(const binding_part &, const binding_part &) noexcept = default;
    };

    /**
     * @struct binding
     * @brief A complete description of where one of an action's values comes from.
     */
    struct binding
    {
        binding_shape shape{binding_shape::control};
        /**
         * @brief The controls read, in the order the shape defines. For a composite shape, either one part per
         * direction (buttons) or one per dimension (axes) - `composite` says which.
         */
        std::vector<binding_part> parts;
        /**
         * @brief True when a composite shape's parts are buttons standing in for directions, false when they are axes.
         * Meaningless for binding_shape::control.
         */
        bool composite{false};
        /**
         * @brief Controls that must all be down for the binding to produce anything - the Ctrl in Ctrl+S.
         * @note A chord also suppresses the *unmodified* binding while it is held, so binding "S" and "Ctrl+S" to
         * different actions does what the user means rather than firing both.
         */
        std::vector<binding_part> modifiers;
        interaction interact{};
        /** @brief Set by an interactive rebind so a bindings screen can tell overrides from defaults. */
        bool user_override{false};

        /** @brief How many parts this shape expects, given `composite`. */
        [[nodiscard]] std::size_t expected_parts() const noexcept;

        /** @brief True if the binding names enough valid controls to be evaluated. */
        [[nodiscard]] bool valid() const noexcept;

        [[nodiscard]] friend bool operator==(const binding &, const binding &) noexcept = default;
    };

    /** @brief The dimensionality a shape produces: 1, 2 or 3. */
    [[nodiscard]] inline constexpr std::size_t binding_dimensions(binding_shape s) noexcept
    {
        switch (s)
        {
        case binding_shape::axis2d: return 2;
        case binding_shape::axis3d: return 3;
        default: return 1;
        }
    }

    // ------------------------------------------------------------------------------------------------------------------
    // Builders
    // ------------------------------------------------------------------------------------------------------------------

    /**
     * @class binding_builder
     * @brief Chainable construction of a `binding`. Converts to one implicitly, so it is never named in user code.
     */
    class binding_builder
    {
    public:
        explicit binding_builder(binding b) noexcept : m_binding(std::move(b)) {}

        /** @brief The finished binding. */
        [[nodiscard]] operator binding() const & { return m_binding; }
        [[nodiscard]] operator binding() && { return std::move(m_binding); }
        [[nodiscard]] const binding &get() const noexcept { return m_binding; }

        // ---- device narrowing ----

        /** @brief Restricts every part to the device in @p slot - "player 2's pad". */
        binding_builder &slot(std::uint32_t slot) noexcept;
        /** @brief Restricts every part to one specific device. */
        binding_builder &device(device_id id) noexcept;

        // ---- processors, applied to every part ----

        /** @brief Sets the dead zone. On a composite or a stick it is applied radially. */
        binding_builder &deadzone(float threshold) noexcept;
        /** @brief Sets the saturation point: magnitudes at or above it read as 1. */
        binding_builder &saturation(float threshold) noexcept;
        /** @brief Sets the response curve exponent. 1 is linear; 2 gives finer control near centre. */
        binding_builder &curve(float exponent) noexcept;
        /** @brief Multiplies the value. Mouse sensitivity, trigger gain. */
        binding_builder &scale(float factor) noexcept;
        /** @brief Negates the value. */
        binding_builder &invert(bool on = true) noexcept;
        /** @brief Sets the value at which the control counts as pressed. */
        binding_builder &press_point(float point) noexcept;

        // ---- interactions ----

        /** @brief Perform on the way down only. */
        binding_builder &on_press() noexcept;
        /** @brief Perform on the way up. */
        binding_builder &on_release() noexcept;
        /** @brief Perform once held for @p duration; cancel if released first. */
        binding_builder &hold(std::chrono::milliseconds duration) noexcept;
        /** @brief Perform on release, if held for less than @p duration. */
        binding_builder &tap(std::chrono::milliseconds max_duration = std::chrono::milliseconds(200)) noexcept;
        /** @brief Perform on the @p count-th tap, each within @p window of the last. */
        binding_builder &multi_tap(std::uint8_t count = 2,
                                   std::chrono::milliseconds window = std::chrono::milliseconds(300)) noexcept;

        // ---- chords ----

        /** @brief Requires a key to be held as well. Repeat for several. */
        binding_builder &with(key_code code);
        /** @brief Requires a gamepad button to be held as well. */
        binding_builder &with(gamepad_button button);
        /** @brief Requires an arbitrary control to be held as well. */
        binding_builder &with(device_selector device, control_id control);

        /** @brief Marks the binding as a user override rather than a default. */
        binding_builder &as_override(bool on = true) noexcept;

    private:
        binding m_binding;
    };

    /**
     * @namespace catalyst::input::bind
     * @brief The binding builders. `using namespace catalyst::input::bind;` at the point of use is intended.
     */
    namespace bind
    {
        /** @brief Any control on any device matching @p device. The escape hatch for hardware with no named enum. */
        [[nodiscard]] binding_builder control(device_selector device, control_id control);

        // ---- keyboard ----

        /** @brief A physical key on any keyboard. */
        [[nodiscard]] binding_builder key(key_code code);

        // ---- mouse ----

        /** @brief A mouse button. */
        [[nodiscard]] binding_builder mouse(mouse_button button);
        /** @brief One mouse axis on its own. */
        [[nodiscard]] binding_builder pointer(mouse_axis axis);
        /** @brief Cursor motion as a 2D axis. What a look or camera-pan action wants. */
        [[nodiscard]] binding_builder mouse_delta();
        /** @brief Unaccelerated device motion as a 2D axis. What a captured-cursor FPS camera wants. */
        [[nodiscard]] binding_builder mouse_raw_delta();
        /** @brief Wheel movement as a 2D axis; bind `.y` for ordinary scrolling. */
        [[nodiscard]] binding_builder wheel();

        // ---- gamepad ----

        /** @brief A gamepad button on any pad. */
        [[nodiscard]] binding_builder pad(gamepad_button button);
        /** @brief A single gamepad axis - a trigger, or one half of a stick. */
        [[nodiscard]] binding_builder pad(gamepad_axis axis);
        /** @brief A pair of gamepad axes as one 2D axis, with a radial dead zone. */
        [[nodiscard]] binding_builder stick(gamepad_axis x, gamepad_axis y);
        /** @brief The left stick. */
        [[nodiscard]] binding_builder left_stick();
        /** @brief The right stick. */
        [[nodiscard]] binding_builder right_stick();
        /** @brief The d-pad's four buttons as one 2D axis. */
        [[nodiscard]] binding_builder dpad();

        // ---- generic HID ----

        /** @brief Button @p n on any joystick. */
        [[nodiscard]] binding_builder joystick_button(std::size_t n);
        /** @brief Axis @p n on any joystick. */
        [[nodiscard]] binding_builder joystick_axis(std::size_t n);
        /** @brief Hat @p n as one 2D axis. */
        [[nodiscard]] binding_builder joystick_hat(std::size_t n);

        // ---- touch and pen ----

        /** @brief Contact @p index's down flag. */
        [[nodiscard]] binding_builder touch(std::size_t index = 0);
        /** @brief Contact @p index's position as a 2D axis. */
        [[nodiscard]] binding_builder touch_position(std::size_t index = 0);
        /** @brief A stylus button. */
        [[nodiscard]] binding_builder pen(pen_button button);
        /** @brief A stylus axis - pressure, tilt, twist. */
        [[nodiscard]] binding_builder pen(pen_axis axis);

        // ---- MIDI ----

        /** @brief A MIDI note's velocity, on any port. */
        [[nodiscard]] binding_builder note(std::uint8_t note);
        /** @brief A MIDI continuous controller - 1 is the mod wheel, 64 the sustain pedal. */
        [[nodiscard]] binding_builder cc(std::uint8_t controller);

        // ---- composites ----

        /**
         * @class axis1d_builder
         * @brief Two buttons standing in for one axis. Both down reads as 0.
         */
        class axis1d_builder
        {
        public:
            axis1d_builder();
            /** @brief The control driving the axis negative. */
            axis1d_builder &negative(key_code code);
            axis1d_builder &negative(gamepad_button button);
            axis1d_builder &negative(device_selector device, control_id control);
            /** @brief The control driving the axis positive. */
            axis1d_builder &positive(key_code code);
            axis1d_builder &positive(gamepad_button button);
            axis1d_builder &positive(device_selector device, control_id control);

            [[nodiscard]] operator binding() const;
            /** @brief Continues with the processor and interaction methods. */
            [[nodiscard]] binding_builder done() const;

        private:
            void set(std::size_t index, device_selector device, control_id control);
            binding m_binding;
        };

        /**
         * @class axis2d_builder
         * @brief Four buttons standing in for a 2D axis - WASD, arrow keys, a d-pad. Opposite pairs cancel.
         */
        class axis2d_builder
        {
        public:
            axis2d_builder();
            axis2d_builder &up(key_code code);
            axis2d_builder &down(key_code code);
            axis2d_builder &left(key_code code);
            axis2d_builder &right(key_code code);
            axis2d_builder &up(gamepad_button button);
            axis2d_builder &down(gamepad_button button);
            axis2d_builder &left(gamepad_button button);
            axis2d_builder &right(gamepad_button button);
            axis2d_builder &up(device_selector device, control_id control);
            axis2d_builder &down(device_selector device, control_id control);
            axis2d_builder &left(device_selector device, control_id control);
            axis2d_builder &right(device_selector device, control_id control);

            /** @brief The four arrow keys. */
            axis2d_builder &arrows();
            /** @brief W, A, S and D. */
            axis2d_builder &wasd();

            [[nodiscard]] operator binding() const;
            [[nodiscard]] binding_builder done() const;

        private:
            void set(std::size_t index, device_selector device, control_id control);
            binding m_binding;
        };

        /** @brief Begins a two-button 1D composite. */
        [[nodiscard]] axis1d_builder compose1d();
        /** @brief Begins a four-button 2D composite. */
        [[nodiscard]] axis2d_builder compose2d();

    } // namespace bind

} // namespace catalyst::input
