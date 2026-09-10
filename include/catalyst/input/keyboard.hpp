/**
 * @file keyboard.hpp
 * @brief Defines the key_code enumeration, which represents physical keys on a keyboard. The key codes are based on the USB HID Keyboard/Keypad usage IDs (usage page 0x07). This file also includes a function to convert key codes to their corresponding USB HID usage IDs. The key_code enumeration allows for consistent representation of keyboard keys across different platforms and input systems, while text entry is delivered separately via character/text input events.
 * @details The key_code enumeration provides a comprehensive list of physical keys on a keyboard, including letters, digits, function keys, and various control keys. Each key code corresponds to a specific physical key, and the values are aligned with the USB HID standard for keyboard input. This allows for consistent handling of keyboard input across different platforms and input systems. The to_usb_hid function provides a convenient way to convert key codes to their corresponding USB HID usage IDs, which can be useful when interfacing with low-level input APIs or when implementing custom input handling logic that requires knowledge of the underlying USB HID usage IDs. Text entry is handled separately through character or text input events, allowing for a clear distinction between physical key presses and the resulting text input, which can be affected by factors such as keyboard layout and modifier keys.
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

#include <catalyst/input/device.hpp>

/**
 * @namespace catalyst::input
 * @brief The catalyst::input namespace contains definitions and utilities related to input handling in the Catalyst framework, including keyboard input, mouse input, gamepad input, and other input devices. This namespace provides a consistent interface for handling various types of input across different platforms and input systems, allowing developers to easily manage user interactions in their applications. The key_code enumeration, which represents physical keys on a keyboard, is defined within this namespace, along with functions for converting key codes to USB HID usage IDs and handling text input events separately from physical key presses.
 * @details The catalyst::input namespace is designed to provide a comprehensive set of tools and definitions for handling user input in a consistent and efficient manner. By organizing all input-related functionality within this namespace, we can avoid naming conflicts and provide a clear structure for developers to access the various input handling utilities they need for their projects. The key_code enumeration allows for consistent representation of physical keys on a keyboard, while the to_usb_hid function provides a convenient way to convert key codes to their corresponding USB HID usage IDs. Text entry is handled separately through character or text input events, allowing for a clear distinction between physical key presses and the resulting text input, which can be affected by factors such as keyboard layout and modifier keys. This design allows for flexibility in handling different types of input while maintaining a clear and organized structure for developers to work with.
 */
namespace catalyst::input
{

