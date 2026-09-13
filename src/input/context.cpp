/**
 * @file context.cpp
 * @brief Implementation of input::context - the frame, the backends, and the platform feed.
 * License: MIT (see LICENSE).
 */

#include <catalyst/input/context.hpp>

#include "detail_backend.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace catalyst::input
{
    namespace
    {
        /**
         * @brief How long an empty gamepad slot is left alone before it is probed again.
         * @details Probing an empty XInput slot costs on the order of a millisecond. Four of them, every frame, is a
         * measurable share of a 16ms budget spent asking about controllers nobody owns.
         */
        constexpr auto k_empty_slot_probe_interval = std::chrono::milliseconds(1000);

        [[nodiscard]] double clamp_threshold(double t) noexcept
        {
            return std::clamp(t, 0.0, 0.999);
        }
    } // namespace

    context::context(events::bus &bus)
        : m_registry(bus), m_state(m_registry)
    {
        m_touch_used.fill(false);
    }

    context::~context()
    {
        // Detach before the registry goes: the tracker's listeners hold a `this` that is about to stop being valid.
        m_state.detach();
        m_registry.clear();
    }

    const char *context::backend_name() const noexcept
    {
        return detail::backend_name();
    }

    // ------------------------------------------------------------------------------------------------------------------
    // The frame
    // ------------------------------------------------------------------------------------------------------------------

    void context::new_frame() noexcept
    {
        m_state.new_frame();
        m_actions.new_frame();
        m_registry.clear_deltas();
    }

    void context::update()
    {
        update(input_clock::now());
    }

    void context::update(input_time now)
    {
        m_actions.update(m_registry, now, &m_registry.bus());
    }

    void context::poll()
    {
        poll_gamepads(input_clock::now());
    }

    // ------------------------------------------------------------------------------------------------------------------
    // Devices
    // ------------------------------------------------------------------------------------------------------------------

    device_id context::ensure_device(device_id &cache, device_kind kind, const layout_ref &layout, const char *name)
    {
        if (m_registry.alive(cache))
            return cache;

        device_info info;
        info.kind = kind;
        info.name = name;
        cache = m_registry.add_device(std::move(info), layout);
        return cache;
    }

    device_id context::keyboard() { return ensure_device(m_keyboard, device_kind::keyboard, keyboard_layout(), "Keyboard"); }
    device_id context::mouse() { return ensure_device(m_mouse, device_kind::mouse, mouse_layout(), "Mouse"); }
    device_id context::touchscreen() { return ensure_device(m_touch, device_kind::touchscreen, touch_layout(), "Touchscreen"); }
    device_id context::pen_device() { return ensure_device(m_pen, device_kind::pen, pen_layout(), "Pen"); }

    device_id context::add_simulated_device(device_kind kind, layout_ref layout, std::string name, std::uint32_t slot)
    {
        device_info info;
        info.kind = kind;
        info.name = name.empty() ? std::string(device_kind_name(kind)) + " (simulated)" : std::move(name);
        info.slot = slot;
        info.flags = device_flags::simulated;
        return m_registry.add_device(std::move(info), std::move(layout));
    }

    template <typename Event>
    void context::feed_control(device_id device, control_id control, float value, Event e)
    {
        // The control value goes in first, so that a listener woken by the typed event below already sees the registry
        // agreeing with it. The other order would let a handler read a stale value from inside the event describing the
        // change - the exact confusion this module was rebuilt to remove.
        m_registry.set_value(device, control, value);

        e.device = device;
        if (e.time == input_time{})
            e.time = input_clock::now();
        m_registry.bus().dispatch(std::move(e));
    }

    // ------------------------------------------------------------------------------------------------------------------
    // Keyboard and text
    // ------------------------------------------------------------------------------------------------------------------

    void context::feed_key(const key_event &e)
    {
        const device_id id = keyboard();
        const control_id c = control_of(e.code);

        // A repeat leaves the control where it is; only presses and releases move it.
        if (e.action != button_action::repeat)
            m_registry.set_value(id, c, e.action == button_action::press ? 1.0f : 0.0f);

        key_event out = e;
        out.device = id;
        if (out.time == input_time{})
            out.time = input_clock::now();
        m_registry.bus().dispatch(std::move(out));
    }

    void context::feed_text(const text_input_event &e)
    {
        text_input_event out = e;
        out.device = keyboard();
        if (out.time == input_time{})
            out.time = input_clock::now();
        m_registry.bus().dispatch(std::move(out));
    }

    void context::feed_composition(const text_composition_event &e)
    {
        text_composition_event out = e;
        out.device = keyboard();
        if (out.time == input_time{})
            out.time = input_clock::now();
        m_registry.bus().dispatch(std::move(out));
    }

    void context::feed_focus_lost(std::uint64_t window)
    {
        const input_time now = input_clock::now();

        // The platform layer used to keep a bitset per window purely to synthesise these. It does not have to: the
        // registry already knows exactly what is down, so one call produces exactly the right releases and nothing is
        // left stuck when the user alt-tabs mid-strafe.
        if (m_registry.alive(m_keyboard))
        {
            const auto values = m_registry.values(m_keyboard);
            std::vector<key_code> held;
            for (std::size_t i = 0; i < values.size(); ++i)
            {
                if (values[i] >= 0.5f)
                    held.push_back(key_of(control_at(i)));
            }

            for (const key_code code : held)
            {
                key_event e;
                e.window = window;
                e.code = code;
                e.action = button_action::release;
                e.time = now;
                feed_key(e);
            }
        }

        if (m_registry.alive(m_mouse))
        {
            for (std::size_t i = 0; i < mouse_button_count; ++i)
            {
                const auto b = static_cast<mouse_button>(i);
                if (!m_registry.is_down(m_mouse, control_of(b)))
                    continue;

                mouse_button_event e;
                e.window = window;
                e.button = b;
                e.action = button_action::release;
                e.time = now;
                feed_mouse_button(e);
            }
        }
    }

    // ------------------------------------------------------------------------------------------------------------------
    // Mouse
    // ------------------------------------------------------------------------------------------------------------------

    void context::feed_mouse_move(const mouse_move_event &e)
    {
        const device_id id = mouse();

        m_registry.set_value(id, control_of(mouse_axis::x), static_cast<float>(e.position_px[0]));
        m_registry.set_value(id, control_of(mouse_axis::y), static_cast<float>(e.position_px[1]));

        // Deltas accumulate across the frame, because several move messages can arrive between two new_frame() calls
        // and a camera wants their sum, not the last one.
        const float dx = m_registry.value(id, control_of(mouse_axis::delta_x)) + static_cast<float>(e.delta_px[0]);
        const float dy = m_registry.value(id, control_of(mouse_axis::delta_y)) + static_cast<float>(e.delta_px[1]);
        m_registry.set_value(id, control_of(mouse_axis::delta_x), dx);
        m_registry.set_value(id, control_of(mouse_axis::delta_y), dy);

        mouse_move_event out = e;
        out.device = id;
        if (out.time == input_time{})
            out.time = input_clock::now();
        m_registry.bus().dispatch(std::move(out));
    }

    void context::feed_mouse_button(const mouse_button_event &e)
    {
        feed_control(mouse(), control_of(e.button), e.action == button_action::press ? 1.0f : 0.0f, e);
    }

    void context::feed_mouse_wheel(const mouse_wheel_event &e)
    {
        const device_id id = mouse();
        const float wx = m_registry.value(id, control_of(mouse_axis::wheel_x)) + e.delta[0];
        const float wy = m_registry.value(id, control_of(mouse_axis::wheel_y)) + e.delta[1];
        m_registry.set_value(id, control_of(mouse_axis::wheel_x), wx);
        m_registry.set_value(id, control_of(mouse_axis::wheel_y), wy);

        mouse_wheel_event out = e;
        out.device = id;
        if (out.time == input_time{})
            out.time = input_clock::now();
        m_registry.bus().dispatch(std::move(out));
    }

    void context::feed_mouse_enter(const mouse_enter_event &e)
    {
        const device_id id = mouse();
        m_registry.set_value(id, control_of(mouse_axis::x), static_cast<float>(e.position_px[0]));
        m_registry.set_value(id, control_of(mouse_axis::y), static_cast<float>(e.position_px[1]));

        mouse_enter_event out = e;
        out.device = id;
        if (out.time == input_time{})
            out.time = input_clock::now();
        m_registry.bus().dispatch(std::move(out));
    }

    void context::feed_mouse_leave(const mouse_leave_event &e)
    {
        mouse_leave_event out = e;
        out.device = mouse();
        if (out.time == input_time{})
            out.time = input_clock::now();
        m_registry.bus().dispatch(std::move(out));
    }

    void context::feed_mouse_raw_move(const mouse_raw_move_event &e)
    {
        const device_id id = mouse();
        const float rx = m_registry.value(id, control_of(mouse_axis::raw_x)) + static_cast<float>(e.delta[0]);
        const float ry = m_registry.value(id, control_of(mouse_axis::raw_y)) + static_cast<float>(e.delta[1]);
        m_registry.set_value(id, control_of(mouse_axis::raw_x), rx);
        m_registry.set_value(id, control_of(mouse_axis::raw_y), ry);

        mouse_raw_move_event out = e;
        out.device = id;
        if (out.time == input_time{})
            out.time = input_clock::now();
        m_registry.bus().dispatch(std::move(out));
    }

    // ------------------------------------------------------------------------------------------------------------------
    // Touch and pen
    // ------------------------------------------------------------------------------------------------------------------

    void context::feed_touch(const touch_event &e)
    {
        const device_id id = touchscreen();

        // Map the platform's tracking id onto a control slot and keep it there for the contact's whole life, so a
        // gesture reading "contact 2" is reading the same finger from began to ended.
        std::size_t index = max_touch_points;
        for (std::size_t i = 0; i < max_touch_points; ++i)
        {
            if (m_touch_used[i] && m_touch_ids[i] == e.id)
            {
                index = i;
                break;
            }
        }
        if (index == max_touch_points && e.phase == touch_phase::began)
        {
            for (std::size_t i = 0; i < max_touch_points; ++i)
            {
                if (!m_touch_used[i])
                {
                    index = i;
                    m_touch_used[i] = true;
                    m_touch_ids[i] = e.id;
                    break;
                }
            }
        }
        if (index == max_touch_points)
            return; // More contacts than slots: drop the extra rather than aliasing an existing finger.

        const bool active = is_active_phase(e.phase);
        m_registry.set_value(id, touch_control_of(index, touch_control::x), e.position_px[0]);
        m_registry.set_value(id, touch_control_of(index, touch_control::y), e.position_px[1]);
        m_registry.set_value(id, touch_control_of(index, touch_control::pressure), active ? e.pressure : 0.0f);
        m_registry.set_value(id, touch_control_of(index, touch_control::size), active ? e.size : 0.0f);
        m_registry.set_value(id, touch_control_of(index, touch_control::down), active ? 1.0f : 0.0f);

        if (!active)
            m_touch_used[index] = false;

        touch_event out = e;
        out.device = id;
        out.index = static_cast<std::uint8_t>(index);
        if (out.time == input_time{})
            out.time = input_clock::now();
        m_registry.bus().dispatch(std::move(out));
    }

    void context::feed_pen(const pen_event &e)
    {
        const device_id id = pen_device();

        m_registry.set_value(id, control_of(pen_axis::x), e.position_px[0]);
        m_registry.set_value(id, control_of(pen_axis::y), e.position_px[1]);
        m_registry.set_value(id, control_of(pen_axis::pressure), e.pressure);
        m_registry.set_value(id, control_of(pen_axis::tilt_x), e.tilt[0]);
        m_registry.set_value(id, control_of(pen_axis::tilt_y), e.tilt[1]);
        m_registry.set_value(id, control_of(pen_axis::twist), e.twist);
        m_registry.set_value(id, control_of(pen_axis::distance), e.distance);

        // Which end is down is a property of the pen, not a separate button press the platform has to remember.
        const float contact = e.contact ? 1.0f : 0.0f;
        m_registry.set_value(id, control_of(pen_button::tip), e.inverted ? 0.0f : contact);
        m_registry.set_value(id, control_of(pen_button::eraser), e.inverted ? contact : 0.0f);

        pen_event out = e;
        out.device = id;
        if (out.time == input_time{})
            out.time = input_clock::now();
        m_registry.bus().dispatch(std::move(out));
    }

    void context::feed_pen_button(const pen_button_event &e)
    {
        feed_control(pen_device(), control_of(e.button), e.action == button_action::press ? 1.0f : 0.0f, e);
    }

    // ------------------------------------------------------------------------------------------------------------------
    // Gamepads
    // ------------------------------------------------------------------------------------------------------------------

    std::size_t context::gamepad_capacity() const noexcept
    {
        return std::min(detail::gamepad_capacity(), max_gamepads);
    }

    void context::set_deadzone(const gamepad_deadzone &dz) noexcept
    {
        m_deadzone.stick = clamp_threshold(dz.stick);
        m_deadzone.trigger = clamp_threshold(dz.trigger);
    }

    void context::apply_deadzones(gamepad_state &s) const noexcept
    {
        auto &ax = s.axes;
        apply_radial_deadzone(ax[static_cast<std::size_t>(gamepad_axis::left_x)],
                              ax[static_cast<std::size_t>(gamepad_axis::left_y)], m_deadzone.stick);
        apply_radial_deadzone(ax[static_cast<std::size_t>(gamepad_axis::right_x)],
                              ax[static_cast<std::size_t>(gamepad_axis::right_y)], m_deadzone.stick);

        for (const auto a : {gamepad_axis::left_trigger, gamepad_axis::right_trigger})
        {
            double &v = ax[static_cast<std::size_t>(a)];
            v = apply_deadzone(std::clamp(v, 0.0, 1.0), m_deadzone.trigger);
        }
    }

    gamepad_state context::raw_gamepad(std::uint32_t slot) const noexcept
    {
        gamepad_state out{};
        if (slot >= max_gamepads)
            return out;

        const gamepad_slot &s = m_gamepads[slot];
        if (!s.connected)
            return out;

        out = m_state.gamepad(slot);
        out.axes = s.raw_axes;
        return out;
    }

    bool context::set_rumble(std::uint32_t slot, const rumble_state &r) noexcept
    {
        if (slot >= gamepad_capacity())
            return false;
        return detail::set_gamepad_rumble(slot, std::clamp(r.low_frequency, 0.0, 1.0),
                                          std::clamp(r.high_frequency, 0.0, 1.0));
    }

    bool context::set_rumble(device_id id, const rumble_state &r) noexcept
    {
        const device_info *info = m_registry.info(id);
        if (!info || info->kind != device_kind::gamepad || info->slot == any_slot)
            return false;
        return set_rumble(info->slot, r);
    }

    void context::publish_gamepad_state(gamepad_slot &slot, std::uint32_t index, const gamepad_state &next, input_time now)
    {
        const device_id id = slot.device;

        for (std::size_t i = 0; i < gamepad_button_count; ++i)
        {
            const auto b = static_cast<gamepad_button>(i);
            const control_id c = control_of(b);
            const bool was = m_registry.is_down(id, c);
            const bool is = next.is_down(b);
            if (was == is)
                continue;

            m_registry.set_value(id, c, is ? 1.0f : 0.0f);

            gamepad_button_event e;
            e.device = id;
            e.time = now;
            e.slot = index;
            e.button = b;
            e.action = is ? button_action::press : button_action::release;
            e.value = is ? 1.0f : 0.0f;
            m_registry.bus().dispatch(std::move(e));
        }

        for (std::size_t i = 0; i < gamepad_axis_count; ++i)
        {
            const auto a = static_cast<gamepad_axis>(i);
            const control_id c = control_of(a);
            const float previous = m_registry.value(id, c);
            const auto value = static_cast<float>(next.axes[i]);
            if (previous == value)
                continue;

            m_registry.set_value(id, c, value);

            gamepad_axis_event e;
            e.device = id;
            e.time = now;
            e.slot = index;
            e.axis = a;
            e.value = value;
            e.previous = previous;
            m_registry.bus().dispatch(std::move(e));
        }
    }

    void context::poll_gamepads(input_time now)
    {
        const std::size_t capacity = gamepad_capacity();

        for (std::uint32_t index = 0; index < capacity; ++index)
        {
            gamepad_slot &slot = m_gamepads[index];

            if (!slot.connected && now < slot.next_probe)
                continue;

            gamepad_state raw{};
            if (!detail::read_gamepad(index, raw))
            {
                slot.next_probe = now + k_empty_slot_probe_interval;
                if (slot.connected)
                {
                    // The registry resets the controls and publishes the disconnect; nothing else has to unwind.
                    m_registry.remove_device(slot.device);
                    slot = gamepad_slot{};
                    slot.next_probe = now + k_empty_slot_probe_interval;
                }
                continue;
            }

            raw.connected = true;
            slot.raw_axes = raw.axes;
            apply_deadzones(raw);

            if (!slot.connected)
            {
                device_info info;
                info.kind = device_kind::gamepad;
                info.name = "Gamepad";
                info.slot = index;
                info.flags = device_flags::rumble;
                slot.device = m_registry.add_device(std::move(info), gamepad_layout());
                slot.connected = true;
                // Controls start at rest, so a pad plugged in with a button already held reports that press.
            }

            publish_gamepad_state(slot, index, raw, now);
        }
    }

} // namespace catalyst::input
