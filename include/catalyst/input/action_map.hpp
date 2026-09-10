/**
 * @file action_map.hpp
 * @brief Groups of actions, and the contexts that switch between them.
 * @details A map is a set of actions that belong together and are enabled together. That grouping is the answer to
 * input routing, which is otherwise one of the messier parts of a game: the pause menu opens, gameplay stops hearing
 * anything, the menu starts, and nothing anywhere needs an `if (paused)`.
 *
 *     action_set &actions = in.actions();
 *     action_map &play = actions.add_map("gameplay");
 *     action_map &menu = actions.add_map("menu");
 *     ...
 *     actions.enable_only("menu");     // pausing
 *     actions.enable_only("gameplay"); // resuming
 *
 * Maps can also overlap - a "global" map holding screenshot and quit stays enabled alongside whichever of the others
 * is - because `enable()` and `disable()` are per map and `enable_only()` is just the common case spelled once.
 *
 * Within a map, a chord suppresses the plain binding it contains. Bind "S" to `move_back` and "Ctrl+S" to `quicksave`
 * and holding Ctrl+S saves without also walking backwards, which is what the player meant and what they would
 * otherwise have to work around.
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#include <catalyst/input/action.hpp>

#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace catalyst::input
{

    /**
     * @class action_map
     * @brief A named group of actions, enabled and disabled as one.
     */
    class action_map
    {
    public:
        explicit action_map(std::string name);

        action_map(const action_map &) = delete;
        action_map &operator=(const action_map &) = delete;

        [[nodiscard]] std::string_view name() const noexcept { return m_name; }

        /** @brief A disabled map's actions all read zero and never change phase. */
        [[nodiscard]] bool enabled() const noexcept { return m_enabled; }
        void set_enabled(bool on);
        void enable() { set_enabled(true); }
        void disable() { set_enabled(false); }

        // --------------------------------------------------------------------------------------------------------------
        // Actions
        // --------------------------------------------------------------------------------------------------------------

        /**
         * @brief Adds an action, or returns the existing one if @p name is already taken and has the same kind.
         * @note References stay valid for the life of the map, so it is safe to keep the one you get at setup.
         */
        action &add(std::string name, action_kind kind);

        /** @brief Adds an on/off action. */
        action &add_button(std::string name) { return add(std::move(name), action_kind::button); }
        /** @brief Adds a one-axis action. */
        action &add_axis1d(std::string name) { return add(std::move(name), action_kind::axis1d); }
        /** @brief Adds a two-axis action - movement, look, a cursor. */
        action &add_axis2d(std::string name) { return add(std::move(name), action_kind::axis2d); }
        /** @brief Adds a three-axis action. */
        action &add_axis3d(std::string name) { return add(std::move(name), action_kind::axis3d); }

        /** @brief The action with this name, or nullptr. */
        [[nodiscard]] action *find(std::string_view name) noexcept;
        [[nodiscard]] const action *find(std::string_view name) const noexcept;

        /**
         * @brief The action with this name.
         * @throws std::out_of_range if there is none. Use find() where absence is expected; this is for the setup code
         * that has just added it and would rather fail loudly than read zeroes all session.
         */
        [[nodiscard]] action &operator[](std::string_view name);
        [[nodiscard]] const action &operator[](std::string_view name) const;

        /** @brief How many actions the map holds. */
        [[nodiscard]] std::size_t size() const noexcept { return m_actions.size(); }

        /** @brief Calls `fn(action&)` for each action, in the order they were added. */
        template <typename F>
        void for_each(F &&fn)
        {
            for (action &a : m_actions)
                fn(a);
        }

        /** @brief Calls `fn(const action&)` for each action. */
        template <typename F>
        void for_each(F &&fn) const
        {
            for (const action &a : m_actions)
                fn(a);
        }

        // --------------------------------------------------------------------------------------------------------------
        // Driving
        // --------------------------------------------------------------------------------------------------------------

        /** @brief Clears every action's frame edges. */
        void new_frame() noexcept;

        /**
         * @brief Re-evaluates every action against the registry, publishing phase changes to @p bus.
         * @param now The frame's timestamp; passed in so a replay can supply its own and reproduce timings exactly.
         */
        void update(const device_registry &registry, input_time now, events::bus *bus);

    private:
        std::string m_name;
        bool m_enabled{true};
        /** @brief A deque so that references handed out by add() survive later additions. */
        std::deque<action> m_actions;
        /** @brief Scratch reused each update() so a per-frame evaluation allocates nothing. */
        std::vector<control_id> m_suppressed;
    };

    /**
     * @class action_set
     * @brief Every map an application has, and which of them are listening.
     */
    class action_set
    {
    public:
        action_set() = default;

        action_set(const action_set &) = delete;
        action_set &operator=(const action_set &) = delete;

        /** @brief Adds a map, or returns the existing one with that name. References stay valid for the set's life. */
        action_map &add_map(std::string name);

        /** @brief The map with this name, or nullptr. */
        [[nodiscard]] action_map *find_map(std::string_view name) noexcept;
        [[nodiscard]] const action_map *find_map(std::string_view name) const noexcept;

        /**
         * @brief The map with this name.
         * @throws std::out_of_range if there is none.
         */
        [[nodiscard]] action_map &operator[](std::string_view name);

        /** @brief Enables one map, leaving the others alone. */
        void enable(std::string_view name);
        /** @brief Disables one map. */
        void disable(std::string_view name);

        /** @brief Enables @p name and disables every other map - a context switch, in one call. */
        void enable_only(std::string_view name);

        /** @brief Enables every map. */
        void enable_all();
        /** @brief Disables every map. */
        void disable_all();

        /** @brief How many maps the set holds. */
        [[nodiscard]] std::size_t size() const noexcept { return m_maps.size(); }

        /** @brief Calls `fn(action_map&)` for each map, in the order they were added. */
        template <typename F>
        void for_each(F &&fn)
        {
            for (action_map &m : m_maps)
                fn(m);
        }

        /** @brief Calls `fn(const action_map&)` for each map. */
        template <typename F>
        void for_each(F &&fn) const
        {
            for (const action_map &m : m_maps)
                fn(m);
        }

        /**
         * @brief Finds an action by "map/action" - "gameplay/jump" - or by bare name across every map.
         * @return nullptr if no map has it.
         */
        [[nodiscard]] action *find_action(std::string_view path) noexcept;

        /** @brief Clears every map's frame edges. */
        void new_frame() noexcept;

        /** @brief Updates every enabled map. */
        void update(const device_registry &registry, input_time now, events::bus *bus);

    private:
        std::deque<action_map> m_maps;
    };

} // namespace catalyst::input
