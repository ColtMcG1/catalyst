/**
 * @file state.cpp
 * @brief Implementation of input_state.
 * License: CDDL-1.0 (see LICENSE).
 */

#include <catalyst/input/state.hpp>

#include <algorithm>

namespace catalyst::input
{
    namespace
    {
        /** @brief The threshold at which a control counts as down for the edge bookkeeping. */
        constexpr float k_press_point = 0.5f;

        [[nodiscard]] bool crossed_up(float previous, float value) noexcept
        {
            return previous < k_press_point && value >= k_press_point;
        }

        [[nodiscard]] bool crossed_down(float previous, float value) noexcept
        {
            return previous >= k_press_point && value < k_press_point;
        }
    } // namespace

    // ----------------------------------------------------------------------------------------------------------------
    // Attach / detach
    // ----------------------------------------------------------------------------------------------------------------

    void input_state::attach(device_registry &registry, int priority)
    {
        detach();
        m_registry = &registry;

        events::bus &bus = registry.bus();
        m_tokens.reserve(9);

        m_tokens.emplace_back(bus.add_listener<control_changed_event>(
            [this](const control_changed_event &e) { on_control_changed(e); }, priority));
        m_tokens.emplace_back(bus.add_listener<device_connected_event>(
            [this](const device_connected_event &e) { on_device_connected(e); }, priority));
        m_tokens.emplace_back(bus.add_listener<device_disconnected_event>(
            [this](const device_disconnected_event &e) { on_device_disconnected(e); }, priority));
        m_tokens.emplace_back(bus.add_listener<key_event>(
            [this](const key_event &e) { on_key(e); }, priority));
        m_tokens.emplace_back(bus.add_listener<text_input_event>(
            [this](const text_input_event &e) { on_text(e); }, priority));
        m_tokens.emplace_back(bus.add_listener<mouse_button_event>(
            [this](const mouse_button_event &e) { on_mouse_button(e); }, priority));
        m_tokens.emplace_back(bus.add_listener<mouse_move_event>(
            [this](const mouse_move_event &e) { on_mouse_move(e); }, priority));
        m_tokens.emplace_back(bus.add_listener<mouse_enter_event>(
            [this](const mouse_enter_event &e) { on_mouse_enter(e); }, priority));
        m_tokens.emplace_back(bus.add_listener<mouse_leave_event>(
            [this](const mouse_leave_event &e) { on_mouse_leave(e); }, priority));
    }

    void input_state::detach() noexcept
    {
        m_tokens.clear();
        m_registry = nullptr;
        m_edges.clear();
        m_modifiers = key_modifiers::none;
        m_text.clear();
        m_disconnected.clear();
        m_mouse_window = 0;
        m_mouse_inside = false;
        m_double_clicked = mouse_buttons::none;
    }

