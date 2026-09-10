/**
 * @file action.cpp
 * @brief Reading bindings, running interactions, and deciding an action's phase.
 * License: CDDL-1.0 (see LICENSE).
 */

#include <catalyst/input/action.hpp>

#include <algorithm>
#include <cmath>

namespace catalyst::input
{
    namespace
    {
        using ms = std::chrono::milliseconds;

        /**
         * @brief Whether a control has a natural range the processors can normalise against.
         * @details A mouse delta and a pixel coordinate do not: shaping one would turn a fast flick and a slow drag
         * into the same number. Everything else - buttons, sticks, triggers, pressure - does. The layout is asked
         * rather than the value guessed at, because a stick that happens to read 1.2 for a frame is still bounded.
         */
        [[nodiscard]] bool control_is_bounded(const device_registry &registry, device_id device, control_id control) noexcept
        {
            const device_layout *layout = registry.layout(device);
            if (!layout || !layout->contains(control))
                return true;
            const control_kind k = layout->kind_of(control);
            return k != control_kind::delta && k != control_kind::absolute;
        }

        /** @brief Length of the first @p dims components. */
        [[nodiscard]] float vector_magnitude(const float (&v)[3], std::size_t dims) noexcept
        {
            float sum = 0.0f;
            for (std::size_t i = 0; i < dims && i < 3; ++i)
                sum += v[i] * v[i];
            return std::sqrt(sum);
        }

        [[nodiscard]] ms elapsed_ms(input_time from, input_time to) noexcept
        {
            if (to <= from)
                return ms{0};
            return std::chrono::duration_cast<ms>(to - from);
        }
    } // namespace

    action::action(std::string name, action_kind kind)
        : m_name(std::move(name)), m_kind(kind)
    {
        m_value.kind = kind;
    }

    void action::set_enabled(bool on) noexcept
    {
        if (m_enabled == on)
            return;
        m_enabled = on;

        // Leaving an action mid-hold and coming back to find it still "down" is how a pause menu ends up firing the
        // weapon on resume. Disabling drops everything.
        if (!on)
        {
            m_value = action_value{0.0f, 0.0f, 0.0f, m_kind};
            m_phase = action_phase::idle;
            m_held = false;
            m_source_device = no_device;
            m_source_control = no_control;
            for (binding_state &s : m_states)
                s = binding_state{};
        }
    }

    // ------------------------------------------------------------------------------------------------------------------
    // Bindings
    // ------------------------------------------------------------------------------------------------------------------

    action &action::bind(binding b)
    {
        m_bindings.push_back(std::move(b));
        m_states.emplace_back();
        return *this;
    }

    bool action::rebind(std::size_t index, binding b)
    {
        if (index >= m_bindings.size())
            return false;
        m_bindings[index] = std::move(b);
        m_states[index] = binding_state{};
        return true;
    }

    bool action::unbind(std::size_t index)
    {
        if (index >= m_bindings.size())
            return false;
        m_bindings.erase(m_bindings.begin() + static_cast<std::ptrdiff_t>(index));
        m_states.erase(m_states.begin() + static_cast<std::ptrdiff_t>(index));
        return true;
    }

    void action::clear_bindings()
    {
        m_bindings.clear();
        m_states.clear();
    }

    std::chrono::milliseconds action::held_for(input_time now) const noexcept
    {
        return m_held ? elapsed_ms(m_held_since, now) : ms{0};
    }

    void action::new_frame() noexcept
    {
        m_started_this_frame = false;
        m_performed_this_frame = false;
        m_cancelled_this_frame = false;
    }

    // ------------------------------------------------------------------------------------------------------------------
    // Reading the registry
    // ------------------------------------------------------------------------------------------------------------------

    float action::read_part(const device_registry &registry, const binding_part &p, device_id &device_out)
    {
        if (!p.control.valid())
            return 0.0f;

        // A selector that names no specific device matches every device of its kind, and the loudest of them wins -
        // so a laptop keyboard and an external one behave as one keyboard, and either of two pads can drive a
        // single-player action without the game caring which.
        float best = 0.0f;
        device_id best_device = no_device;

        registry.for_each(p.device, [&](device_id id)
                          {
            const float raw = registry.value(id, p.control);
            if (std::fabs(raw) > std::fabs(best))
            {
                best = raw;
                best_device = id;
            } });

        if (best_device.valid())
            device_out = best_device;
        return best;
    }

