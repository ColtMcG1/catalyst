/**
 * @file action_map.cpp
 * @brief Implementation of action_map and action_set.
 * License: CDDL-1.0 (see LICENSE).
 */

#include <catalyst/input/action_map.hpp>

#include <algorithm>
#include <stdexcept>

namespace catalyst::input
{

    // ------------------------------------------------------------------------------------------------------------------
    // action_map
    // ------------------------------------------------------------------------------------------------------------------

    action_map::action_map(std::string name) : m_name(std::move(name)) {}

    void action_map::set_enabled(bool on)
    {
        if (m_enabled == on)
            return;
        m_enabled = on;

        // Push the change down: an action disabled with the map drops whatever it was holding, so nothing fires on the
        // frame the map comes back.
        for (action &a : m_actions)
            a.set_enabled(on);
    }

    action &action_map::add(std::string name, action_kind kind)
    {
        if (action *existing = find(name))
        {
            // Same name and same kind is the caller setting up twice; same name and a different kind is a mistake worth
            // hearing about rather than a silently reshaped action.
            if (existing->kind() != kind)
                throw std::invalid_argument("catalyst::input::action_map::add: '" + name + "' already exists with a different kind");
            return *existing;
        }

        m_actions.emplace_back(std::move(name), kind);
        action &a = m_actions.back();
        a.set_enabled(m_enabled);
        return a;
    }

    action *action_map::find(std::string_view name) noexcept
    {
        for (action &a : m_actions)
        {
            if (a.name() == name)
                return &a;
        }
        return nullptr;
    }

    const action *action_map::find(std::string_view name) const noexcept
    {
        return const_cast<action_map *>(this)->find(name);
    }

    action &action_map::operator[](std::string_view name)
    {
        if (action *a = find(name))
            return *a;
        throw std::out_of_range("catalyst::input::action_map: no action named '" + std::string(name) + "'");
    }

    const action &action_map::operator[](std::string_view name) const
    {
        return const_cast<action_map &>(*this)[name];
    }

    void action_map::new_frame() noexcept
    {
        for (action &a : m_actions)
            a.new_frame();
    }

    void action_map::update(const device_registry &registry, input_time now, events::bus *bus)
    {
        if (!m_enabled)
            return;

        // Work out which plain bindings a satisfied chord is shadowing before evaluating anything, so the order actions
        // were added in cannot change the answer.
        m_suppressed.clear();
        for (const action &a : m_actions)
            a.collect_active_chords(registry, m_suppressed);

        for (action &a : m_actions)
            a.evaluate(registry, now, bus, m_name, m_suppressed);
    }

    // ------------------------------------------------------------------------------------------------------------------
    // action_set
    // ------------------------------------------------------------------------------------------------------------------

    action_map &action_set::add_map(std::string name)
    {
        if (action_map *existing = find_map(name))
            return *existing;
        m_maps.emplace_back(std::move(name));
        return m_maps.back();
    }

    action_map *action_set::find_map(std::string_view name) noexcept
    {
        for (action_map &m : m_maps)
        {
            if (m.name() == name)
                return &m;
        }
        return nullptr;
    }

    const action_map *action_set::find_map(std::string_view name) const noexcept
    {
        return const_cast<action_set *>(this)->find_map(name);
    }

    action_map &action_set::operator[](std::string_view name)
    {
        if (action_map *m = find_map(name))
            return *m;
        throw std::out_of_range("catalyst::input::action_set: no map named '" + std::string(name) + "'");
    }

    void action_set::enable(std::string_view name)
    {
        if (action_map *m = find_map(name))
            m->enable();
    }

    void action_set::disable(std::string_view name)
    {
        if (action_map *m = find_map(name))
            m->disable();
    }

    void action_set::enable_only(std::string_view name)
    {
        for (action_map &m : m_maps)
            m.set_enabled(m.name() == name);
    }

    void action_set::enable_all()
    {
        for (action_map &m : m_maps)
            m.enable();
    }

    void action_set::disable_all()
    {
        for (action_map &m : m_maps)
            m.disable();
    }

    action *action_set::find_action(std::string_view path) noexcept
    {
        const std::size_t slash = path.find('/');
        if (slash != std::string_view::npos)
        {
            action_map *m = find_map(path.substr(0, slash));
            return m ? m->find(path.substr(slash + 1)) : nullptr;
        }

        for (action_map &m : m_maps)
        {
            if (action *a = m.find(path))
                return a;
        }
        return nullptr;
    }

    void action_set::new_frame() noexcept
    {
        for (action_map &m : m_maps)
            m.new_frame();
    }

    void action_set::update(const device_registry &registry, input_time now, events::bus *bus)
    {
        for (action_map &m : m_maps)
            m.update(registry, now, bus);
    }

} // namespace catalyst::input
