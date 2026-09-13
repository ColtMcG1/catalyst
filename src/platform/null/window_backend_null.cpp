/**
 * @file window_backend_null.cpp
 * @brief The headless implementation of the Catalyst platform window backend. It keeps the same observable behaviour as a
 * real backend -- ids, per-window state, the event bus and input feed seams -- entirely in memory, so code that drives
 * windows can be built and tested on machines with no window system at all.
 * License: MIT (see LICENSE).
 */

#include "../detail_backend.hpp"

#include <catalyst/events/bus.hpp>
#include <catalyst/input/feed.hpp>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <unordered_map>

namespace catalyst::platform::detail
{

    namespace
    {
        struct window_state
        {
            window_id id = 0;
            std::string title;
            platform::rect_px rect_px{};
            math::vec2<std::int32_t> position_px{0, 0};
            math::vec2<std::int32_t> min_size_px{0, 0};
            math::vec2<std::int32_t> max_size_px{0, 0};
            float dpi_scale = 1.0f;
            float opacity = 1.0f;
            cursor_mode cursor = cursor_mode::normal;
            window_display_state display = window_display_state::restored;
            bool visible = true;
            bool resizable = true;
            bool fullscreen = false;
            bool always_on_top = false;
            bool dark_mode = false;
            frame_callback frame_cb = nullptr;
            void *frame_user = nullptr;
        };

        std::uint64_t g_next_window_id = 1;
        std::unordered_map<window_id, window_state> g_windows;

        /**
         * @var g_bus
         * @brief The bus window events are dispatched to, or nullptr when nobody is listening.
         */
        events::bus *g_bus = nullptr;

        /**
         * @var g_feed
         * @brief The feed window-sourced input is delivered to, or nullptr. A headless backend has no OS to translate
         * input from, so it never calls the feed itself; it is held so that input_feed() reports what was installed and a
         * test can check the wiring without a window system.
         */
        input::event_feed *g_feed = nullptr;

        /**
         * @fn dispatch_event
         * @brief Dispatches a window event to the installed bus, or drops it if there is none.
         * @details The bus keys on the static event type, so there is nothing to stamp and nothing to allocate: the event
         * is dispatched from the value itself. Events are not retained when no bus is installed -- see
         * platform::set_event_bus for why there is no queue behind this any more.
         */
        template <typename E>
        void dispatch_event(E e)
        {
            if (g_bus)
                g_bus->dispatch(std::move(e));
        }

        std::int32_t resolve_px(const ui::length &v, ui::axis a, float dpi_scale) noexcept
        {
            ui::resolve_context ctx{};
            ctx.dpi_scale = dpi_scale;
            ctx.dpi_x = dpi_scale * 96.0f;
            ctx.dpi_y = dpi_scale * 96.0f;
            const float px = ui::resolve_or(v, a, ctx, 0.0f);
            return static_cast<std::int32_t>(px);
        }

        window_state *state_from_id(window_id id) noexcept
        {
            auto it = g_windows.find(id);
            return (it == g_windows.end()) ? nullptr : &it->second;
        }

        void dispatch_created_events(window_id id, const window_state &s)
        {
            window_resized_event e;
            e.window = id;
            e.width_px = ui::px(static_cast<float>(s.rect_px.size().x()));
            e.height_px = ui::px(static_cast<float>(s.rect_px.size().y()));
            dispatch_event(e);

            window_moved_event m;
            m.window = id;
            m.position_px = s.position_px;
            dispatch_event(m);

            window_dpi_changed_event dpi;
            dpi.window = id;
            dpi.dpi_scale = s.dpi_scale;
            dispatch_event(dpi);
        }

        /** @brief Publishes a display state transition, if it is one. */
        void set_display_state(window_state &s, window_display_state state)
        {
            if (s.display == state)
                return;

            s.display = state;

            window_display_state_event e;
            e.window = s.id;
            e.state = state;
            dispatch_event(e);
        }

