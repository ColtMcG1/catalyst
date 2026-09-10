/**
 * @file hid.hpp
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
	inline constexpr std::uint16_t usb_hid_page_simulation = 0x02u;
	inline constexpr std::uint16_t usb_hid_page_keyboard = 0x07u;
	inline constexpr std::uint16_t usb_hid_page_led = 0x08u;
	inline constexpr std::uint16_t usb_hid_page_button = 0x09u;
	inline constexpr std::uint16_t usb_hid_page_digitizer = 0x0Du;

	// Generic Desktop usages a backend needs to recognise a device and name its axes. These are the ids that let the
	// win32 Raw Input backend turn an unknown stick into a device_layout without a table per product.
	inline constexpr std::uint16_t usb_hid_usage_pointer = 0x01u;
	inline constexpr std::uint16_t usb_hid_usage_mouse = 0x02u;
	inline constexpr std::uint16_t usb_hid_usage_joystick = 0x04u;
	inline constexpr std::uint16_t usb_hid_usage_gamepad = 0x05u;
	inline constexpr std::uint16_t usb_hid_usage_keyboard = 0x06u;
	inline constexpr std::uint16_t usb_hid_usage_multi_axis = 0x08u;
	inline constexpr std::uint16_t usb_hid_usage_x = 0x30u;
	inline constexpr std::uint16_t usb_hid_usage_y = 0x31u;
	inline constexpr std::uint16_t usb_hid_usage_z = 0x32u;
	inline constexpr std::uint16_t usb_hid_usage_rx = 0x33u;
	inline constexpr std::uint16_t usb_hid_usage_ry = 0x34u;
	inline constexpr std::uint16_t usb_hid_usage_rz = 0x35u;
	inline constexpr std::uint16_t usb_hid_usage_slider = 0x36u;
	inline constexpr std::uint16_t usb_hid_usage_dial = 0x37u;
	inline constexpr std::uint16_t usb_hid_usage_wheel = 0x38u;
	inline constexpr std::uint16_t usb_hid_usage_hat_switch = 0x39u;

	// Simulation Controls usages, which is where a wheel or a HOTAS puts its pedals and throttle.
	inline constexpr std::uint16_t usb_hid_usage_throttle = 0xBBu;
	inline constexpr std::uint16_t usb_hid_usage_rudder = 0xBAu;
	inline constexpr std::uint16_t usb_hid_usage_accelerator = 0xC4u;
	inline constexpr std::uint16_t usb_hid_usage_brake = 0xC5u;
	inline constexpr std::uint16_t usb_hid_usage_clutch = 0xC6u;
	inline constexpr std::uint16_t usb_hid_usage_steering = 0xC8u;

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

