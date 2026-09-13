#include "../test_common.hpp"

#include <catalyst/input/keyboard.hpp>
#include <catalyst/input/mouse.hpp>
#include <catalyst/input/hid.hpp>
#include <catalyst/input/text.hpp>

#include <string_view>
#include <type_traits>

using namespace catalyst::input;

namespace
{
    void test_usb_hid_packing()
    {
        const usb_hid h = make_usb_hid(usb_hid_page_keyboard, 0x04);
        CT_REQUIRE(usb_hid_page(h) == usb_hid_page_keyboard);
        CT_REQUIRE(usb_hid_id(h) == 0x04);
        CT_REQUIRE(h == 0x00070004u);
    }

    void test_key_code_hid_round_trip()
    {
        CT_REQUIRE(to_usb_hid(key_code::a) == make_usb_hid(usb_hid_page_keyboard, 4));
        CT_REQUIRE(to_usb_hid(key_code::unknown) == usb_hid_unknown);

        for (std::uint16_t v = 4; v <= 231; ++v)
        {
            const auto code = static_cast<key_code>(v);
            CT_REQUIRE(from_usb_hid(to_usb_hid(code)) == code);
        }

        CT_REQUIRE(from_usb_hid(usb_hid_unknown) == key_code::unknown);
        CT_REQUIRE(from_usb_hid(make_usb_hid(usb_hid_page_keyboard, 0)) == key_code::unknown);
        // Wrong page: a Generic Desktop usage must not turn into a key.
        CT_REQUIRE(from_usb_hid(make_usb_hid(usb_hid_page_generic_desktop, 4)) == key_code::unknown);
    }

    void test_modifier_keys()
    {
        CT_REQUIRE(static_cast<std::uint16_t>(key_code::left_control) == 224);
        CT_REQUIRE(static_cast<std::uint16_t>(key_code::right_super) == 231);
        CT_REQUIRE(is_modifier_key(key_code::left_shift));
        CT_REQUIRE(is_modifier_key(key_code::right_alt));
        CT_REQUIRE(!is_modifier_key(key_code::a));
        CT_REQUIRE(!is_modifier_key(key_code::caps_lock));
        CT_REQUIRE(static_cast<std::size_t>(key_code::right_super) < key_code_count);
    }

    void test_modifier_flags()
    {
        key_modifiers m = key_modifiers::none;
        CT_REQUIRE(!has_modifier(m, key_modifiers::shift));

        m |= key_modifiers::shift;
        m |= key_modifiers::control;
        CT_REQUIRE(has_modifier(m, key_modifiers::shift));
        CT_REQUIRE(has_modifier(m, key_modifiers::control));
        CT_REQUIRE(!has_modifier(m, key_modifiers::alt));

        m &= ~key_modifiers::shift;
        CT_REQUIRE(!has_modifier(m, key_modifiers::shift));
        CT_REQUIRE(has_modifier(m, key_modifiers::control));

        m ^= key_modifiers::control;
        CT_REQUIRE(m == key_modifiers::none);
    }

    void test_key_names()
    {
        CT_REQUIRE(key_name(key_code::a) == "A");
        CT_REQUIRE(key_name(key_code::digit_0) == "0");
        CT_REQUIRE(key_name(key_code::left_shift) == "Left Shift");
        CT_REQUIRE(key_name(key_code::keypad_enter) == "Keypad Enter");
        CT_REQUIRE(key_name(key_code::unknown) == "Unknown");
        CT_REQUIRE(key_name(static_cast<key_code>(200)) == "Unknown");
        CT_REQUIRE(key_name(static_cast<key_code>(0xFFFF)) == "Unknown");

        // Every enumerator has a name that is not the fallback.
        for (std::uint16_t v = 4; v <= 152; ++v)
        {
            if ((v >= 130 && v <= 132)) // not part of the enumeration (locking keys)
                continue;
            CT_REQUIRE(key_name(static_cast<key_code>(v)) != "Unknown");
        }
        for (std::uint16_t v = 224; v <= 231; ++v)
            CT_REQUIRE(key_name(static_cast<key_code>(v)) != "Unknown");
    }