    bool action::modifiers_satisfied(const device_registry &registry, const binding &b)
    {
        for (const binding_part &m : b.modifiers)
        {
            device_id ignored{};
            const float v = read_part(registry, m, ignored);
            if (v < m.processors.press_point)
                return false;
        }
        return true;
    }

    action::evaluation action::read_binding(const device_registry &registry, const binding &b) const
    {
        evaluation out{};
        out.value.kind = m_kind;

        if (!b.valid() || !modifiers_satisfied(registry, b))
            return out;

        const std::size_t dims = binding_dimensions(b.shape);

        if (b.shape == binding_shape::control)
        {
            const binding_part &p = b.parts[0];
            const float raw = read_part(registry, p, out.device);
            out.value.x = p.processors.apply(raw, control_is_bounded(registry, out.device, p.control));
            out.control = p.control;
            out.down = std::fabs(out.value.x) >= p.processors.press_point;
        }
        else if (b.composite)
        {
            // Buttons standing in for directions: each dimension is (positive - negative), so opposite keys cancel.
            float components[3]{};
            for (std::size_t d = 0; d < dims; ++d)
            {
                const binding_part &neg = b.parts[d * 2];
                const binding_part &pos = b.parts[d * 2 + 1];

                const float n = read_part(registry, neg, out.device) >= neg.processors.press_point ? 1.0f : 0.0f;
                const float p = read_part(registry, pos, out.device) >= pos.processors.press_point ? 1.0f : 0.0f;
                components[d] = p - n;

                if (n != 0.0f)
                    out.control = neg.control;
                if (p != 0.0f)
                    out.control = pos.control;
            }

            // The shared processors of the first part apply to the assembled vector, so a scale or an inversion set on
            // a WASD composite behaves the same way it would on a stick.
            // Buttons are always bounded, and the radial pass is what normalises the diagonal: W and D together read
            // (1, 1), which saturates back to length 1 rather than letting the player move faster north-east.
            const processor_chain &chain = b.parts[0].processors;
            if (dims == 1)
                components[0] = components[0] * chain.scale * (chain.invert ? -1.0f : 1.0f);
            else if (dims == 2)
                chain.apply_radial(components[0], components[1], true);
            else
                chain.apply_radial(components[0], components[1], components[2], true);

            out.value.x = components[0];
            out.value.y = components[1];
            out.value.z = components[2];
            // Over the *binding's* dimensions, not the action's: a 2D binding on a button action is down when the
            // stick is pushed, and reading only x would leave it silent for anything pushed straight up.
            out.down = vector_magnitude(components, dims) >= b.parts[0].processors.press_point;
        }
        else
        {
            // One analog control per dimension. The dead zone is radial across them, so a stick pushed diagonally is
            // not clipped into a square.
            float components[3]{};
            for (std::size_t d = 0; d < dims; ++d)
            {
                components[d] = read_part(registry, b.parts[d], out.device);
                if (components[d] != 0.0f)
                    out.control = b.parts[d].control;
            }

            const processor_chain &chain = b.parts[0].processors;
            const bool bounded = control_is_bounded(registry, out.device, b.parts[0].control);
            if (dims == 1)
                components[0] = chain.apply(components[0], bounded);
            else if (dims == 2)
                chain.apply_radial(components[0], components[1], bounded);
            else
                chain.apply_radial(components[0], components[1], components[2], bounded);

            out.value.x = components[0];
            out.value.y = components[1];
            out.value.z = components[2];
            out.down = vector_magnitude(components, dims) >= b.parts[0].processors.press_point;
        }

        out.value = shape_to_kind(out.value, dims);
        out.magnitude = out.value.magnitude();
        return out;
    }

