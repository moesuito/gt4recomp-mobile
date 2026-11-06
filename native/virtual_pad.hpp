#pragma once
// TASK-185: the on-screen virtual pad of the Android build. The player's layout file (the contract in
// android/virtual-pad/README of the launcher's editor: `android/virtual-pad/default-layout.json`) names elements in
// screen fractions; this file parses and validates that file, turns touches into PS2 pad state and merges that state
// with the physical controller's, so the game keeps reading one pad (port 1) exactly as before.
//
// The layout model (LayoutElement, Layout, parse_layout, load_layout) and the input mapping (Pad, stick_bytes,
// dpad_buttons, merge) are separate on purpose: a later task swaps layouts by game context (a menu layout against a
// racing one) by handing another Layout to Pad::set_layout, and nothing in the input path changes.
//
// No Android, Kotlin or SDL dependency: virtual_pad_fixture runs the same code on the build machine.
#include "pad_mapping.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace gt4::vpad {

// What an element does. The contract's `type` field, one to one.
enum class ElementType : std::uint8_t { button, dpad, stick };

// PS2 button bits, the same indices the controls table uses (controls.hpp `inputs`): the digit lives in the pad state's
// button word and in its pressure byte array.
namespace pbutton {
inline constexpr std::uint8_t select = 0;
inline constexpr std::uint8_t l3 = 1;
inline constexpr std::uint8_t r3 = 2;
inline constexpr std::uint8_t start = 3;
inline constexpr std::uint8_t up = 4;
inline constexpr std::uint8_t right = 5;
inline constexpr std::uint8_t down = 6;
inline constexpr std::uint8_t left = 7;
inline constexpr std::uint8_t l2 = 8;
inline constexpr std::uint8_t r2 = 9;
inline constexpr std::uint8_t l1 = 10;
inline constexpr std::uint8_t r1 = 11;
inline constexpr std::uint8_t triangle = 12;
inline constexpr std::uint8_t circle = 13;
inline constexpr std::uint8_t cross = 14;
inline constexpr std::uint8_t square = 15;
} // namespace pbutton

// One drawable element of the layout, already validated.
struct LayoutElement {
  std::string id;         // the contract's `id`, for the log lines (may be empty: the binds name is used then)
  ElementType type = ElementType::button;
  std::uint8_t bind = 0;  // button: a pbutton::* bit; stick: 0 left pair (LX, LY), 1 right pair (RX, RY); dpad: unused
  // Geometry in screen fractions, exactly as the contract defines them: x and y are the centre, width and height are
  // fractions of the screen *height* (so a square element stays square in pixels on any phone), and x/y use the
  // screen's width for x.
  float x = 0.0f;
  float y = 0.0f;
  float width = 0.1f;
  float height = 0.1f;
  float opacity = 0.6f;   // the global opacity unless the element overrides it
  float deadzone = 0.08f; // sticks
  bool visible = true;
};

struct Layout {
  int version = 1;
  std::string name;
  float opacity = 0.6f;
  bool haptics = true;
  float deadzone = 0.08f;
  std::vector<LayoutElement> elements;
};

// A layout file that cannot be used at all (not JSON, not an object, another version, no usable element). The message
// says what and where, so the log line names the line and column of a syntax error.
class LayoutError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

struct ParseResult {
  Layout layout;
  // Lines worth logging for a file that parsed: a skipped element (unknown type or binds, bad geometry), a clamped
  // value. Empty for a clean file.
  std::vector<std::string> notes;
};

// Parses and validates one layout text (the contract's format, version 1). Unknown fields are ignored, an element with
// an unknown type or binds is skipped with a note, and an element's geometry is clamped into the screen. Throws
// LayoutError when the text cannot be used.
ParseResult parse_layout(std::string_view json);