    void test_text_input_event()
    {
        static_assert(std::is_trivially_copyable_v<std::array<char32_t, text_input_event::inline_capacity>>);

        text_input_event empty;
        CT_REQUIRE(empty.empty());
        CT_REQUIRE(empty.size() == 0);
        CT_REQUIRE(empty.text().empty());

        text_input_event one(U'x');
        CT_REQUIRE(one.size() == 1);
        CT_REQUIRE(one.text() == U"x");

        const std::u32string_view long_text = U"0123456789abcdefghij"; // 20 code points
        text_input_event truncated(long_text);
        CT_REQUIRE(truncated.size() == text_input_event::inline_capacity);
        CT_REQUIRE(truncated.full());
        CT_REQUIRE(truncated.text() == long_text.substr(0, text_input_event::inline_capacity));
        CT_REQUIRE(!truncated.push_back(U'!'));
        CT_REQUIRE(truncated.size() == text_input_event::inline_capacity);

        text_input_event built;
        CT_REQUIRE(built.push_back(U'a'));
        CT_REQUIRE(built.push_back(U'\U0001F600')); // non-BMP is a single code point
        CT_REQUIRE(built.text() == U"a\U0001F600");

        // Copies own their text; the view must point into the copy, not the original.
        text_input_event copy = built;
        built.clear();
        CT_REQUIRE(built.empty());
        CT_REQUIRE(copy.text() == U"a\U0001F600");
        CT_REQUIRE(copy.text().data() != built.text().data());

        copy.assign(U"xyz");
        CT_REQUIRE(copy.text() == U"xyz");
    }

    void test_event_tags_are_distinct()
    {
        // Every input event carries a static tag so the bus dispatches on a constant rather than typeid. Two events
        // sharing one would silently deliver each to the other's listeners.
        static_assert(key_event::tag != text_input_event::tag);
        static_assert(mouse_move_event::tag != mouse_button_event::tag);
        static_assert(mouse_wheel_event::tag != mouse_raw_move_event::tag);
        static_assert(mouse_enter_event::tag != mouse_leave_event::tag);

        // ... and every one of them is inside the block this module owns.
        static_assert(key_event::tag >= tags::input_base);
        static_assert(key_event::tag < tags::input_base + 0x1'0000u);
    }

    void test_key_controls_round_trip()
    {
        // A key's control slot is its HID usage id, which is what lets a keyboard layout be a flat array.
        CT_REQUIRE(control_of(key_code::a).index == 4);
        CT_REQUIRE(control_of(key_code::unknown) == no_control);
        CT_REQUIRE(key_of(control_of(key_code::space)) == key_code::space);
        CT_REQUIRE(key_of(no_control) == key_code::unknown);

        for (std::uint16_t v = 4; v <= 231; ++v)
        {
            const auto code = static_cast<key_code>(v);
            CT_REQUIRE(key_of(control_of(code)) == code);
        }

        const layout_ref &layout = keyboard_layout();
        CT_REQUIRE(layout->size() == key_code_count);
        CT_REQUIRE(layout->kind() == device_kind::keyboard);
        CT_REQUIRE(layout->name_of(control_of(key_code::a)) == "A");
        CT_REQUIRE(layout->kind_of(control_of(key_code::a)) == control_kind::button);
    }

    void test_mouse_button_sets()
    {
        CT_REQUIRE(to_mouse_buttons(mouse_button::left) == mouse_buttons::left);
        CT_REQUIRE(to_mouse_buttons(mouse_button::x2) == mouse_buttons::x2);
        CT_REQUIRE(to_mouse_buttons(mouse_button::unknown) == mouse_buttons::none);

        mouse_buttons set = mouse_buttons::none;
        set |= mouse_buttons::left;
        set |= mouse_buttons::middle;
        CT_REQUIRE(has_button(set, mouse_button::left));
        CT_REQUIRE(has_button(set, mouse_button::middle));
        CT_REQUIRE(!has_button(set, mouse_button::right));
        CT_REQUIRE(!has_button(set, mouse_button::unknown));

        set &= ~mouse_buttons::left;
        CT_REQUIRE(!has_button(set, mouse_button::left));
        CT_REQUIRE(has_button(set, mouse_button::middle));

        // ~ never sets bits outside the five buttons.
        CT_REQUIRE((~mouse_buttons::none) == (mouse_buttons::left | mouse_buttons::right | mouse_buttons::middle | mouse_buttons::x1 | mouse_buttons::x2));
    }
} // namespace

int main()
{
    test_usb_hid_packing();
    test_key_code_hid_round_trip();
    test_modifier_keys();
    test_modifier_flags();
    test_key_names();
    test_text_input_event();
    test_event_tags_are_distinct();
    test_key_controls_round_trip();
    test_mouse_button_sets();
    return 0;
}
