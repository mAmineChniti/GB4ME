// Unified keyboard + gamepad input for GB/GBC and GBA.
//
// Each logical button (A, B, Select, Start, D-pad, L, R) has TWO independent
// bindings: one keyboard key and one controller source (button or axis).
// Either one alone drives it.
//
// State is RECOMPUTED from raw device state every frame rather than forwarded
// on edges, so a logical button stays held while ANY bound source holds it:
// pressing A on the pad while A is also down on the keyboard releases only
// when both are up, and two controllers holding the same button cannot clear
// each other.
#ifndef GB4ME_INPUT_H
#define GB4ME_INPUT_H

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "gb/types.h"

// Forward declarations at global scope: the GBA keypad lives in the
// top-level gba namespace, not nested inside gb.
namespace gb
{
class Joypad;
}
namespace gba
{
class GbaKeypad;
}

namespace gb
{

// Logical buttons, in the same order as the unified settings table.
enum class PadButton : u8 {
	A = 0,
	B,
	Select,
	Start,
	Right,
	Left,
	Up,
	Down,
	R,
	L,
	Count,
};

constexpr unsigned kPadButtonCount = static_cast<unsigned>(PadButton::Count);

// Human-readable logical button name, shared by the settings panel and the
// remap prompt so both spell the same ten buttons identically.
const char *pad_button_name(PadButton b);

// What kind of physical input drives a logical button.
enum class BindKind : u8 {
	None = 0,
	Key,     // SDL_Scancode
	Button,  // SDL_GamepadButton
	AxisNeg, // axis pushed negative (stick left/up)
	AxisPos, // axis pushed positive  (stick right/down)
};

struct Binding {
	BindKind kind = BindKind::None;
	int code = 0;

	bool operator==(const Binding &o) const
	{
		return kind == o.kind && code == o.code;
	}
	bool operator!=(const Binding &o) const
	{
		return !(*this == o);
	}
	bool bound() const
	{
		return kind != BindKind::None;
	}
	// Packed for the settings file: (kind << 16) | code.
	int packed() const
	{
		return (static_cast<int>(kind) << 16) | (code & 0xFFFF);
	}
	static Binding unpack(int v)
	{
		Binding b;
		const int kind = (v >> 16) & 0xF;
		b.kind = (kind >= 0 && kind <= 4) ? static_cast<BindKind>(kind) : BindKind::None;
		b.code = v & 0xFFFF;
		if (!b.bound())
			b.code = 0;
		return b;
	}
	// Convenience: a binding for a plain keyboard scancode.
	static Binding makeKey(SDL_Scancode sc)
	{
		Binding b;
		if (sc != SDL_SCANCODE_UNKNOWN) {
			b.kind = BindKind::Key;
			b.code = static_cast<int>(sc);
		}
		return b;
	}
	// Convenience: a binding for a controller face/menu/shoulder/d-pad
	// button, and for one direction of an analogue axis.
	static Binding makeButton(SDL_GamepadButton btn)
	{
		Binding b;
		b.kind = BindKind::Button;
		b.code = static_cast<int>(btn);
		return b;
	}
	static Binding makeAxis(SDL_GamepadAxis axis, bool positive)
	{
		Binding b;
		b.kind = positive ? BindKind::AxisPos : BindKind::AxisNeg;
		b.code = static_cast<int>(axis);
		return b;
	}
};

// ASCII-only label for a binding (SDL names contain UTF-8, which the
// overlay font cannot render).
std::string pad_label(const Binding &b);

using BindingTable = std::array<Binding, kPadButtonCount>;

// One opened controller plus its raw state.
struct PadDevice {
	SDL_JoystickID instance = 0;
	SDL_Gamepad *pad = nullptr;
	std::string name;
	std::array<bool, SDL_GAMEPAD_BUTTON_COUNT> buttons{};
	std::array<Sint16, SDL_GAMEPAD_AXIS_COUNT> axes{};
};

class InputManager
{
  public:
	InputManager() = default;
	~InputManager();

	InputManager(const InputManager &) = delete;
	InputManager &operator=(const InputManager &) = delete;

	// Opens every currently connected controller. Safe to call repeatedly.
	void openControllers();
	void closeControllers();
	unsigned controllerCount() const
	{
		return static_cast<unsigned>(pads_.size());
	}
	// Name of a connected controller, for the device dropdown.
	std::string controllerName(unsigned i) const;

	// Feeds one SDL event: keyboard state, gamepad hotplug, buttons, axes.
	void handleEvent(const SDL_Event &ev);

	// Recomputes logical button state from raw device state. Call once per
	// frame after pumping events.
	void update();

	bool isPressed(PadButton b) const
	{
		return pressed_[static_cast<unsigned>(b)] != 0;
	}

	// Axis activation threshold. Default sits well inside the stick travel
	// so a resting stick never triggers a direction.
	int axisThreshold() const
	{
		return axis_threshold_;
	}

	// Binding tables (the two halves of the mapping).
	const BindingTable &padBindings() const
	{
		return pad_map_;
	}
	void setKeyBinding(PadButton b, const Binding &v)
	{
		key_map_[static_cast<unsigned>(b)] = v;
	}
	void setPadBinding(PadButton b, const Binding &v)
	{
		pad_map_[static_cast<unsigned>(b)] = v;
	}
	// Optional SECOND controller source per button, so auto-map can bind
	// both the d-pad and the left stick to a direction.
	const Binding &padBinding2(PadButton b) const
	{
		return pad_map2_[static_cast<unsigned>(b)];
	}
	void setPadBinding2(PadButton b, const Binding &v)
	{
		pad_map2_[static_cast<unsigned>(b)] = v;
	}

	// One-click default layout from the first connected controller.
	bool autoMap();

	// Pushes current logical state into an emulated joypad / keypad.
	// Idempotent: both devices only act on real changes.
	void applyTo(Joypad &joypad, class gba::GbaKeypad &keypad) const;

	// Drops all held state (window focus loss, device removal).
	void releaseAll();

	// Marks a key as owned by the frontend (menu/shortcut/remap) so it can
	// never also drive the emulated pad, and so a release always clears it
	// symmetrically. Gameplay-relevant keys stay unsuppressed.
	void setSuppressed(SDL_Scancode sc, bool on);

  private:
	void handleHotplug(SDL_JoystickID instance, bool added);
	int findPad(SDL_JoystickID instance) const;
	bool padActive(const PadDevice &d, const Binding &b) const;
	bool keyActive(const Binding &b) const;

	std::vector<PadDevice> pads_;
	BindingTable key_map_{};
	BindingTable pad_map_{};
	BindingTable pad_map2_{};
	std::array<bool, SDL_SCANCODE_COUNT> keys_down_{};
	std::array<bool, SDL_SCANCODE_COUNT> keys_suppressed_{};
	std::array<u8, kPadButtonCount> pressed_{};
	int axis_threshold_ = 12000;
};

} // namespace gb

#endif // GB4ME_INPUT_H