        /** @brief Resizes a window's client area and publishes the resize, honouring any size limits. */
        void resize_client(window_state &s, math::vec2<std::int32_t> size_px)
        {
            if (s.min_size_px.x() > 0)
                size_px.x() = std::max(size_px.x(), s.min_size_px.x());
            if (s.min_size_px.y() > 0)
                size_px.y() = std::max(size_px.y(), s.min_size_px.y());
            if (s.max_size_px.x() > 0)
                size_px.x() = std::min(size_px.x(), s.max_size_px.x());
            if (s.max_size_px.y() > 0)
                size_px.y() = std::min(size_px.y(), s.max_size_px.y());

            s.rect_px.min = {0, 0};
            s.rect_px.max = size_px;

            window_resized_event e;
            e.window = s.id;
            e.width_px = ui::px(static_cast<float>(size_px.x()));
            e.height_px = ui::px(static_cast<float>(size_px.y()));
            dispatch_event(e);
        }
    } // namespace

    window_id create_window(const window_desc &desc)
    {
        const window_id id = g_next_window_id++;

        window_state s;
        s.id = id;
        s.title = desc.title ? desc.title : "Catalyst";
        s.visible = desc.visible;
        s.resizable = desc.resizable;
        s.rect_px.min = {0, 0};
        s.rect_px.max = {resolve_px(desc.width_px, ui::axis::x, s.dpi_scale),
                         resolve_px(desc.height_px, ui::axis::y, s.dpi_scale)};

        g_windows.emplace(id, s);
        dispatch_created_events(id, s);
        return id;
    }

    void destroy_window(window_id id) noexcept
    {
        auto it = g_windows.find(id);
        if (it == g_windows.end())
            return;

        g_windows.erase(it);

        window_destroyed_event e;
        e.window = id;
        dispatch_event(e);
    }

    bool is_window_valid(window_id id) noexcept
    {
        return g_windows.find(id) != g_windows.end();
    }

    native_handle get_native_handle(window_id /*id*/) noexcept
    {
        return {};
    }

    rect_px client_rect_px(window_id id) noexcept
    {
        const window_state *s = state_from_id(id);
        return s ? s->rect_px : rect_px{};
    }

    float dpi_scale(window_id id) noexcept
    {
        const window_state *s = state_from_id(id);
        return s ? s->dpi_scale : 1.0f;
    }

    void pump_events() noexcept
    {
        // No OS.
    }

    bool wait_events(std::uint32_t timeout_ms) noexcept
    {
        // There is no OS to wake this backend up: a headless window only ever changes because the application itself
        // called into this module, on this thread, which it cannot do while blocked here. So no wait can produce an event,
        // and this reports that rather than pretending otherwise. A finite timeout still elapses, so a loop that paces
        // itself with wait_events() keeps its timing headless; an infinite one returns at once instead of deadlocking.
        if (timeout_ms != 0xFFFFFFFFu && timeout_ms != 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));