    /**
     * @enum key_code
     * @brief An enumeration representing physical keys on a keyboard, based on the USB HID Keyboard/Keypad usage IDs (usage page 0x07). Each key code corresponds to a specific physical key, allowing for consistent representation of keyboard input across different platforms and input systems. The key codes include letters, digits, function keys, and various control keys, providing a comprehensive list of keys that can be used in applications for handling keyboard input. Text entry is handled separately through character or text input events, allowing for a clear distinction between physical key presses and the resulting text input.
     * @details The key_code enumeration provides a comprehensive list of physical keys on a keyboard, with values aligned to the USB HID standard for keyboard input. This allows for consistent handling of keyboard input across different platforms and input systems. Each key code corresponds to a specific physical key, such as letters (a-z), digits (0-9), function keys (F1-F24), and various control keys (enter, escape, backspace, etc.). By using this enumeration, developers can easily manage keyboard input in their applications while maintaining compatibility with the underlying USB HID usage IDs when needed. Text entry is handled separately through character or text input events, allowing for proper representation of characters that may be produced by various key combinations, keyboard layouts, and modifier keys.
     */
    enum class key_code : std::uint16_t
    {
        unknown = 0,
        a = 4,
        b = 5,
        c = 6,
        d = 7,
        e = 8,
        f = 9,
        g = 10,
        h = 11,
        i = 12,
        j = 13,
        k = 14,
        l = 15,
        m = 16,
        n = 17,
        o = 18,
        p = 19,
        q = 20,
        r = 21,
        s = 22,
        t = 23,
        u = 24,
        v = 25,
        w = 26,
        x = 27,
        y = 28,
        z = 29,
        digit_1 = 30,
        digit_2 = 31,
        digit_3 = 32,
        digit_4 = 33,
        digit_5 = 34,
        digit_6 = 35,
        digit_7 = 36,
        digit_8 = 37,
        digit_9 = 38,
        digit_0 = 39,
        enter = 40,
        escape = 41,
        backspace = 42,
        tab = 43,
        space = 44,
        minus = 45,
        equal = 46,
        left_bracket = 47,
        right_bracket = 48,
        backslash = 49,
        non_us_hash = 50,
        semicolon = 51,
        apostrophe = 52,
        grave_accent = 53,
        comma = 54,
        period = 55,
        slash = 56,
        caps_lock = 57,
        f1 = 58,
        f2 = 59,
        f3 = 60,
        f4 = 61,
        f5 = 62,
        f6 = 63,
        f7 = 64,
        f8 = 65,
        f9 = 66,
        f10 = 67,
        f11 = 68,
        f12 = 69,
        print_screen = 70,
        scroll_lock = 71,
        pause = 72,
        insert = 73,
        home = 74,
        page_up = 75,
        delete_key = 76,
        end = 77,
        page_down = 78,
        right_arrow = 79,
        left_arrow = 80,
        down_arrow = 81,
        up_arrow = 82,
        num_lock = 83,
        keypad_divide = 84,
        keypad_multiply = 85,
        keypad_minus = 86,
        keypad_plus = 87,
        keypad_enter = 88,
        keypad_1 = 89,
        keypad_2 = 90,
        keypad_3 = 91,
        keypad_4 = 92,
        keypad_5 = 93,
        keypad_6 = 94,
        keypad_7 = 95,
        keypad_8 = 96,
        keypad_9 = 97,
        keypad_0 = 98,
        keypad_period = 99,
        non_us_backslash = 100,
        application = 101,
        power = 102,
        keypad_equal = 103,
        f13 = 104,
        f14 = 105,
        f15 = 106,
        f16 = 107,
        f17 = 108,
        f18 = 109,
        f19 = 110,
        f20 = 111,
        f21 = 112,
        f22 = 113,
        f23 = 114,
        f24 = 115,
        execute = 116,
        help = 117,
        menu = 118,
        select = 119,
        stop = 120,
        again = 121,
        undo = 122,
        cut = 123,
        copy = 124,
        paste = 125,
        find = 126,
        mute = 127,
        volume_up = 128,
        volume_down = 129,
        keypad_comma = 133,
        keypad_equal_as400 = 134,
        international1 = 135,
        international2 = 136,
        international3 = 137,
        international4 = 138,
        international5 = 139,
        international6 = 140,
        international7 = 141,
        international8 = 142,
        international9 = 143,
        lang1 = 144,
        lang2 = 145,
        lang3 = 146,
        lang4 = 147,
        lang5 = 148,
        lang6 = 149,
        lang7 = 150,
        lang8 = 151,
        lang9 = 152,
        left_control = 224,
        left_shift = 225,
        left_alt = 226,
        left_super = 227,
        right_control = 228,
        right_shift = 229,
        right_alt = 230,
        right_super = 231
    };

    /**
     * @var key_code_count
     * @brief One past the largest key_code value. Useful for sizing lookup tables indexed by key_code.
     */
    inline constexpr std::size_t key_code_count = 256;

    /**
     * @fn key_name
     * @brief Returns a short, stable, human-readable name for a physical key (e.g. "A", "Left Shift", "Keypad Enter"). The
     * names describe the key's position on a US layout, not the character it produces under the active layout; they are
     * intended for key-binding UIs, logs and configuration files. Returns "Unknown" for key_code::unknown and for values
     * that are not part of the enumeration.
     * @param code The key to name.
     * @return A null-terminated string view with static storage duration.
     */
    [[nodiscard]] std::string_view key_name(key_code code) noexcept;

    /**
     * @fn is_modifier_key
     * @brief True for the eight modifier keys (left/right control, shift, alt and super).
     */
    [[nodiscard]] inline constexpr bool is_modifier_key(key_code code) noexcept
    {
        return code >= key_code::left_control && code <= key_code::right_super;
    }

