/**
 * @file test_registry.cpp
 * @brief device_registry: handles, generations, control values, and the events it publishes.
 * License: CDDL-1.0 (see LICENSE).
 */

#include "../test_common.hpp"

#include <catalyst/events/bus.hpp>
#include <catalyst/input/gamepad.hpp>
#include <catalyst/input/keyboard.hpp>
#include <catalyst/input/mouse.hpp>
#include <catalyst/input/registry.hpp>

#include <string>
#include <vector>

using namespace catalyst::input;
namespace events = catalyst::events;

namespace
{
    device_info make_info(device_kind kind, std::uint32_t slot = any_slot, std::string name = {})
    {
        device_info info;
        info.kind = kind;
        info.slot = slot;
        info.name = std::move(name);
        return info;
    }

    void test_add_and_remove()
    {
        events::bus bus;
        device_registry reg(bus);

        CT_REQUIRE(reg.size() == 0);
        CT_REQUIRE(!reg.alive(no_device));

        const device_id kb = reg.add_device(make_info(device_kind::keyboard), keyboard_layout());
        CT_REQUIRE(kb.valid());
        CT_REQUIRE(reg.alive(kb));
        CT_REQUIRE(reg.size() == 1);
        CT_REQUIRE(reg.info(kb)->kind == device_kind::keyboard);
        // An empty name is filled in from the kind rather than left blank in a device list.
        CT_REQUIRE(reg.info(kb)->name == "keyboard");
        CT_REQUIRE(reg.layout(kb)->size() == key_code_count);

        reg.remove_device(kb);
        CT_REQUIRE(!reg.alive(kb));
        CT_REQUIRE(reg.size() == 0);
        CT_REQUIRE(reg.info(kb) == nullptr);
        CT_REQUIRE(reg.layout(kb) == nullptr);
    }

    void test_stale_handles_do_not_alias()
    {
        events::bus bus;
        device_registry reg(bus);

        const device_id first = reg.add_device(make_info(device_kind::gamepad, 0), gamepad_layout());
        reg.set_value(first, control_of(gamepad_button::a), 1.0f);
        reg.remove_device(first);

        // The freed slot is reused, but the generation moves on - so the old handle must not reach the new device.
        const device_id second = reg.add_device(make_info(device_kind::gamepad, 0), gamepad_layout());
        CT_REQUIRE(second.index == first.index);
        CT_REQUIRE(second.generation != first.generation);
        CT_REQUIRE(second != first);

        CT_REQUIRE(!reg.alive(first));
        CT_REQUIRE(reg.alive(second));
        CT_REQUIRE(reg.value(first, control_of(gamepad_button::a)) == 0.0f);
        CT_REQUIRE(!reg.set_value(first, control_of(gamepad_button::a), 1.0f));
        CT_REQUIRE(reg.value(second, control_of(gamepad_button::a)) == 0.0f);
    }

    void test_control_values()
    {
        events::bus bus;
        device_registry reg(bus);
        const device_id pad = reg.add_device(make_info(device_kind::gamepad, 0), gamepad_layout());

        const control_id a = control_of(gamepad_button::a);
        CT_REQUIRE(reg.value(pad, a) == 0.0f);
        CT_REQUIRE(!reg.is_down(pad, a));

        CT_REQUIRE(reg.set_value(pad, a, 1.0f));
        CT_REQUIRE(reg.value(pad, a) == 1.0f);
        CT_REQUIRE(reg.is_down(pad, a));

        // Writing the same value again is not a change, which is what callers diff on to decide whether to publish.
        CT_REQUIRE(!reg.set_value(pad, a, 1.0f));

        // Out-of-range and null controls are answered, not crashed on.
        CT_REQUIRE(reg.value(pad, no_control) == 0.0f);
        CT_REQUIRE(reg.value(pad, control_at(9999)) == 0.0f);
        CT_REQUIRE(!reg.set_value(pad, control_at(9999), 1.0f));
    }

    void test_publishes_control_changes()
    {
        events::bus bus;
        device_registry reg(bus);

        std::vector<control_changed_event> seen;
        const auto token = bus.add_listener<control_changed_event>(
            [&](const control_changed_event &e) { seen.push_back(e); });

        const device_id pad = reg.add_device(make_info(device_kind::gamepad, 0), gamepad_layout());
        reg.set_value(pad, control_of(gamepad_axis::left_x), 0.5f);
        reg.set_value(pad, control_of(gamepad_axis::left_x), 0.5f); // no change, no event
        reg.set_value(pad, control_of(gamepad_axis::left_x), 0.25f);

        CT_REQUIRE(seen.size() == 2);
        CT_REQUIRE(seen[0].device == pad);
        CT_REQUIRE(seen[0].kind == device_kind::gamepad);
        CT_REQUIRE(seen[0].control == control_of(gamepad_axis::left_x));
        CT_REQUIRE(seen[0].previous == 0.0f);
        CT_REQUIRE(seen[0].value == 0.5f);
        CT_REQUIRE(seen[0].control_kind_ == control_kind::axis);
        CT_REQUIRE(seen[1].previous == 0.5f);
        CT_REQUIRE(seen[1].value == 0.25f);
        // Every event is stamped, so a recorder never has to invent a time.
        CT_REQUIRE(seen[0].time != input_time{});
    }

