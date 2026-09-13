#pragma once

#include <cstddef>
#include <variant>
#include <typeindex>
#include <concepts>

namespace catalyst::events
{

    using event_type_t = std::size_t;

    /**
     * @struct tagged
     * @brief A template struct used to tag events with a unique event type identifier.
     * @tparam Id The unique identifier for the event type.
     */
    template <event_type_t Id>
    struct tagged
    {
        static constexpr event_type_t tag = Id;
    };

    /**
     * @struct static_event_tag
     * @brief A template struct used to retrieve the static event tag of an event type, if it exists.
     * @tparam Event The event type to query for a static event tag.
     */
    template <typename Event>
    struct static_event_tag
    {
    };

    /**
     * @concept has_tag
     * @brief A concept that checks if an event type has a static tag.
     * @tparam Event The event type to check.
     */
    template <typename Event>
    concept has_tag = requires { { Event::tag } -> std::convertible_to<event_type_t>; };

    /**
     * @struct has_static_tag
     * @brief A template struct used to determine if an event type has a static event tag.
     * @tparam Event The event type to query.
     */
    template <typename Event>
        requires has_tag<Event>
    struct static_event_tag<Event>
    {
        static constexpr event_type_t value = Event::tag;
    };

    /**
     * @struct has_static_tag
     * @brief A template struct used to determine if an event type has a static event tag.
     * @tparam Event The event type to query.
     */
    template <typename Event>
        struct has_static_tag
        : std::bool_constant < requires
    {
        {static_event_tag<Event>::value}->std::convertible_to<event_type_t>;
    }>{};

    /**
     * @variable has_static_tag_v
     * @brief A variable template that provides a convenient way to check if an event type has a static event tag.
     * @tparam Event The event type to query.
     */
    template <typename Event>
    inline constexpr bool has_static_tag_v = has_static_tag<Event>::value;

    /**
     * @typedef event_key
     * @brief A variant type that can hold either an event type identifier or a type index.
     */
    using event_key = std::variant<event_type_t, std::type_index>;

    /**
     * @variable invalid_event_key
     * @brief A constant representing an invalid event key.
     */
    constexpr event_key invalid_event_key = event_key{
        std::in_place_index<0>,
        static_cast<event_type_t>(-1)};

    /**
     * @brief Retrieves the event key for a given event type.
     * @tparam Event The event type to query.
     * @return The event key corresponding to the event type.
     */
    template <typename Event>
    constexpr event_key event_id()
    {
        if constexpr (has_static_tag_v<Event>) // If the event has a static tag, use it as the event key.
            return event_key{
                std::in_place_index<0>,
                static_event_tag<Event>::value};
        else // If the event does not have a static tag, use its type index as the event key.
            return event_key{
                std::in_place_index<1>,
                typeid(Event)};
    }

} // namespace catalyst::events