    /**
     * @fn to_usb_hid
     * @brief Converts a key_code to its corresponding USB HID usage ID. This function takes a key_code enumeration value and returns the corresponding usb_hid value that represents the same physical key according to the USB HID standard for keyboard input (usage page 0x07). If the key_code is unknown, it returns usb_hid_unknown. This conversion is useful when interfacing with low-level input APIs or when implementing custom input handling logic that requires knowledge of the underlying USB HID usage IDs.
     * @param code The key_code value to convert to a USB HID usage ID.
     * @return The corresponding usb_hid value for the given key_code, or usb_hid_unknown if the key_code is unknown.
     * @details The to_usb_hid function provides a convenient way to convert key codes to their corresponding USB HID usage IDs, which can be useful when interfacing with low-level input APIs or when implementing custom input handling logic that requires knowledge of the underlying USB HID usage IDs. By using this function, developers can easily obtain the USB HID usage ID for a given key code, allowing for consistent handling of keyboard input across different platforms and input systems while maintaining compatibility with the underlying USB HID standard.
     */
    [[nodiscard]] inline constexpr usb_hid to_usb_hid(key_code code) noexcept
    {
        if (code == key_code::unknown)
        {
            return usb_hid_unknown;
        }

        return make_usb_hid(usb_hid_page_keyboard, static_cast<std::uint16_t>(code));
    }

    /**
     * @fn from_usb_hid
     * @brief Converts a USB HID usage ID to its corresponding key_code. This function takes a usb_hid value that represents a USB HID usage ID and returns the corresponding key_code enumeration value that represents the same physical key according to the USB HID standard for keyboard input (usage page 0x07). If the usage ID is unknown or does not belong to the keyboard usage page, it returns key_code::unknown. This conversion is useful when interfacing with low-level input APIs or when implementing custom input handling logic that requires knowledge of the underlying USB HID usage IDs.
     * @param usage The usb_hid value to convert to a key_code.
     * @return The corresponding key_code value for the given usb_hid usage ID, or key_code::unknown if the usage ID is unknown or does not belong to the keyboard usage page.
     * @details The from_usb_hid function provides a convenient way to convert USB HID usage IDs to their corresponding key codes, which can be useful when interfacing with low-level input APIs or when implementing custom input handling logic that requires knowledge of the underlying USB HID usage IDs. By using this function, developers can easily obtain the key code for a given USB HID usage ID, allowing for consistent handling of keyboard input across different platforms and input systems while maintaining compatibility with the underlying USB HID standard.
     */
    [[nodiscard]] inline constexpr key_code from_usb_hid(usb_hid usage) noexcept
    {
        if (usage == usb_hid_unknown)
        {
            return key_code::unknown;
        }

        if (usb_hid_page(usage) != usb_hid_page_keyboard)
        {
            return key_code::unknown;
        }

        const auto id = usb_hid_id(usage);
        if (id == 0u)
        {
            return key_code::unknown;
        }

        return static_cast<key_code>(id);
    }

    /**
     * @enum key_modifiers
     * @brief An enumeration representing modifier keys that can be held down in combination with other keys, such as shift, control, alt, super, caps lock, and num lock. This enumeration is used in key events to indicate which modifier keys are active at the time of a key press, allowing for proper handling of key combinations and modified input in applications. By using this enumeration, developers can easily check for specific modifier keys being held down and implement features such as keyboard shortcuts, modified character input, and special behavior based on active modifiers.
     * @details The key_modifiers enumeration includes values for common modifier keys such as shift, control, alt, super (often the Windows or Command key), caps lock, and num lock. The none value indicates that no modifiers are active. By using this enumeration in key events, developers can easily check for specific modifier keys being held down and implement features such as keyboard shortcuts (e.g., Ctrl+C for copy), modified character input (e.g., Shift+1 for '!'), and special behavior based on active modifiers (e.g., Caps Lock affecting letter case), allowing for more versatile and user-friendly input handling in applications.
     */
    enum class key_modifiers : std::uint8_t
    {
        none = 0x00,
        shift = 0x01,
        control = 0x02,
        alt = 0x04,
        super = 0x08,
        caps_lock = 0x10,
        num_lock = 0x20
    };