    action_value action::shape_to_kind(const action_value &in, std::size_t source_dims) const noexcept
    {
        // A binding whose shape differs from the action's still has to produce something sensible - a config file
        // should not be able to silently disable an action by naming the wrong shape. So a multi-dimensional binding
        // on a button action reads as its magnitude, and a button on a 2D action drives x.
        action_value out{};
        out.kind = m_kind;

        switch (m_kind)
        {
        case action_kind::button:
        {
            if (source_dims <= 1)
            {
                out.x = in.x;
                break;
            }
            const float components[3]{in.x, in.y, in.z};
            out.x = vector_magnitude(components, source_dims);
            break;
        }
        case action_kind::axis1d:
            out.x = in.x;
            break;
        case action_kind::axis2d:
            out.x = in.x;
            out.y = in.y;
            break;
        case action_kind::axis3d:
            out.x = in.x;
            out.y = in.y;
            out.z = in.z;
            break;
        }
        return out;
    }

    void action::collect_active_chords(const device_registry &registry, std::vector<control_id> &out) const
    {
        if (!m_enabled)
            return;

        for (const binding &b : m_bindings)
        {
            if (b.modifiers.empty() || b.parts.empty() || !modifiers_satisfied(registry, b))
                continue;
            for (const binding_part &p : b.parts)
            {
                if (p.control.valid())
                    out.push_back(p.control);
            }
        }
    }

    // ------------------------------------------------------------------------------------------------------------------
    // Evaluation
    // ------------------------------------------------------------------------------------------------------------------

    void action::emit(events::bus *bus, std::string_view map_name, action_phase phase,
                      std::size_t binding_index, control_id control, device_id device,
                      std::chrono::milliseconds elapsed, input_time now)
    {
        switch (phase)
        {
        case action_phase::started: m_started_this_frame = true; break;
        case action_phase::performed: m_performed_this_frame = true; break;
        case action_phase::cancelled: m_cancelled_this_frame = true; break;
        case action_phase::idle: break;
        }
        m_phase = phase;

        if (!bus)
            return;

        action_event e;
        e.device = device;
        e.time = now;
        e.map = map_name;
        e.name = m_name;
        e.phase = phase;
        e.value = m_value;
        e.binding_index = static_cast<std::uint16_t>(binding_index);
        e.control = control;
        e.elapsed = elapsed;
        bus->dispatch(std::move(e));
    }