// The layout the game uses: the player's file when it is readable and valid, else the built-in default. `player_text`
// is null when there is no player file at all. Every line the caller should log (which text was used and why, then the
// parse notes) comes in `notes`, in order.
struct LoadResult {
  Layout layout;
  bool from_player = false;
  std::vector<std::string> notes;
};
LoadResult load_layout(const std::string *player_text, std::string_view default_text);

// The contract's `binds` names, for the activity (drawing chooses the PlayStation symbol from the name) and for the
// log lines. ElementType::stick carries "left_stick" or "right_stick".
const char *bind_name(ElementType type, std::uint8_t bind);
// The element type's name for the log lines and the activity ("button", "dpad", "stick").
const char *type_name(ElementType type);

// One stick reading: the thumb's offset from where it landed, in fractions of the stick's radius, with the screen's
// sign (down is positive). The vector is clamped to the unit circle, the dead zone is skipped and the travel from it
// to the edge is scaled to the full 0..127 deflection. 128 is rest, and the PS2 Y axis counts up (so a thumb above the
// landing point reads above 128).
struct StickBytes {
  std::uint8_t x = 128;
  std::uint8_t y = 128;
};
StickBytes stick_bytes(float dx, float dy, float deadzone);

// The d-pad directions a touch position selects, from its offset to the element's centre in -1..1 (the same screen
// sign). A direction needs a third of the travel; a diagonal presses both. Zero in the middle.
std::uint16_t dpad_buttons(float dx, float dy);

// What the virtual pad adds to the pad state: the buttons at full pressure and the two sticks.
struct Contribution {
  std::uint16_t buttons = 0;
  std::array<std::uint8_t, 4> axes{128, 128, 128, 128}; // RX, RY, LX, LY, as set_pad_axis takes them
  std::array<std::uint8_t, 16> pressures{};
};

// The physical pad with the virtual one combined: buttons OR together, a pressure is the higher of the two, and each
// stick axis takes the value further from rest. The game never sees which of the two produced a value.
pad::PadState merge(const pad::PadState &physical, const Contribution &virtual_pad);

// The live virtual pad: the layout, which element each touch holds and the state it produces. Every method may be
// called from any thread (the activity's touch events and the game's VBlank); the lock is held for the work only.
class Pad {
public:
  // Replaces the layout and clears every held element. `notes` are the lines of the load (kept for the log).
  void set_layout(Layout layout, std::vector<std::string> notes = {});
  Layout layout() const;
  bool haptics() const;

  // A touch on `element` (its index in layout().elements): down starts it, down again moves it, false releases it.
  // dx and dy are in -1..1 with the screen's sign: a stick reads them relative to where the thumb landed, a d-pad
  // relative to its centre; buttons ignore them. Ignores an index out of range and an invisible element.
  void touch(std::size_t element, float dx, float dy, bool down);
  void release_all();
  // `off` (GT4_VIRTUAL_PAD=off) and a hidden pad (auto, with a controller connected) contribute nothing.
  void set_active(bool active);
  bool active() const;
  Contribution contribution() const;

  // The lines the game thread prints for the `[vpad]` block: element presses, releases and stick moves, plus the
  // layout load's notes and the visibility changes. Empty most of the time.
  std::vector<std::string> take_log();
  // Adds one line to the next log (the activity's visibility changes, which only it can explain).
  void note(std::string line);

  // The drawable elements for the activity: 3 floats (element count, global opacity, haptics) then, per element,
  // type (0 button, 1 dpad, 2 stick), bind, x, y, width, height, opacity and visible (1/0). Invisible elements are
  // included with visible 0 so the indices stay the layout's.
  std::vector<float> geometry() const;

private:
  struct Held {
    bool pressed = false;
    float dx = 0.0f;
    float dy = 0.0f;
  };

  void log_change(std::size_t element, const Held &before, const Held &after);

  mutable std::mutex mutex_;
  Layout layout_;
  std::vector<Held> held_;
  bool active_ = false;
  std::vector<std::string> log_;
};

} // namespace gt4::vpad