    /**
     * @fn operator|
     * @brief Bitwise OR operator for key_modifiers enumeration. This operator allows for combining multiple key modifiers into a single key_modifiers value, enabling the representation of multiple active modifiers at the same time. For example, if both shift and control modifiers are active, you can combine them using this operator to create a key_modifiers value that represents both modifiers being held down.
     * @param a The first key_modifiers value to combine.
     * @param b The second key_modifiers value to combine.
     * @return A new key_modifiers value that represents the combination of the two input modifiers using a bitwise OR operation.
     * @details The operator| function allows for combining multiple key modifiers into a single key_modifiers value by performing a bitwise OR operation on the underlying integer representations of the modifiers. This is useful for representing multiple active modifiers at the same time, such as when both shift and control keys are held down. By using this operator, developers can easily create combined modifier values that can be checked against in key events to determine which modifiers are active during a key press.
     */
    [[nodiscard]] inline constexpr key_modifiers operator|(key_modifiers a, key_modifiers b) noexcept
    {
        using u = std::underlying_type_t<key_modifiers>;
        return static_cast<key_modifiers>(static_cast<u>(a) | static_cast<u>(b));
    }
    /**
     * @fn operator&
     * @brief Bitwise AND operator for key_modifiers enumeration. This operator allows for checking if specific modifiers are active within a combined key_modifiers value. For example, if you have a key_modifiers value that represents both shift and control being active, you can use this operator to check if the shift modifier is active by performing a bitwise AND operation with the shift modifier value.
     * @param a The first key_modifiers value to check.
     * @param b The second key_modifiers value to check against.
     * @return A new key_modifiers value that represents the result of the bitwise AND operation between the two input modifiers, which can be used to check for specific active modifiers.
     * @details The operator& function allows for checking if specific modifiers are active within a combined key_modifiers value by performing a bitwise AND operation on the underlying integer representations of the modifiers. This is useful for determining if certain modifiers are active during a key press, such as when both shift and control keys are held down. By using this operator, developers can easily check for specific active modifiers in key events and implement behavior based on which modifiers are currently active.
     */
    [[nodiscard]] inline constexpr key_modifiers operator&(key_modifiers a, key_modifiers b) noexcept
    {
        using u = std::underlying_type_t<key_modifiers>;
        return static_cast<key_modifiers>(static_cast<u>(a) & static_cast<u>(b));
    }
    /**
     * @fn operator^
     * @brief Bitwise XOR operator for key_modifiers enumeration. This operator allows for toggling specific modifiers within a combined key_modifiers value. For example, if you have a key_modifiers value that represents both shift and control being active, you can use this operator to toggle the shift modifier by performing a bitwise XOR operation with the shift modifier value.
     * @param a The first key_modifiers value to toggle.
     * @param b The second key_modifiers value to toggle against.
     * @return A new key_modifiers value that represents the result of the bitwise XOR operation between the two input modifiers, which can be used to toggle specific modifiers.
     * @details The operator^ function allows for toggling specific modifiers within a combined key_modifiers value by performing a bitwise XOR operation on the underlying integer representations of the modifiers. This is useful for changing the state of certain modifiers during a key press, such as when both shift and control keys are held down and you want to toggle the state of one of them. By using this operator, developers can easily toggle specific modifiers in key events and implement behavior based on changes to which modifiers are currently active.
     */
    [[nodiscard]] inline constexpr key_modifiers operator^(key_modifiers a, key_modifiers b) noexcept
    {
        using u = std::underlying_type_t<key_modifiers>;
        return static_cast<key_modifiers>(static_cast<u>(a) ^ static_cast<u>(b));
    }
    /**
     * @fn operator~
     * @brief Bitwise NOT operator for key_modifiers enumeration. This operator allows for inverting the state of all modifiers within a key_modifiers value. For example, if you have a key_modifiers value that represents both shift and control being active, using this operator will produce a new key_modifiers value where shift and control are inactive, and all other modifiers are active.
     * @param a The key_modifiers value to invert.
     * @return A new key_modifiers value that represents the result of the bitwise NOT operation on the input modifier, which can be used to invert the state of all modifiers.
     * @details The operator~ function allows for inverting the state of all modifiers within a key_modifiers value by performing a bitwise NOT operation on the underlying integer representation of the modifiers. This is useful for quickly toggling the state of all modifiers at once, such as when you want to check for the absence of certain modifiers or when you want to create a new modifier state that is the opposite of the current one. By using this operator, developers can easily invert the state of all modifiers in key events and implement behavior based on changes to which modifiers are currently active.
     */
    [[nodiscard]] inline constexpr key_modifiers operator~(key_modifiers a) noexcept
    {
        using u = std::underlying_type_t<key_modifiers>;
        return static_cast<key_modifiers>(~static_cast<u>(a));
    }
    /**
     * @fn operator|=
     * @brief Bitwise OR assignment operator for key_modifiers enumeration. This operator allows for combining multiple key modifiers into a single key_modifiers value and assigning the result back to the first operand. For example, if you have a key_modifiers variable that currently represents the shift modifier being active, you can use this operator to add the control modifier to it by performing a bitwise OR operation with the control modifier value and assigning the result back to the variable.
     * @param a The key_modifiers variable to modify and assign the result to.
     * @param b The key_modifiers value to combine with the first operand.
     * @return A reference to the modified key_modifiers variable that now represents the combination of the original value and the second input modifier using a bitwise OR operation.
     * @details The operator|= function allows for combining multiple key modifiers into a single key_modifiers value and assigning the result back to the first operand by performing a bitwise OR operation on the underlying integer representations of the modifiers. This is useful for representing multiple active modifiers at the same time, such as when both shift and control keys are held down. By using this operator, developers can easily create combined modifier values that can be checked against in key events to determine which modifiers are active during a key press, while also allowing for convenient modification of existing modifier states.
     */
    inline constexpr key_modifiers &operator|=(key_modifiers &a, key_modifiers b) noexcept
    {
        a = (a | b);
        return a;
    }
    /**
     * @fn operator&=
     * @brief Bitwise AND assignment operator for key_modifiers enumeration. This operator allows for checking if specific modifiers are active within a combined key_modifiers value and assigning the result back to the first operand. For example, if you have a key_modifiers variable that currently represents both shift and control being active, you can use this operator to check if the shift modifier is active by performing a bitwise AND operation with the shift modifier value and assigning the result back to the variable.
     * @param a The key_modifiers variable to modify and assign the result to.
     * @param b The key_modifiers value to check against and combine with the first operand.
     * @return A reference to the modified key_modifiers variable that now represents the result of the bitwise AND operation between the original value and the second input modifier, which can be used to check for specific active modifiers.
     * @details The operator&= function allows for checking if specific modifiers are active within a combined key_modifiers value and assigning the result back to the first operand by performing a bitwise AND operation on the underlying integer representations of the modifiers. This is useful for determining if certain modifiers are active during a key press, such as when both shift and control keys are held down. By using this operator, developers can easily check for specific active modifiers in key events and implement behavior based on which modifiers are currently active, while also allowing for convenient modification of existing modifier states.
     */
    inline constexpr key_modifiers &operator&=(key_modifiers &a, key_modifiers b) noexcept
    {
        a = (a & b);
        return a;
    }
    /**
     * @fn operator^=
     * @brief Bitwise XOR assignment operator for key_modifiers enumeration. This operator allows for toggling specific modifiers within a combined key_modifiers value and assigning the result back to the first operand. For example, if you have a key_modifiers variable that currently represents both shift and control being active, you can use this operator to toggle the shift modifier by performing a bitwise XOR operation with the shift modifier value and assigning the result back to the variable.
     * @param a The key_modifiers variable to modify and assign the result to.
     * @param b The key_modifiers value to toggle against and combine with the first operand.
     * @return A reference to the modified key_modifiers variable that now represents the result of the bitwise XOR operation between the original value and the second input modifier, which can be used to toggle specific modifiers.
     * @details The operator^= function allows for toggling specific modifiers within a combined key_modifiers value and assigning the result back to the first operand by performing a bitwise XOR operation on the underlying integer representations of the modifiers. This is useful for changing the state of certain modifiers during a key press, such as when both shift and control keys are held down and you want to toggle the state of one of them. By using this operator, developers can easily toggle specific modifiers in key events and implement behavior based on changes to which modifiers are currently active, while also allowing for convenient modification of existing modifier states.
     */
    inline constexpr key_modifiers &operator^=(key_modifiers &a, key_modifiers b) noexcept
    {
        a = (a ^ b);
        return a;
    }
    /**
     * @fn has_modifier
     * @brief Checks if a specific modifier is active within a combined key_modifiers value. This function takes a key_modifiers value that may represent multiple active modifiers and a specific key_modifiers flag to check for, and returns true if the specified modifier is active within the combined value, or false otherwise. For example, if you have a key_modifiers value that represents both shift and control being active, you can use this function to check if the shift modifier is active by passing the combined value and the shift modifier flag.
     * @param value The combined key_modifiers value to check for the presence of the specified modifier.
     * @param flag The specific key_modifiers flag to check for within the combined value.
     * @return True if the specified modifier is active within the combined key_modifiers value, or false otherwise.
     * @details The has_modifier function allows for checking if a specific modifier is active within a combined key_modifiers value by performing a bitwise AND operation between the combined value and the specified flag, and then checking if the result is not equal to none. This is useful for determining if certain modifiers are active during a key press, such as when both shift and control keys are held down. By using this function, developers can easily check for specific active modifiers in key events and implement behavior based on which modifiers are currently active.
     */
    [[nodiscard]] inline constexpr bool has_modifier(key_modifiers value, key_modifiers flag) noexcept
    {
        return (value & flag) != key_modifiers::none;
    }

