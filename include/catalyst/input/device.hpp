/**
 * @file device.hpp
 * @brief The device and control model every other header in the module is expressed in.
 * @details A device is a flat array of float controls plus metadata naming them. That is the whole
 * model, and it is what lets the module carry a keyboard, a pen and an eleven-axis HOTAS without an
 * API per device family:
 *
 *   - A button is a control in [0, 1]. Usually 0 or 1, but analog when the hardware is - which is
 *     why an Xbox trigger is a button *and* an axis with no special case anywhere.
 *   - A stick is two adjacent controls. A sensor's orientation is four. A touch contact is five.
 *   - Every named enum in this module (key_code, gamepad_axis, mouse_button, ...) is a *name for a
 *     slot index* on its device kind, produced by a `control_of()` overload in that kind's header.
 *
 * So the common case reads exactly as it always did - `control_of(gamepad_axis::left_x)` - while the
 * binding layer below only ever sees `control_id`, and therefore works with hardware nobody wrote an
 * enum for. A device whose layout is discovered at runtime (anything behind Raw Input) builds the
 * same `device_layout` a built-in kind does; nothing downstream can tell the difference.
 *
 * `device_id` is an index plus a generation, so a handle kept across a disconnect is detected rather
 * than silently aliasing whatever took the slot next.
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#include <catalyst/events/tag.hpp>
#include <catalyst/input/hid.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace catalyst::input
{

    // ------------------------------------------------------------------------------------------------------------------
    // Time
    // ------------------------------------------------------------------------------------------------------------------

    /** @brief The clock every input event is stamped with. Steady, because input timing is about intervals. */
    using input_clock = std::chrono::steady_clock;

    /** @brief When an input event happened. */
    using input_time = input_clock::time_point;

    // ------------------------------------------------------------------------------------------------------------------
    // Device identity
    // ------------------------------------------------------------------------------------------------------------------

    /**
     * @struct device_id
     * @brief A handle to a device: a slot index plus the generation that occupied it.
     * @details A generation of 0 is the null handle. Because the generation moves on every disconnect, a handle held
     * across one is detected by device_registry rather than resolving to whatever plugged in afterwards.
     */
    struct device_id
    {
        std::uint16_t index{0};
        std::uint16_t generation{0};

        [[nodiscard]] constexpr bool valid() const noexcept { return generation != 0; }
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return valid(); }

        /** @brief The handle as one integer, for hashing or for logging. */
        [[nodiscard]] constexpr std::uint32_t bits() const noexcept
        {
            return (static_cast<std::uint32_t>(generation) << 16) | index;
        }

        [[nodiscard]] friend constexpr bool operator==(device_id, device_id) noexcept = default;
    };

    /** @brief The null device handle. */
    inline constexpr device_id no_device{};

    /**
     * @enum device_kind
     * @brief What sort of device this is. Determines which header's `control_of()` overloads name its slots.
     */
    enum class device_kind : std::uint8_t
    {
        /** @brief Matches every kind when used in a device_selector. */
        any = 0,
        keyboard,
        mouse,
        gamepad,
        /** @brief Anything Generic-Desktop that is not a gamepad: sticks, wheels, HOTAS, throttle quadrants. */
        joystick,
        touchscreen,
        pen,
        midi,
        sensor,
        /** @brief Synthesised by the application - a replay, a network peer, an on-screen pad. */
        simulated
    };

    /** @brief Number of values in device_kind, including `any`. */
    inline constexpr std::size_t device_kind_count = 10;

    /** @brief A short, stable name for a device kind ("gamepad", "touchscreen", ...). */
    [[nodiscard]] std::string_view device_kind_name(device_kind kind) noexcept;

    /**
     * @enum device_flags
     * @brief Optional capabilities a device may advertise. Bindings never need these; UI and haptics do.
     */
    enum class device_flags : std::uint16_t
    {
        none = 0,
        /** @brief Accepts set_rumble(); see gamepad.hpp. */
        rumble = 1u << 0,
        /** @brief Reports a continuous pressure control (pen, force-touch trackpad). */
        pressure = 1u << 1,
        /** @brief Reports tilt and/or twist (pen). */
        tilt = 1u << 2,
        /** @brief Buttons report analog travel rather than only 0 and 1. */
        analog_buttons = 1u << 3,
        /** @brief The layout was discovered at runtime rather than being one of the built-in ones. */
        discovered_layout = 1u << 4,
        /** @brief Values are produced by the application, not by hardware. */
        simulated = 1u << 5
    };

    [[nodiscard]] inline constexpr device_flags operator|(device_flags a, device_flags b) noexcept
    {
        using u = std::underlying_type_t<device_flags>;
        return static_cast<device_flags>(static_cast<u>(a) | static_cast<u>(b));
    }
    [[nodiscard]] inline constexpr device_flags operator&(device_flags a, device_flags b) noexcept
    {
        using u = std::underlying_type_t<device_flags>;
        return static_cast<device_flags>(static_cast<u>(a) & static_cast<u>(b));
    }
    inline constexpr device_flags &operator|=(device_flags &a, device_flags b) noexcept { return a = (a | b); }
    inline constexpr device_flags &operator&=(device_flags &a, device_flags b) noexcept { return a = (a & b); }

    /** @brief True if @p set contains every bit of @p flag. */
    [[nodiscard]] inline constexpr bool has_flag(device_flags set, device_flags flag) noexcept
    {
        return (set & flag) == flag && flag != device_flags::none;
    }

    /** @brief Slot value meaning "this device has no slot", and, in a device_selector, "any slot". */
    inline constexpr std::uint32_t any_slot = 0xFFFF'FFFFu;

    /**
     * @struct device_info
     * @brief Everything about a device that is not a control value. Filled in by whoever creates the device.
     */
    struct device_info
    {
        device_kind kind{device_kind::any};
        /** @brief Display name, e.g. "Xbox Controller". Never empty once the device is registered. */
        std::string name;
        std::string manufacturer;
        /** @brief Serial or instance path, when the backend can supply one. Used to re-pair a device to a player. */
        std::string serial;
        std::uint16_t vendor_id{0};
        std::uint16_t product_id{0};
        /**
         * @brief The backend's slot for the device - the XInput user index, the MIDI port. Stable while the device stays
         * connected, and what a device_selector matches on when an application wants "player 2's pad". `any_slot` when
         * the device kind has no such notion.
         */
        std::uint32_t slot{any_slot};
        device_flags flags{device_flags::none};
    };

    // ------------------------------------------------------------------------------------------------------------------
    // Controls
    // ------------------------------------------------------------------------------------------------------------------

    /**
     * @enum control_kind
     * @brief The natural range and meaning of a control's float value. The binding layer reads this to decide what a
     * dead zone or a press point means for it; nothing else in the module branches on it.
     */
    enum class control_kind : std::uint8_t
    {
        /** @brief [0, 1]. Down at or above the binding's press point (0.5 by default). */
        button,
        /** @brief [-1, 1], resting at 0. Sticks, steering. */
        axis,
        /** @brief [0, 1], resting at 0. Triggers, pedals, pen pressure, MIDI velocity. */
        ratio,
        /** @brief Unbounded relative motion accumulated over the frame. Mouse and wheel deltas. */
        delta,
        /** @brief Unbounded absolute position. Cursor and touch coordinates, in pixels. */
        absolute
    };

    /** @brief The value a control of this kind reads when nothing is touching it. */
    [[nodiscard]] inline constexpr float control_rest_value(control_kind) noexcept { return 0.0f; }

    /**
     * @struct control_id
     * @brief Which slot on a device a value lives in. Relative to the device's kind, so the same index means different
     * things on a keyboard and on a gamepad; a binding always pins the kind alongside it.
     */
    struct control_id
    {
        static constexpr std::uint16_t invalid_index = 0xFFFFu;

        std::uint16_t index{invalid_index};

        [[nodiscard]] constexpr bool valid() const noexcept { return index != invalid_index; }
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return valid(); }

        [[nodiscard]] friend constexpr bool operator==(control_id, control_id) noexcept = default;
        [[nodiscard]] friend constexpr auto operator<=>(control_id, control_id) noexcept = default;
    };

    /** @brief The null control handle. */
    inline constexpr control_id no_control{};

    /** @brief Builds a control_id from a raw slot index. Named so that a bare integer never turns into one by accident. */
    [[nodiscard]] inline constexpr control_id control_at(std::size_t index) noexcept
    {
        return control_id{static_cast<std::uint16_t>(index)};
    }

    /**
     * @struct control_info
     * @brief One control's metadata: what it is, and what to call it in a bindings screen.
     */
    struct control_info
    {
        control_kind kind{control_kind::button};
        /** @brief Display name, e.g. "Left Stick X". Points at static storage for built-in layouts. */
        std::string_view name{};
        /** @brief The USB HID usage the control came from, or usb_hid_unknown. Set for discovered layouts. */
        usb_hid usage{usb_hid_unknown};
    };

    /**
     * @class device_layout
     * @brief The ordered list of controls a device kind exposes.
     * @details Built-in kinds each have one shared layout, returned by the `layout()` function in their header and
     * handed out as a shared_ptr so every keyboard points at the same object. A device behind Raw Input builds its own
     * at connect time from the report descriptor; from here on nothing can tell the two apart, which is the point.
     */
    class device_layout
    {
    public:
        device_layout() = default;

        /** @brief A layout over controls with static storage duration (the built-in kinds). */
        device_layout(device_kind kind, std::span<const control_info> controls) noexcept
            : m_kind(kind), m_static(controls) {}

        /** @brief A layout that owns its control list (a discovered device). */
        device_layout(device_kind kind, std::vector<control_info> controls) noexcept
            : m_kind(kind), m_owned(std::move(controls)) {}

        [[nodiscard]] device_kind kind() const noexcept { return m_kind; }

        /** @brief The controls, in slot order. */
        [[nodiscard]] std::span<const control_info> controls() const noexcept
        {
            return m_owned.empty() ? m_static : std::span<const control_info>(m_owned);
        }

        /** @brief Number of slots a device with this layout has. */
        [[nodiscard]] std::size_t size() const noexcept { return controls().size(); }

        /** @brief True if @p c names a slot in this layout. */
        [[nodiscard]] bool contains(control_id c) const noexcept { return c.valid() && c.index < size(); }

        /** @brief Metadata for a slot, or a default-constructed control_info for one this layout does not have. */
        [[nodiscard]] control_info control(control_id c) const noexcept
        {
            return contains(c) ? controls()[c.index] : control_info{};
        }

        /** @brief The kind of a slot; control_kind::button for a slot this layout does not have. */
        [[nodiscard]] control_kind kind_of(control_id c) const noexcept { return control(c).kind; }

        /** @brief The display name of a slot, or an empty view. */
        [[nodiscard]] std::string_view name_of(control_id c) const noexcept { return control(c).name; }

        /** @brief The first slot whose USB HID usage is @p usage, or no_control. */
        [[nodiscard]] control_id find_usage(usb_hid usage) const noexcept;

        /** @brief The first slot whose display name is @p name, or no_control. Used when loading bindings from JSON. */
        [[nodiscard]] control_id find_name(std::string_view name) const noexcept;

    private:
        device_kind m_kind{device_kind::any};
        std::span<const control_info> m_static{};
        std::vector<control_info> m_owned{};
    };

    /** @brief A layout shared by every device that has it. */
    using layout_ref = std::shared_ptr<const device_layout>;

    // ------------------------------------------------------------------------------------------------------------------
    // Selecting devices
    // ------------------------------------------------------------------------------------------------------------------

    /**
     * @struct device_selector
     * @brief Which device (or devices) a binding reads from.
     * @details Three levels of precision, in order of how often they are wanted:
     *
     *     device_selector::of(device_kind::gamepad)     any gamepad - the default for a single-player game
     *     device_selector::of(device_kind::gamepad, 1)  the pad in slot 1 - local multiplayer
     *     device_selector::exact(id)                    that one device, whatever it is
     *
     * `kind == device_kind::any` matches every kind, which is how "the next control anyone touches" is expressed during
     * interactive rebinding.
     */
    struct device_selector
    {
        device_kind kind{device_kind::any};
        std::uint32_t slot{any_slot};
        /** @brief When valid, the only thing that is matched; kind and slot are ignored. */
        device_id id{};

        [[nodiscard]] static constexpr device_selector of(device_kind k, std::uint32_t s = any_slot) noexcept
        {
            return device_selector{k, s, no_device};
        }

        [[nodiscard]] static constexpr device_selector exact(device_id d) noexcept
        {
            return device_selector{device_kind::any, any_slot, d};
        }

        /** @brief True if a device with this identity is one this selector picks. */
        [[nodiscard]] constexpr bool matches(device_id d, const device_kind k, std::uint32_t s) const noexcept
        {
            if (id.valid())
                return id == d;
            if (kind != device_kind::any && kind != k)
                return false;
            return slot == any_slot || slot == s;
        }

        [[nodiscard]] friend constexpr bool operator==(const device_selector &, const device_selector &) noexcept = default;
    };

    // ------------------------------------------------------------------------------------------------------------------
    // Events
    // ------------------------------------------------------------------------------------------------------------------

    /**
     * @enum button_action
     * @brief What happened to a button. Shared by every device kind: the old module spelled this three different ways
     * (key_action, mouse_button_action, and a bare bool) for no reason anyone could name.
     * @note `repeat` is only ever produced by a keyboard, where the platform's auto-repeat generates it. Consumers that
     * do not care about repeats should test for `press`; those driving text navigation want `press` or `repeat`.
     */
    enum class button_action : std::uint8_t
    {
        press,
        release,
        repeat
    };

    /** @brief True for the actions that mean the button is down afterwards. */
    [[nodiscard]] inline constexpr bool is_down_action(button_action a) noexcept
    {
        return a != button_action::release;
    }

    /**
     * @namespace catalyst::input::tags
     * @brief Static event tags for this module, so the bus dispatches on a constant instead of `typeid`.
     * @details The input module owns the block 0x0001'0000 - 0x0001'FFFF. Values are never reused or reordered: a
     * binding file or a recorded session may name one.
     */
    namespace tags
    {
        inline constexpr events::event_type_t input_base = 0x0001'0000u;

        inline constexpr events::event_type_t device_connected = input_base + 0x00;
        inline constexpr events::event_type_t device_disconnected = input_base + 0x01;
        inline constexpr events::event_type_t control_changed = input_base + 0x02;

        inline constexpr events::event_type_t key = input_base + 0x10;
        inline constexpr events::event_type_t text_input = input_base + 0x11;
        inline constexpr events::event_type_t text_composition = input_base + 0x12;

        inline constexpr events::event_type_t mouse_move = input_base + 0x20;
        inline constexpr events::event_type_t mouse_button = input_base + 0x21;
        inline constexpr events::event_type_t mouse_wheel = input_base + 0x22;
        inline constexpr events::event_type_t mouse_enter = input_base + 0x23;
        inline constexpr events::event_type_t mouse_leave = input_base + 0x24;
        inline constexpr events::event_type_t mouse_raw_move = input_base + 0x25;

        inline constexpr events::event_type_t gamepad_button = input_base + 0x30;
        inline constexpr events::event_type_t gamepad_axis = input_base + 0x31;

        inline constexpr events::event_type_t joystick_button = input_base + 0x40;
        inline constexpr events::event_type_t joystick_axis = input_base + 0x41;
        inline constexpr events::event_type_t joystick_hat = input_base + 0x42;

        inline constexpr events::event_type_t touch = input_base + 0x50;

        inline constexpr events::event_type_t pen = input_base + 0x60;
        inline constexpr events::event_type_t pen_button = input_base + 0x61;

        inline constexpr events::event_type_t midi = input_base + 0x70;

        inline constexpr events::event_type_t action = input_base + 0x80;
    } // namespace tags

    /**
     * @struct device_event
     * @brief The two fields every input event carries. Inherited rather than repeated fifteen times.
     * @tparam Tag The event's static tag; see catalyst::input::tags.
     */
    template <events::event_type_t Tag>
    struct device_event : events::tagged<Tag>
    {
        /** @brief The device the event came from. Never null for events the registry publishes. */
        device_id device{};
        /** @brief When the event happened, stamped by whoever published it. */
        input_time time{};
    };

    /**
     * @struct device_connected_event
     * @brief A device appeared. Published before any of its control events, so a listener can size its own state from
     * `layout` on the way past.
     */
    struct device_connected_event : device_event<tags::device_connected>
    {
        /** @brief The device's identity. A copy, so the event stays valid after the device goes away. */
        device_info info{};
        /** @brief The device's controls. Shared, so this costs a refcount rather than a copy. */
        layout_ref layout{};
    };

    /**
     * @struct device_disconnected_event
     * @brief A device went away. Its controls are reset to rest *before* this is published, and no releases are
     * published for controls that were held - consumers tracking held state should clear it here (input_state does).
     */
    struct device_disconnected_event : device_event<tags::device_disconnected>
    {
        device_info info{};
    };

    /**
     * @struct control_changed_event
     * @brief The generic form of every control change, published alongside the typed event for the same change.
     * @details Typed events (key_event, gamepad_axis_event, ...) are the readable surface and are what applications
     * normally listen to. This one exists for code that must handle controls it has no enum for: a bindings screen
     * waiting for "whatever the user presses next", an input recorder, a debug overlay listing live controls.
     */
    struct control_changed_event : device_event<tags::control_changed>
    {
        device_kind kind{device_kind::any};
        control_id control{};
        control_kind control_kind_{control_kind::button};
        float value{0.0f};
        float previous{0.0f};
    };

} // namespace catalyst::input
