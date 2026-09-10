/**
 * @file usb.hpp
 * @brief Defines the USB HID usage type and related functions for the Catalyst Input module. This file provides a way to represent USB HID usages in a compact form, allowing for easy handling of USB input devices such as keyboards, mice, and gamepads. The usb_hid type is a 32-bit unsigned integer that encodes both the usage page and usage ID, making it simple to work with USB HID usages in a consistent manner across different platforms and input systems.
 * @details The usb_hid type is designed to represent USB HID usages in a compact and efficient way. It combines the usage page (a 16-bit value) and usage ID (another 16-bit value) into a single 32-bit unsigned integer. This allows for easy storage and comparison of USB HID usages without needing to manage separate values for the page and ID. The make_usb_hid function provides a convenient way to create a usb_hid value from a given usage page and ID, while the usb_hid_page and usb_hid_id functions allow you to extract the page and ID from an existing usb_hid value. By using these types and functions, developers can easily work with USB HID usages in their applications, enabling support for a wide range of USB input devices.
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#include <cstdint>

namespace catalyst::input {

	// Packed USB HID usage: (page << 16) | id
	// - Page: USB HID Usage Page (e.g. 0x07 = Keyboard/Keypad)
	// - Id:   Usage ID within that page
	using usb_hid = std::uint32_t;

	inline constexpr usb_hid usb_hid_unknown = 0u;

	inline constexpr std::uint16_t usb_hid_page_generic_desktop = 0x01u;
	inline constexpr std::uint16_t usb_hid_page_keyboard = 0x07u;
	inline constexpr std::uint16_t usb_hid_page_button = 0x09u;

	[[nodiscard]] inline constexpr usb_hid make_usb_hid(std::uint16_t page, std::uint16_t id) noexcept
	{
		return (static_cast<usb_hid>(page) << 16) | static_cast<usb_hid>(id);
	}

	[[nodiscard]] inline constexpr std::uint16_t usb_hid_page(usb_hid usage) noexcept
	{
		return static_cast<std::uint16_t>((usage >> 16) & 0xFFFFu);
	}

	[[nodiscard]] inline constexpr std::uint16_t usb_hid_id(usb_hid usage) noexcept
	{
		return static_cast<std::uint16_t>(usage & 0xFFFFu);
	}

} // namespace catalyst::input

