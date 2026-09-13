/**
 * @file registry.cpp
 * @brief Implementation of device_registry.
 * License: CDDL-1.0 (see LICENSE).
 */

#include <catalyst/input/registry.hpp>

#include <algorithm>
#include <utility>

namespace catalyst::input
{
    namespace
    {
        /** @brief Slot indices are 16-bit, so this is the hard ceiling on simultaneously connected devices. */
        constexpr std::size_t k_max_slots = 0xFFFEu;
    } // namespace

    device_registry::slot_data *device_registry::find_slot(device_id id) noexcept
    {
        if (!id.valid() || id.index >= m_slots.size())
            return nullptr;
        slot_data &s = m_slots[id.index];
        return s.generation == id.generation ? &s : nullptr;
    }

    const device_registry::slot_data *device_registry::find_slot(device_id id) const noexcept
    {
        return const_cast<device_registry *>(this)->find_slot(id);
    }

    device_id device_registry::add_device(device_info info, layout_ref layout)
    {
        if (!layout)
            layout = std::make_shared<const device_layout>();
        if (info.name.empty())
            info.name = std::string(device_kind_name(info.kind));

        std::uint16_t index{};
        if (!m_free.empty())
        {
            index = m_free.back();
            m_free.pop_back();
        }
        else
        {
            if (m_slots.size() >= k_max_slots)
                return no_device;
            index = static_cast<std::uint16_t>(m_slots.size());
            m_slots.emplace_back();
        }

        // Generation 0 marks a free slot, so it is skipped on wrap rather than handed out.
        if (m_next_generation == 0)
            m_next_generation = 1;
        const std::uint16_t generation = m_next_generation++;

        slot_data &s = m_slots[index];
        s.info = std::move(info);
        s.layout = std::move(layout);
        s.values.assign(s.layout->size(), 0.0f);
        s.generation = generation;

        const device_id id{index, generation};
        m_live.push_back(id);

        device_connected_event e;
        e.device = id;
        e.info = s.info;
        e.layout = s.layout;
        publish(std::move(e));
        return id;
    }

    void device_registry::remove_device(device_id id)
    {
        slot_data *s = find_slot(id);
        if (!s)
            return;

        // Reset before announcing, so a listener that reads control values from the disconnect event sees them at rest
        // rather than frozen at whatever was held when the cable came out.
        std::fill(s->values.begin(), s->values.end(), 0.0f);

        device_disconnected_event e;
        e.device = id;
        e.info = s->info;

        m_live.erase(std::remove(m_live.begin(), m_live.end(), id), m_live.end());

        s->generation = 0;
        s->layout.reset();
        s->values.clear();
        s->values.shrink_to_fit();
        const device_info info = std::exchange(s->info, device_info{});
        m_free.push_back(id.index);

        e.info = info;
        publish(std::move(e));
    }

    void device_registry::clear()
    {
        // remove_device() mutates m_live, so work from a copy.
        const std::vector<device_id> live = m_live;
        for (const device_id id : live)
            remove_device(id);
    }

    bool device_registry::alive(device_id id) const noexcept
    {
        return find_slot(id) != nullptr;
    }

    const device_info *device_registry::info(device_id id) const noexcept
    {
        const slot_data *s = find_slot(id);
        return s ? &s->info : nullptr;
    }

    const device_layout *device_registry::layout(device_id id) const noexcept
    {
        const slot_data *s = find_slot(id);
        return s ? s->layout.get() : nullptr;
    }

    layout_ref device_registry::layout_ref_of(device_id id) const noexcept
    {
        const slot_data *s = find_slot(id);
        return s ? s->layout : layout_ref{};
    }

    device_id device_registry::find(const device_selector &sel) const noexcept
    {
        for (const device_id id : m_live)
        {
            const slot_data *s = find_slot(id);
            if (s && sel.matches(id, s->info.kind, s->info.slot))
                return id;
        }
        return no_device;
    }

    device_id device_registry::find(device_kind kind, std::uint32_t slot) const noexcept
    {
        return find(device_selector::of(kind, slot));
    }

    float device_registry::value(device_id id, control_id control) const noexcept
    {
        const slot_data *s = find_slot(id);
        if (!s || !control.valid() || control.index >= s->values.size())
            return 0.0f;
        return s->values[control.index];
    }

    bool device_registry::is_down(device_id id, control_id control, float press_point) const noexcept
    {
        return value(id, control) >= press_point;
    }

    std::span<const float> device_registry::values(device_id id) const noexcept
    {
        const slot_data *s = find_slot(id);
        return s ? std::span<const float>(s->values) : std::span<const float>{};
    }

    bool device_registry::set_value_silent(device_id id, control_id control, float value) noexcept
    {
        slot_data *s = find_slot(id);
        if (!s || !control.valid() || control.index >= s->values.size())
            return false;

        float &slot = s->values[control.index];
        if (slot == value)
            return false;
        slot = value;
        return true;
    }

    bool device_registry::set_value(device_id id, control_id control, float value)
    {
        const slot_data *s = find_slot(id);
        if (!s || !control.valid() || control.index >= s->values.size())
            return false;

        const float previous = s->values[control.index];
        if (previous == value)
            return false;

        const device_kind kind = s->info.kind;
        const control_kind ck = s->layout->kind_of(control);
        (void)set_value_silent(id, control, value);

        control_changed_event e;
        e.device = id;
        e.kind = kind;
        e.control = control;
        e.control_kind_ = ck;
        e.value = value;
        e.previous = previous;
        publish(std::move(e));
        return true;
    }

    void device_registry::clear_deltas() noexcept
    {
        for (slot_data &s : m_slots)
        {
            if (s.generation == 0 || !s.layout)
                continue;

            const auto controls = s.layout->controls();
            const std::size_t n = std::min(controls.size(), s.values.size());
            for (std::size_t i = 0; i < n; ++i)
            {
                if (controls[i].kind == control_kind::delta)
                    s.values[i] = 0.0f;
            }
        }
    }

    void device_registry::reset_device(device_id id)
    {
        const slot_data *s = find_slot(id);
        if (!s)
            return;

        // Copy the indices that need clearing first: publishing re-enters user code, which may add or remove devices
        // and so invalidate the slot pointer.
        std::vector<control_id> changed;
        for (std::size_t i = 0; i < s->values.size(); ++i)
        {
            if (s->values[i] != 0.0f)
                changed.push_back(control_at(i));
        }

        for (const control_id c : changed)
            set_value(id, c, 0.0f);
    }

} // namespace catalyst::input