    // ------------------------------------------------------------------------------------------------------------------
    // Controls
    // ------------------------------------------------------------------------------------------------------------------

    /**
     * @fn control_of
     * @brief The control slot a key occupies on a keyboard device. Slot index == USB HID usage id, so the layout is the
     * HID keyboard page laid out flat and `from_usb_hid(make_usb_hid(page_keyboard, slot))` recovers the key.
     * @param code The key.
     * @return Its slot, or no_control for key_code::unknown.
     */
    [[nodiscard]] inline constexpr control_id control_of(key_code code) noexcept
    {
        return code == key_code::unknown ? no_control : control_at(static_cast<std::size_t>(code));
    }

    /**
     * @fn key_of
     * @brief The inverse of control_of(): the key a keyboard slot belongs to, or key_code::unknown.
     */
    [[nodiscard]] inline constexpr key_code key_of(control_id control) noexcept
    {
        if (!control.valid() || control.index >= key_code_count || control.index == 0)
            return key_code::unknown;
        return static_cast<key_code>(control.index);
    }

    /**
     * @fn keyboard_layout
     * @brief The layout every keyboard device shares: key_code_count button controls, named by key_name().
     * @details Returned by reference to a function-local static, so it is built once, on first use, and every keyboard
     * device points at the same object.
     */
    [[nodiscard]] const layout_ref &keyboard_layout();