    void input_state::new_frame() noexcept
    {
        for (edges &e : m_edges)
        {
            std::fill(e.pressed.begin(), e.pressed.end(), std::uint8_t{0});
            std::fill(e.released.begin(), e.released.end(), std::uint8_t{0});
            std::fill(e.repeated.begin(), e.repeated.end(), std::uint8_t{0});
            e.connected = false;
            e.disconnected = false;
        }
        m_disconnected.clear();
        m_text.clear();
        m_double_clicked = mouse_buttons::none;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // Edge bookkeeping
    // ----------------------------------------------------------------------------------------------------------------

    input_state::edges *input_state::edges_for(device_id id) noexcept
    {
        if (!id.valid())
            return nullptr;
        if (id.index >= m_edges.size())
            m_edges.resize(static_cast<std::size_t>(id.index) + 1);

        edges &e = m_edges[id.index];
        if (e.generation != id.generation)
        {
            // A new device took the slot: whatever the last one left behind is not ours.
            e.generation = id.generation;
            std::fill(e.pressed.begin(), e.pressed.end(), std::uint8_t{0});
            std::fill(e.released.begin(), e.released.end(), std::uint8_t{0});
            std::fill(e.repeated.begin(), e.repeated.end(), std::uint8_t{0});
        }
        return &e;
    }

    const input_state::edges *input_state::edges_for(device_id id) const noexcept
    {
        if (!id.valid() || id.index >= m_edges.size())
            return nullptr;
        const edges &e = m_edges[id.index];
        return e.generation == id.generation ? &e : nullptr;
    }

    bool input_state::edge_flag(device_id id, control_id control,
                                std::vector<std::uint8_t> edges::*which) const noexcept
    {
        const edges *e = edges_for(id);
        if (!e || !control.valid())
            return false;
        const std::vector<std::uint8_t> &v = e->*which;
        return control.index < v.size() && v[control.index] != 0;
    }

    void input_state::on_control_changed(const control_changed_event &e)
    {
        edges *ed = edges_for(e.device);
        if (!ed || !e.control.valid())
            return;

        const std::size_t needed = static_cast<std::size_t>(e.control.index) + 1;
        if (ed->pressed.size() < needed)
        {
            ed->pressed.resize(needed, 0);
            ed->released.resize(needed, 0);
            ed->repeated.resize(needed, 0);
        }

        if (crossed_up(e.previous, e.value))
            ed->pressed[e.control.index] = 1;
        else if (crossed_down(e.previous, e.value))
            ed->released[e.control.index] = 1;
    }

    void input_state::on_device_connected(const device_connected_event &e)
    {
        if (edges *ed = edges_for(e.device))
            ed->connected = true;
    }

    void input_state::on_device_disconnected(const device_disconnected_event &e)
    {
        // The registry has already reset the controls, so the releases arrived as control_changed_events before this.
        // All that is left is to record that the slot emptied - and to keep the entry alive until the next new_frame()
        // so that was_gamepad_disconnected() can still be asked about it this frame.
        if (e.device.index < m_edges.size() && m_edges[e.device.index].generation == e.device.generation)
            m_edges[e.device.index].disconnected = true;
        m_disconnected.emplace_back(e.info.kind, e.info.slot);
    }

    void input_state::on_key(const key_event &e)
    {
        m_modifiers = e.modifiers;

        // A repeat leaves the control at 1, so it produces no control_changed_event; it has to be caught here.
        if (e.action != button_action::repeat)
            return;

        edges *ed = edges_for(e.device);
        const control_id c = e.control();
        if (!ed || !c.valid())
            return;

        const std::size_t needed = static_cast<std::size_t>(c.index) + 1;
        if (ed->repeated.size() < needed)
        {
            ed->pressed.resize(needed, 0);
            ed->released.resize(needed, 0);
            ed->repeated.resize(needed, 0);
        }
        ed->repeated[c.index] = 1;
    }

    void input_state::on_text(const text_input_event &e)
    {
        m_text.append(e.text());
    }

    void input_state::on_mouse_button(const mouse_button_event &e) noexcept
    {
        m_modifiers = e.modifiers;
        m_mouse_window = e.window;
        if (e.action == button_action::press && e.clicks >= 2)
            m_double_clicked |= to_mouse_buttons(e.button);
    }

    void input_state::on_mouse_move(const mouse_move_event &e) noexcept
    {
        m_modifiers = e.modifiers;
        m_mouse_window = e.window;
    }

    void input_state::on_mouse_enter(const mouse_enter_event &e) noexcept
    {
        m_mouse_window = e.window;
        m_mouse_inside = true;
    }

    void input_state::on_mouse_leave(const mouse_leave_event &e) noexcept
    {
        m_mouse_window = e.window;
        m_mouse_inside = false;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // Generic queries
    // ----------------------------------------------------------------------------------------------------------------

    device_id input_state::device(const device_selector &sel) const noexcept
    {
        return m_registry ? m_registry->find(sel) : no_device;
    }

    device_id input_state::first_of(device_kind kind) const noexcept
    {
        return device(device_selector::of(kind));
    }

    float input_state::value(device_id id, control_id control) const noexcept
    {
        return m_registry ? m_registry->value(id, control) : 0.0f;
    }

    bool input_state::is_down(device_id id, control_id control, float press_point) const noexcept
    {
        return m_registry && m_registry->is_down(id, control, press_point);
    }

    bool input_state::was_pressed(device_id id, control_id control) const noexcept
    {
        return edge_flag(id, control, &edges::pressed);
    }

    bool input_state::was_released(device_id id, control_id control) const noexcept
    {
        return edge_flag(id, control, &edges::released);
    }

    // ----------------------------------------------------------------------------------------------------------------
    // Keyboard
    // ----------------------------------------------------------------------------------------------------------------

    bool input_state::is_key_down(key_code code) const noexcept
    {
        if (!m_registry)
            return false;

        // Any keyboard: two of them plugged in should behave like one, which is what a user with a laptop and an
        // external keyboard expects.
        bool down = false;
        const control_id c = control_of(code);
        m_registry->for_each(device_selector::of(device_kind::keyboard),
                             [&](device_id id) { down = down || m_registry->is_down(id, c); });
        return down;
    }

    bool input_state::was_key_pressed(key_code code) const noexcept
    {
        if (!m_registry)
            return false;
        bool hit = false;
        const control_id c = control_of(code);
        m_registry->for_each(device_selector::of(device_kind::keyboard),
                             [&](device_id id) { hit = hit || was_pressed(id, c); });
        return hit;
    }

    bool input_state::was_key_released(key_code code) const noexcept
    {
        if (!m_registry)
            return false;
        bool hit = false;
        const control_id c = control_of(code);
        m_registry->for_each(device_selector::of(device_kind::keyboard),
                             [&](device_id id) { hit = hit || was_released(id, c); });
        return hit;
    }

    bool input_state::was_key_repeated(key_code code) const noexcept
    {
        if (!m_registry)
            return false;
        bool hit = false;
        const control_id c = control_of(code);
        m_registry->for_each(device_selector::of(device_kind::keyboard), [&](device_id id)
                             { hit = hit || was_pressed(id, c) || edge_flag(id, c, &edges::repeated); });
        return hit;
    }

    std::size_t input_state::keys_down_count() const noexcept
    {
        if (!m_registry)
            return 0;

        std::size_t count = 0;
        m_registry->for_each(device_selector::of(device_kind::keyboard), [&](device_id id)
                             {
            for (const float v : m_registry->values(id))
                if (v >= k_press_point)
                    ++count; });
        return count;
    }

    bool input_state::any_key_down() const noexcept
    {
        return keys_down_count() != 0;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // Mouse
    // ----------------------------------------------------------------------------------------------------------------

    bool input_state::is_mouse_button_down(mouse_button b) const noexcept
    {
        return is_down(first_of(device_kind::mouse), control_of(b));
    }

    bool input_state::was_mouse_button_pressed(mouse_button b) const noexcept
    {
        return was_pressed(first_of(device_kind::mouse), control_of(b));
    }

    bool input_state::was_mouse_button_released(mouse_button b) const noexcept
    {
        return was_released(first_of(device_kind::mouse), control_of(b));
    }

    bool input_state::was_mouse_button_double_clicked(mouse_button b) const noexcept
    {
        return has_button(m_double_clicked, b);
    }

    mouse_buttons input_state::mouse_buttons_down() const noexcept
    {
        mouse_buttons held = mouse_buttons::none;
        const device_id id = first_of(device_kind::mouse);
        for (std::size_t i = 0; i < mouse_button_count; ++i)
        {
            const auto b = static_cast<mouse_button>(i);
            if (is_down(id, control_of(b)))
                held |= to_mouse_buttons(b);
        }
        return held;
    }

    math::vec2<std::int32_t> input_state::mouse_position() const noexcept
    {
        const device_id id = first_of(device_kind::mouse);
        return {static_cast<std::int32_t>(value(id, control_of(mouse_axis::x))),
                static_cast<std::int32_t>(value(id, control_of(mouse_axis::y)))};
    }

    math::vec2<std::int32_t> input_state::mouse_delta() const noexcept
    {
        const device_id id = first_of(device_kind::mouse);
        return {static_cast<std::int32_t>(value(id, control_of(mouse_axis::delta_x))),
                static_cast<std::int32_t>(value(id, control_of(mouse_axis::delta_y)))};
    }

    math::vec2<std::int32_t> input_state::raw_mouse_delta() const noexcept
    {
        const device_id id = first_of(device_kind::mouse);
        return {static_cast<std::int32_t>(value(id, control_of(mouse_axis::raw_x))),
                static_cast<std::int32_t>(value(id, control_of(mouse_axis::raw_y)))};
    }

    math::vec2<float> input_state::wheel_delta() const noexcept
    {
        const device_id id = first_of(device_kind::mouse);
        return {value(id, control_of(mouse_axis::wheel_x)), value(id, control_of(mouse_axis::wheel_y))};
    }

    // ----------------------------------------------------------------------------------------------------------------
    // Gamepads
    // ----------------------------------------------------------------------------------------------------------------

    device_id input_state::gamepad_device(std::uint32_t slot) const noexcept
    {
        return device(device_selector::of(device_kind::gamepad, slot));
    }

    bool input_state::is_gamepad_connected(std::uint32_t slot) const noexcept
    {
        return gamepad_device(slot).valid();
    }

    bool input_state::is_gamepad_button_down(std::uint32_t slot, gamepad_button b) const noexcept
    {
        return is_down(gamepad_device(slot), control_of(b));
    }

    bool input_state::was_gamepad_button_pressed(std::uint32_t slot, gamepad_button b) const noexcept
    {
        return was_pressed(gamepad_device(slot), control_of(b));
    }

    bool input_state::was_gamepad_button_released(std::uint32_t slot, gamepad_button b) const noexcept
    {
        return was_released(gamepad_device(slot), control_of(b));
    }

    double input_state::gamepad_axis_value(std::uint32_t slot, gamepad_axis a) const noexcept
    {
        return static_cast<double>(value(gamepad_device(slot), control_of(a)));
    }

    gamepad_state input_state::gamepad(std::uint32_t slot) const noexcept
    {
        gamepad_state out{};
        const device_id id = gamepad_device(slot);
        if (!id.valid())
            return out;

        out.connected = true;
        for (std::size_t i = 0; i < gamepad_button_count; ++i)
        {
            const auto b = static_cast<gamepad_button>(i);
            if (is_down(id, control_of(b)))
                out.buttons |= to_gamepad_buttons(b);
        }
        for (std::size_t i = 0; i < gamepad_axis_count; ++i)
            out.axes[i] = static_cast<double>(value(id, control_of(static_cast<gamepad_axis>(i))));
        return out;
    }

    bool input_state::was_gamepad_connected(std::uint32_t slot) const noexcept
    {
        const edges *e = edges_for(gamepad_device(slot));
        return e && e->connected;
    }

    bool input_state::was_gamepad_disconnected(std::uint32_t slot) const noexcept
    {
        // The device is already gone, so it cannot be looked up by slot any more; the frame's record is all there is.
        return std::any_of(m_disconnected.begin(), m_disconnected.end(), [&](const auto &d)
                           { return d.first == device_kind::gamepad && d.second == slot; });
    }

    // ----------------------------------------------------------------------------------------------------------------
    // Touch
    // ----------------------------------------------------------------------------------------------------------------

    std::size_t input_state::touch_count() const noexcept
    {
        const device_id id = first_of(device_kind::touchscreen);
        if (!id.valid())
            return 0;

        std::size_t n = 0;
        for (std::size_t i = 0; i < max_touch_points; ++i)
        {
            if (is_down(id, touch_control_of(i, touch_control::down)))
                ++n;
        }
        return n;
    }

    bool input_state::is_touch_down(std::size_t index) const noexcept
    {
        return is_down(first_of(device_kind::touchscreen), touch_control_of(index, touch_control::down));
    }

    math::vec2<float> input_state::touch_position(std::size_t index) const noexcept
    {
        const device_id id = first_of(device_kind::touchscreen);
        return {value(id, touch_control_of(index, touch_control::x)),
                value(id, touch_control_of(index, touch_control::y))};
    }

} // namespace catalyst::input