    void test_connect_and_disconnect_events()
    {
        events::bus bus;
        device_registry reg(bus);

        int connects = 0;
        int disconnects = 0;
        device_info disconnected_info;

        const auto t1 = bus.add_listener<device_connected_event>([&](const device_connected_event &e)
                                                                 {
            ++connects;
            CT_REQUIRE(e.layout != nullptr);
            CT_REQUIRE(e.info.kind == device_kind::gamepad); });
        const auto t2 = bus.add_listener<device_disconnected_event>([&](const device_disconnected_event &e)
                                                                    {
            ++disconnects;
            disconnected_info = e.info; });

        const device_id pad = reg.add_device(make_info(device_kind::gamepad, 3, "Test Pad"), gamepad_layout());
        CT_REQUIRE(connects == 1);

        reg.remove_device(pad);
        CT_REQUIRE(disconnects == 1);
        // The identity survives the device, so a listener can still say which player just unplugged.
        CT_REQUIRE(disconnected_info.name == "Test Pad");
        CT_REQUIRE(disconnected_info.slot == 3);
    }

    void test_disconnect_resets_controls_first()
    {
        events::bus bus;
        device_registry reg(bus);

        const device_id pad = reg.add_device(make_info(device_kind::gamepad, 0), gamepad_layout());
        reg.set_value(pad, control_of(gamepad_button::a), 1.0f);

        // The order matters: a consumer tracking held buttons must see the release before it is told the pad is gone,
        // or it is left holding A forever.
        std::vector<std::string> order;
        const auto t1 = bus.add_listener<control_changed_event>(
            [&](const control_changed_event &) { order.emplace_back("control"); });
        const auto t2 = bus.add_listener<device_disconnected_event>(
            [&](const device_disconnected_event &) { order.emplace_back("disconnect"); });

        reg.reset_device(pad);
        reg.remove_device(pad);

        CT_REQUIRE(order.size() == 2);
        CT_REQUIRE(order[0] == "control");
        CT_REQUIRE(order[1] == "disconnect");
    }

    void test_selectors()
    {
        events::bus bus;
        device_registry reg(bus);

        const device_id kb = reg.add_device(make_info(device_kind::keyboard), keyboard_layout());
        const device_id pad0 = reg.add_device(make_info(device_kind::gamepad, 0), gamepad_layout());
        const device_id pad1 = reg.add_device(make_info(device_kind::gamepad, 1), gamepad_layout());

        // "Any gamepad" is the one plugged in longest, which is the pad a single-player game should follow.
        CT_REQUIRE(reg.find(device_selector::of(device_kind::gamepad)) == pad0);
        CT_REQUIRE(reg.find(device_selector::of(device_kind::gamepad, 1)) == pad1);
        CT_REQUIRE(reg.find(device_selector::of(device_kind::gamepad, 7)) == no_device);
        CT_REQUIRE(reg.find(device_selector::of(device_kind::keyboard)) == kb);
        CT_REQUIRE(reg.find(device_selector::exact(pad1)) == pad1);
        CT_REQUIRE(reg.find(device_selector::of(device_kind::midi)) == no_device);

        // A kind of `any` matches everything, which is what "press any key to rebind" needs.
        CT_REQUIRE(reg.find(device_selector{}) == kb);

        std::size_t pads = 0;
        reg.for_each(device_selector::of(device_kind::gamepad), [&](device_id) { ++pads; });
        CT_REQUIRE(pads == 2);
    }

    void test_clear_deltas()
    {
        events::bus bus;
        device_registry reg(bus);
        const device_id mouse = reg.add_device(make_info(device_kind::mouse), mouse_layout());

        reg.set_value(mouse, control_of(mouse_axis::x), 100.0f);
        reg.set_value(mouse, control_of(mouse_axis::delta_x), 12.0f);
        reg.set_value(mouse, control_of(mouse_axis::wheel_y), 1.0f);

        reg.clear_deltas();

        // Deltas describe a frame and mean nothing across one; an absolute position persists.
        CT_REQUIRE(reg.value(mouse, control_of(mouse_axis::delta_x)) == 0.0f);
        CT_REQUIRE(reg.value(mouse, control_of(mouse_axis::wheel_y)) == 0.0f);
        CT_REQUIRE(reg.value(mouse, control_of(mouse_axis::x)) == 100.0f);
    }

    void test_clear_removes_everything()
    {
        events::bus bus;
        device_registry reg(bus);
        reg.add_device(make_info(device_kind::keyboard), keyboard_layout());
        reg.add_device(make_info(device_kind::mouse), mouse_layout());
        reg.add_device(make_info(device_kind::gamepad, 0), gamepad_layout());

        int disconnects = 0;
        const auto token = bus.add_listener<device_disconnected_event>(
            [&](const device_disconnected_event &) { ++disconnects; });

        reg.clear();
        CT_REQUIRE(reg.size() == 0);
        CT_REQUIRE(disconnects == 3);
    }
} // namespace

int main()
{
    test_add_and_remove();
    test_stale_handles_do_not_alias();
    test_control_values();
    test_publishes_control_changes();
    test_connect_and_disconnect_events();
    test_disconnect_resets_controls_first();
    test_selectors();
    test_clear_deltas();
    test_clear_removes_everything();
    return 0;
}