    void action::evaluate(const device_registry &registry, input_time now, events::bus *bus,
                          std::string_view map_name, std::span<const control_id> suppressed)
    {
        if (!m_enabled)
            return;

        m_states.resize(m_bindings.size());

        evaluation best{};
        best.value.kind = m_kind;
        std::size_t best_index = 0;
        bool any_down = false;

        // Pass one: read every binding and pick the one being pushed hardest. Its value becomes the action's, which is
        // what makes "bound to both a stick and WASD" behave when a player switches between them mid-game.
        std::vector<evaluation> reads(m_bindings.size());
        for (std::size_t i = 0; i < m_bindings.size(); ++i)
        {
            const binding &b = m_bindings[i];

            // A chord elsewhere in the map has claimed this control: Ctrl+S must not also walk backwards.
            const bool blocked = b.modifiers.empty() && !b.parts.empty() &&
                                 std::any_of(b.parts.begin(), b.parts.end(), [&](const binding_part &p)
                                             { return p.control.valid() &&
                                                      std::find(suppressed.begin(), suppressed.end(), p.control) != suppressed.end(); });

            reads[i] = blocked ? evaluation{{0.0f, 0.0f, 0.0f, m_kind}, 0.0f, false, no_control, no_device}
                               : read_binding(registry, b);

            if (reads[i].magnitude > best.magnitude)
            {
                best = reads[i];
                best_index = i;
            }
            any_down = any_down || reads[i].down;
        }

        m_value = best.value;
        if (best.device.valid())
        {
            m_source_device = best.device;
            m_source_control = best.control;
        }

        const bool was_held = m_held;
        m_held = any_down;
        if (m_held && !was_held)
            m_held_since = now;

        // Pass two: run each binding's interaction. Phases come from the interactions, not from the value, because
        // "held for 200ms" is not something a value can express.
        for (std::size_t i = 0; i < m_bindings.size(); ++i)
        {
            const binding &b = m_bindings[i];
            binding_state &s = m_states[i];
            const evaluation &r = reads[i];

            const bool down = r.down;
            const bool went_down = down && !s.down;
            const bool went_up = !down && s.down;
            const ms held = s.down ? elapsed_ms(s.down_at, now) : ms{0};

            if (went_down)
            {
                s.down_at = now;
                s.performed = false;
            }

            switch (b.interact.kind)
            {
            case interaction_kind::none:
                if (went_down)
                {
                    s.started = true;
                    emit(bus, map_name, action_phase::started, i, r.control, r.device, ms{0}, now);
                    emit(bus, map_name, action_phase::performed, i, r.control, r.device, ms{0}, now);
                    s.performed = true;
                }
                else if (down && m_kind != action_kind::button)
                {
                    // A continuous action performs every frame it is away from rest, so a camera driven by one keeps
                    // moving without the game polling for a phase it never gets.
                    emit(bus, map_name, action_phase::performed, i, r.control, r.device, held, now);
                }
                else if (went_up)
                {
                    s.started = false;
                    emit(bus, map_name, action_phase::cancelled, i, r.control, r.device, held, now);
                }
                break;

            case interaction_kind::press:
                if (went_down)
                {
                    emit(bus, map_name, action_phase::started, i, r.control, r.device, ms{0}, now);
                    emit(bus, map_name, action_phase::performed, i, r.control, r.device, ms{0}, now);
                    s.performed = true;
                }
                break;

            case interaction_kind::release:
                if (went_down)
                {
                    s.started = true;
                    emit(bus, map_name, action_phase::started, i, r.control, r.device, ms{0}, now);
                }
                else if (went_up)
                {
                    s.started = false;
                    emit(bus, map_name, action_phase::performed, i, r.control, r.device, held, now);
                }
                break;

            case interaction_kind::hold:
                if (went_down)
                {
                    s.started = true;
                    emit(bus, map_name, action_phase::started, i, r.control, r.device, ms{0}, now);
                }
                else if (down && !s.performed && elapsed_ms(s.down_at, now) >= b.interact.duration)
                {
                    s.performed = true;
                    emit(bus, map_name, action_phase::performed, i, r.control, r.device,
                         elapsed_ms(s.down_at, now), now);
                }
                else if (went_up && !s.performed)
                {
                    // Let go before the time was up: the player changed their mind, so this is a cancellation rather
                    // than a very short hold.
                    s.started = false;
                    emit(bus, map_name, action_phase::cancelled, i, r.control, r.device, held, now);
                }
                break;

            case interaction_kind::tap:
                if (went_down)
                {
                    s.started = true;
                    emit(bus, map_name, action_phase::started, i, r.control, r.device, ms{0}, now);
                }
                else if (went_up)
                {
                    s.started = false;
                    if (held <= b.interact.duration)
                        emit(bus, map_name, action_phase::performed, i, r.control, r.device, held, now);
                    else
                        emit(bus, map_name, action_phase::cancelled, i, r.control, r.device, held, now);
                }
                break;

            case interaction_kind::multi_tap:
                if (went_down)
                {
                    // Too long since the last one: this is the first tap of a new attempt, not the second of the old.
                    if (s.taps != 0 && elapsed_ms(s.last_tap, now) > b.interact.duration)
                        s.taps = 0;

                    ++s.taps;
                    s.last_tap = now;

                    if (s.taps == 1)
                        emit(bus, map_name, action_phase::started, i, r.control, r.device, ms{0}, now);

                    if (s.taps >= std::max<std::uint8_t>(b.interact.count, 1))
                    {
                        s.taps = 0;
                        emit(bus, map_name, action_phase::performed, i, r.control, r.device, ms{0}, now);
                    }
                }
                else if (s.taps != 0 && !down && elapsed_ms(s.last_tap, now) > b.interact.duration)
                {
                    s.taps = 0;
                    emit(bus, map_name, action_phase::cancelled, i, r.control, r.device, ms{0}, now);
                }
                break;
            }

            s.down = down;
        }

        if (!m_held && !m_started_this_frame && !m_performed_this_frame && !m_cancelled_this_frame)
            m_phase = action_phase::idle;

        (void)best_index;
    }

} // namespace catalyst::input