    // ------------------------------------------------------------------------------------------------------------------
    // Events
    // ------------------------------------------------------------------------------------------------------------------

    /**
     * @struct key_event
     * @brief One physical key press, auto-repeat or release. Key events describe *physical* keys (see key_code); for the
     * characters the user actually typed, listen for text_input_event instead (see text.hpp).
     * @details When a window loses focus the platform layer synthesises a release for every key it still considers held,
     * so no consumer is left with a key stuck down.
     */
    struct key_event : device_event<tags::key>
    {
        /** @brief The platform::window_id of the window that had keyboard focus, or 0 if there was none. */
        std::uint64_t window{0};
        /** @brief The physical key, or key_code::unknown if the platform could not map it. */
        key_code code{key_code::unknown};
        /**
         * @brief The platform's raw scan code (Win32: the 8-bit scan code, with 0xE000 added for extended keys), or 0
         * when the platform supplied none. Meaningful only to the platform that produced it; useful for asking the OS
         * for a localised key name.
         */
        std::uint32_t scancode{0};
        /** @brief Whether the key went down, auto-repeated, or came up. */
        button_action action{button_action::press};
        /** @brief The modifiers and lock states active when the event was generated. */
        key_modifiers modifiers{key_modifiers::none};

        /** @brief The slot this event changed. */
        [[nodiscard]] constexpr control_id control() const noexcept { return control_of(code); }
        /** @brief True for a press or a repeat. */
        [[nodiscard]] constexpr bool down() const noexcept { return is_down_action(action); }
    };

} // namespace catalyst::input