        return false;
    }

    void set_event_bus(events::bus *bus) noexcept
    {
        g_bus = bus;
    }

    events::bus *event_bus() noexcept
    {
        return g_bus;
    }

    void set_input_feed(input::event_feed *feed) noexcept
    {
        g_feed = feed;
    }

    input::event_feed *input_feed() noexcept
    {
        return g_feed;
    }

    void set_cursor_mode(window_id id, cursor_mode mode) noexcept
    {
        if (window_state *s = state_from_id(id))
            s->cursor = mode;
    }

    cursor_mode get_cursor_mode(window_id id) noexcept
    {
        const window_state *s = state_from_id(id);
        return s ? s->cursor : cursor_mode::normal;
    }

    /**
     * @note There is no message pump here, so nothing ever calls the stored callback. It is still recorded so that
     * get/set round-trips and lifetime handling behave identically to a real backend.
     */
    void set_frame_callback(window_id id, frame_callback cb, void *user) noexcept
    {
        if (window_state *s = state_from_id(id))
        {
            s->frame_cb = cb;
            s->frame_user = user;
        }
    }

    void set_title(window_id id, const char *utf8_title) noexcept
    {
        if (window_state *s = state_from_id(id))
            s->title = utf8_title ? utf8_title : "";
    }

    void set_client_size(window_id id, ui::length width_px, ui::length height_px) noexcept
    {
        window_state *s = state_from_id(id);
        if (!s)
            return;

        resize_client(*s, {resolve_px(width_px, ui::axis::x, s->dpi_scale),
                           resolve_px(height_px, ui::axis::y, s->dpi_scale)});
    }

    void set_position(window_id id, const math::vec2<std::int32_t> &position_px) noexcept
    {
        window_state *s = state_from_id(id);
        if (!s || (s->position_px.x() == position_px.x() && s->position_px.y() == position_px.y()))
            return;

        s->position_px = position_px;

        window_moved_event e;
        e.window = id;
        e.position_px = position_px;
        dispatch_event(e);
    }

    math::vec2<std::int32_t> position_px(window_id id) noexcept
    {
        const window_state *s = state_from_id(id);
        return s ? s->position_px : math::vec2<std::int32_t>{};
    }

    void set_size_limits(window_id id, const math::vec2<std::int32_t> &min_px, const math::vec2<std::int32_t> &max_px) noexcept
    {
        window_state *s = state_from_id(id);
        if (!s)
            return;

        s->min_size_px = {std::max(0, min_px.x()), std::max(0, min_px.y())};
        s->max_size_px = {std::max(0, max_px.x()), std::max(0, max_px.y())};

        resize_client(*s, s->rect_px.size());
    }

    void show_window(window_id id) noexcept
    {
        if (window_state *s = state_from_id(id))
            s->visible = true;
    }

    void hide_window(window_id id) noexcept
    {
        if (window_state *s = state_from_id(id))
            s->visible = false;
    }

    void minimize_window(window_id id) noexcept
    {
        if (window_state *s = state_from_id(id))
            set_display_state(*s, window_display_state::minimized);
    }

    void maximize_window(window_id id) noexcept
    {
        if (window_state *s = state_from_id(id))
            set_display_state(*s, window_display_state::maximized);
    }

    void restore_window(window_id id) noexcept
    {
        if (window_state *s = state_from_id(id))
            set_display_state(*s, window_display_state::restored);
    }

    void focus_window(window_id id) noexcept
    {
        window_state *s = state_from_id(id);
        if (!s)
            return;

        set_display_state(*s, window_display_state::restored);

        window_focus_event e;
        e.window = id;
        e.focused = true;
        dispatch_event(e);
    }

    void request_attention(window_id /*id*/) noexcept
    {
        // No desktop environment to ask.
    }

    window_display_state display_state(window_id id) noexcept
    {
        const window_state *s = state_from_id(id);
        return s ? s->display : window_display_state::restored;
    }

    void set_resizable(window_id id, bool resizable) noexcept
    {
        if (window_state *s = state_from_id(id))
            s->resizable = resizable;
    }

    bool is_resizable(window_id id) noexcept
    {
        const window_state *s = state_from_id(id);
        return s && s->resizable;
    }

    void set_always_on_top(window_id id, bool on_top) noexcept
    {
        if (window_state *s = state_from_id(id))
            s->always_on_top = on_top;
    }

    void set_opacity(window_id id, float opacity) noexcept
    {
        if (window_state *s = state_from_id(id))
            s->opacity = std::clamp(opacity, 0.0f, 1.0f);
    }

    void set_fullscreen(window_id id, bool fullscreen) noexcept
    {
        if (window_state *s = state_from_id(id))
            s->fullscreen = fullscreen;
    }

    bool is_fullscreen(window_id id) noexcept
    {
        const window_state *s = state_from_id(id);
        return s && s->fullscreen;
    }

    void set_dark_mode(window_id id, bool dark) noexcept
    {
        if (window_state *s = state_from_id(id))
            s->dark_mode = dark;
    }

} // namespace catalyst::platform::detail
