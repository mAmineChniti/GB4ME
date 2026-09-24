// InputManager implementation: controller discovery, raw state tracking and
// the logical button aggregate. See include/gb/input.h for the design.
#include "gb/input.h"

#include <algorithm>

#include "gb/joypad.h"
#include "gba/keypad.h"

namespace gb
{

std::string pad_label(const Binding &b)
{
	switch (b.kind) {
	case BindKind::None:
		return "-";
	case BindKind::Key: {
		const char *n = SDL_GetScancodeName(static_cast<SDL_Scancode>(b.code));
		return (n != nullptr && n[0] != '\0') ? n : "?";
	}
	case BindKind::Button: {
		const char *n = SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(b.code));
		return (n != nullptr && n[0] != '\0') ? n : "btn";
	}
	case BindKind::AxisNeg:
	case BindKind::AxisPos: {
		const char *n = SDL_GetGamepadStringForAxis(static_cast<SDL_GamepadAxis>(b.code));
		std::string base = (n != nullptr && n[0] != '\0') ? n : "axis";
		base += (b.kind == BindKind::AxisNeg) ? "-" : "+";
		return base;
	}
	}
	return "-";
}

const char *pad_button_name(PadButton b)
{
	switch (b) {
	case PadButton::A:
		return "A";
	case PadButton::B:
		return "B";
	case PadButton::Select:
		return "Select";
	case PadButton::Start:
		return "Start";
	case PadButton::Right:
		return "Right";
	case PadButton::Left:
		return "Left";
	case PadButton::Up:
		return "Up";
	case PadButton::Down:
		return "Down";
	case PadButton::R:
		return "R";
	case PadButton::L:
		return "L";
	case PadButton::Count:
		break;
	}
	return "?";
}

InputManager::~InputManager()
{
	closeControllers();
}

void InputManager::openControllers()
{
	int count = 0;
	if (SDL_JoystickID *ids = SDL_GetGamepads(&count)) {
		for (int i = 0; i < count; ++i)
			handleHotplug(ids[i], true);
		SDL_free(ids);
	}
}

void InputManager::closeControllers()
{
	for (PadDevice &d : pads_) {
		if (d.pad != nullptr)
			SDL_CloseGamepad(d.pad);
		d.pad = nullptr;
	}
	pads_.clear();
}

std::string InputManager::controllerName(unsigned i) const
{
	if (i >= pads_.size() || pads_[i].name.empty())
		return "Controller " + std::to_string(i + 1);
	return pads_[i].name;
}

int InputManager::findPad(SDL_JoystickID instance) const
{
	for (size_t i = 0; i < pads_.size(); ++i)
		if (pads_[i].instance == instance)
			return static_cast<int>(i);
	return -1;
}

void InputManager::handleHotplug(SDL_JoystickID instance, bool added)
{
	const int existing = findPad(instance);
	if (!added) {
		if (existing >= 0) {
			PadDevice &d = pads_[static_cast<size_t>(existing)];
			if (d.pad != nullptr)
				SDL_CloseGamepad(d.pad);
			pads_.erase(pads_.begin() + existing);
			releaseAll();
		}
		return;
	}
	if (existing >= 0)
		return;
	// The GameController API gives consistent button/axis numbering across
	// pads, so a pad SDL cannot recognise is simply not supported.
	SDL_Gamepad *pad = SDL_OpenGamepad(instance);
	if (pad == nullptr)
		return;
	PadDevice d;
	d.instance = instance;
	d.pad = pad;
	if (const char *nm = SDL_GetGamepadName(pad); nm != nullptr && nm[0] != '\0')
		d.name = nm;
	pads_.push_back(d);
}

void InputManager::handleEvent(const SDL_Event &ev)
{
	switch (ev.type) {
	case SDL_EVENT_KEY_DOWN:
	case SDL_EVENT_KEY_UP: {
		const int sc = static_cast<int>(ev.key.scancode);
		if (sc >= 0 && sc < static_cast<int>(keys_down_.size()))
			keys_down_[static_cast<size_t>(sc)] = (ev.type == SDL_EVENT_KEY_DOWN);
		break;
	}
	case SDL_EVENT_GAMEPAD_ADDED:
		handleHotplug(static_cast<SDL_JoystickID>(ev.gdevice.which), true);
		break;
	case SDL_EVENT_GAMEPAD_REMOVED:
		handleHotplug(static_cast<SDL_JoystickID>(ev.gdevice.which), false);
		break;
	case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
	case SDL_EVENT_GAMEPAD_BUTTON_UP: {
		const int idx = findPad(static_cast<SDL_JoystickID>(ev.gbutton.which));
		if (idx >= 0) {
			const int b = static_cast<int>(ev.gbutton.button);
			if (b >= 0 && b < static_cast<int>(SDL_GAMEPAD_BUTTON_COUNT)) {
				pads_[static_cast<size_t>(idx)].buttons[static_cast<size_t>(b)] =
					(ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
			}
		}
		break;
	}
	case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
		const int idx = findPad(static_cast<SDL_JoystickID>(ev.gaxis.which));
		if (idx >= 0) {
			const int a = static_cast<int>(ev.gaxis.axis);
			if (a >= 0 && a < static_cast<int>(SDL_GAMEPAD_AXIS_COUNT)) {
				pads_[static_cast<size_t>(idx)].axes[static_cast<size_t>(a)] = ev.gaxis.value;
			}
		}
		break;
	}
	default:
		break;
	}
}

void InputManager::setSuppressed(SDL_Scancode sc, bool on)
{
	const int i = static_cast<int>(sc);
	if (i >= 0 && i < static_cast<int>(keys_suppressed_.size()))
		keys_suppressed_[static_cast<size_t>(i)] = on;
}

bool InputManager::keyActive(const Binding &b) const
{
	if (b.kind != BindKind::Key)
		return false;
	const int sc = b.code;
	if (sc < 0 || sc >= static_cast<int>(keys_down_.size()))
		return false;
	const size_t i = static_cast<size_t>(sc);
	return keys_down_[i] && !keys_suppressed_[i];
}

bool InputManager::padActive(const PadDevice &d, const Binding &b) const
{
	switch (b.kind) {
	case BindKind::Button: {
		const int i = b.code;
		return i >= 0 && i < static_cast<int>(d.buttons.size()) &&
			   d.buttons[static_cast<size_t>(i)];
	}
	case BindKind::AxisNeg: {
		const int i = b.code;
		return i >= 0 && i < static_cast<int>(d.axes.size()) &&
			   d.axes[static_cast<size_t>(i)] < -axis_threshold_;
	}
	case BindKind::AxisPos: {
		const int i = b.code;
		return i >= 0 && i < static_cast<int>(d.axes.size()) &&
			   d.axes[static_cast<size_t>(i)] > axis_threshold_;
	}
	default:
		return false;
	}
}

void InputManager::update()
{
	for (unsigned i = 0; i < kPadButtonCount; ++i) {
		bool down = keyActive(key_map_[i]);
		if (!down) {
			for (const PadDevice &d : pads_) {
				if (padActive(d, pad_map_[i]) || padActive(d, pad_map2_[i])) {
					down = true;
					break;
				}
			}
		}
		pressed_[i] = down ? 1u : 0u;
	}
}

void InputManager::releaseAll()
{
	keys_down_.fill(false);
	keys_suppressed_.fill(false);
	pressed_.fill(0);
	for (PadDevice &d : pads_) {
		d.buttons.fill(false);
		d.axes.fill(0);
	}
}

bool InputManager::autoMap()
{
	if (pads_.empty())
		return false;
	// A conventional layout that works on Xbox/PlayStation/Switch-style
	// pads: the face buttons map to A/B, the shoulders to L/R, the d-pad and
	// the left stick both drive the directions, and the menu buttons map to
	// Start/Select. Pads that lack a right stick simply leave that source
	// unused.
	const auto face = Binding::makeButton;
	const auto axis = Binding::makeAxis;
	pad_map_[static_cast<unsigned>(PadButton::A)] = face(SDL_GAMEPAD_BUTTON_SOUTH);
	pad_map_[static_cast<unsigned>(PadButton::B)] = face(SDL_GAMEPAD_BUTTON_EAST);
	pad_map_[static_cast<unsigned>(PadButton::Start)] = face(SDL_GAMEPAD_BUTTON_START);
	pad_map_[static_cast<unsigned>(PadButton::Select)] = face(SDL_GAMEPAD_BUTTON_BACK);
	pad_map_[static_cast<unsigned>(PadButton::L)] = face(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
	pad_map_[static_cast<unsigned>(PadButton::R)] = face(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
	pad_map_[static_cast<unsigned>(PadButton::Up)] = face(SDL_GAMEPAD_BUTTON_DPAD_UP);
	pad_map_[static_cast<unsigned>(PadButton::Down)] = face(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
	pad_map_[static_cast<unsigned>(PadButton::Left)] = face(SDL_GAMEPAD_BUTTON_DPAD_LEFT);
	pad_map_[static_cast<unsigned>(PadButton::Right)] = face(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
	// The left stick mirrors the d-pad as a SECONDARY source, so thumbstick
	// play works without overriding the d-pad binding.
	pad_map2_[static_cast<unsigned>(PadButton::Up)] = axis(SDL_GAMEPAD_AXIS_LEFTY, false);
	pad_map2_[static_cast<unsigned>(PadButton::Down)] = axis(SDL_GAMEPAD_AXIS_LEFTY, true);
	pad_map2_[static_cast<unsigned>(PadButton::Left)] = axis(SDL_GAMEPAD_AXIS_LEFTX, false);
	pad_map2_[static_cast<unsigned>(PadButton::Right)] = axis(SDL_GAMEPAD_AXIS_LEFTX, true);
	return true;
}

void InputManager::applyTo(Joypad &joypad, gba::GbaKeypad &keypad) const
{
	// Joypad::Key is ordered Right,Left,Up,Down,A,B,Select,Start while
	// PadButton is A,B,Select,Start,Right,Left,Up,Down,R,L, so the two must
	// be mapped explicitly: casting one to the other would send A to Right.
	static constexpr Joypad::Key kGbMap[kPadButtonCount] = {
		Joypad::Key::A,      // PadButton::A
		Joypad::Key::B,      // B
		Joypad::Key::Select, // Select
		Joypad::Key::Start,  // Start
		Joypad::Key::Right,  // Right
		Joypad::Key::Left,   // Left
		Joypad::Key::Up,     // Up
		Joypad::Key::Down,   // Down
	};
	// The GB/GBC joypad has no R/L; those two slots stay untouched.
	for (unsigned i = 0; i < 8; ++i)
		joypad.set_key(kGbMap[i], isPressed(static_cast<PadButton>(i)));
	// GbaKeypad::Key shares PadButton's order, so all ten map directly.
	for (unsigned i = 0; i < kPadButtonCount; ++i)
		keypad.setKey(static_cast<gba::GbaKeypad::Key>(i), pressed_[i] != 0);
}

} // namespace gb
