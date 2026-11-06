// CTest fixture for the virtual pad (virtual_pad.cpp, TASK-185): the layout file's rules (the contract shared with the
// launcher's editor), the stick and d-pad maths, and the merge with the physical pad. Pure functions only: the Android
// activity draws and forwards touches, and this is the part that decides what the game reads.

#include "virtual_pad.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace gt4::vpad;

namespace {

void require(bool value, const std::string &label) {
  if (!value)
    throw std::runtime_error(label);
}

void require_equal(long long actual, long long expected, const std::string &label) {
  if (actual != expected)
    throw std::runtime_error(label + ": expected " + std::to_string(expected) + ", got " + std::to_string(actual));
}

std::string read_file(const std::string &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    throw std::runtime_error("cannot read " + path);
  std::ostringstream text;
  text << input.rdbuf();
  return text.str();
}

const LayoutElement &element(const Layout &layout, std::size_t index) {
  if (index >= layout.elements.size())
    throw std::runtime_error("layout element " + std::to_string(index) + " is missing");
  return layout.elements[index];
}

const char *kDefaultLayout =
    GT4_VIRTUAL_PAD_DEFAULT_LAYOUT; // android/virtual-pad/default-layout.json, the file the APK ships

// The shipped file is the contract: 12 elements in the documented order, with the documented binds and geometry.
void check_shipped_default() {
  const std::string text = read_file(kDefaultLayout);
  const ParseResult parsed = parse_layout(text);
  const Layout &layout = parsed.layout;
  require(parsed.notes.empty(), "the shipped default layout parses without a note");
  require_equal(layout.version, 1, "version");
  require(layout.name == "GT4 default", "name");
  require(std::fabs(layout.opacity - 0.6f) < 1e-6, "global opacity 0.6");
  require(layout.haptics, "haptics on");
  require_equal(static_cast<long long>(layout.elements.size()), 12, "element count");

  const char *ids[] = {"lstick", "dpad", "cross", "square", "circle", "triangle", "l1", "r1", "l2", "r2", "select", "start"};
  for (std::size_t index = 0; index < 12; ++index)
    require(element(layout, index).id == ids[index], "element ids keep the contract's order at " + std::to_string(index));

  const LayoutElement &stick = element(layout, 0);
  require(stick.type == ElementType::stick && stick.bind == 0, "lstick is the left stick");
  require(std::fabs(stick.x - 0.13f) < 1e-6 && std::fabs(stick.y - 0.70f) < 1e-6, "lstick position");
  require(std::fabs(stick.height - 0.32f) < 1e-6 && std::fabs(stick.width - 0.32f) < 1e-6, "a stick without width is square");

  const LayoutElement &dpad = element(layout, 1);
  require(dpad.type == ElementType::dpad && dpad.id == "dpad", "dpad is the d-pad");

  const LayoutElement &cross = element(layout, 2);
  require(cross.type == ElementType::button && cross.bind == pbutton::cross, "cross binds the Cross bit");
  require(std::fabs(cross.opacity - 0.6f) < 1e-6, "an element without an opacity takes the global one");

  const LayoutElement &l1 = element(layout, 6);
  require(l1.type == ElementType::button && l1.bind == pbutton::l1, "l1 binds L1");
  require(std::fabs(l1.width - 0.16f) < 1e-6 && std::fabs(l1.height - 0.12f) < 1e-6, "l1 is a wide, short button");
  require(std::fabs(element(layout, 7).width - 0.16f) < 1e-6, "r1 keeps its width");
  require(element(layout, 10).id == "select" && element(layout, 11).id == "start", "select and start");
  std::fprintf(stderr, "PASS: the shipped default layout (12 elements, contract order and binds)\n");
}

void check_rejected_files() {
  const auto rejects = [](const std::string &text, const std::string &label) {
    try {
      (void)parse_layout(text);
    } catch (const LayoutError &) {
      return;
    }
    throw std::runtime_error(label + ": the parse should have failed");
  };
  rejects("{", "a truncated object");
  rejects("{\"version\": 1, \"elements\": [}", "a syntax error inside the array");
  rejects("[1, 2]", "an array instead of an object");
  rejects("{\"version\": 2, \"elements\": []}", "an unsupported version");
  rejects("{\"version\": 1}", "no elements array");
  rejects("{\"version\": 1, \"elements\": []}", "an empty elements array");
  rejects("{\"version\": 1, \"elements\": [{\"type\": \"button\", \"binds\": \"turbo\", \"x\": 0.5, \"y\": 0.5, \"size\": 0.1}]}",
          "one element with unknown binds leaves nothing usable");
  // A syntax error names its position: "line 2, column 5: ...".
  try {
    (void)parse_layout("{\n  \"version\": 1,\n    oops\n}");
    throw std::runtime_error("a syntax error should have failed");
  } catch (const LayoutError &error) {
    require(std::string(error.what()).find("line 3") == 0, "the syntax error names its line");
  }
  std::fprintf(stderr, "PASS: unusable layout files are refused with a message (syntax, version, elements)\n");
}

void check_skipped_elements_and_clamping() {
  const std::string text = R"({
    "version": 1,
    "name": "test",
    "opacity": 0.5,
    "elements": [
      {"id": "a", "type": "button", "binds": "cross", "x": 0.5, "y": 0.5, "size": 0.1},
      {"id": "b", "type": "button", "binds": "turbo", "x": 0.5, "y": 0.5, "size": 0.1},
      {"id": "c", "type": "wheel", "binds": "circle", "x": 0.5, "y": 0.5, "size": 0.1},
      {"id": "d", "type": "button", "binds": "left_stick", "x": 0.5, "y": 0.5, "size": 0.1},
      {"id": "e", "type": "button", "binds": "square", "x": 1.5, "y": 0.5, "size": 0.1},
      {"id": "f", "type": "button", "binds": "triangle", "y": 0.5, "size": 0.1},
      {"id": "g", "type": "button", "binds": "l1", "x": 0.2, "y": 0.2, "size": 0.1, "opacity": 0.25, "visible": false},
      {"id": "h", "type": "stick", "binds": "right_stick", "x": 0.8, "y": 0.7, "size": 0.3, "width": 0.2, "deadzone": 0.2}
    ]
  })";
  const ParseResult parsed = parse_layout(text);
  const Layout &layout = parsed.layout;
  require_equal(static_cast<long long>(layout.elements.size()), 4, "four usable elements");
  require(element(layout, 0).id == "a", "a is kept");
  require(element(layout, 1).id == "e", "e is clamped, not dropped");
  require(std::fabs(element(layout, 1).x - 1.0f) < 1e-6, "x is clamped to the screen edge");
  require(element(layout, 2).id == "g" && !element(layout, 2).visible, "visible false is kept for the activity");
  require(std::fabs(element(layout, 2).opacity - 0.25f) < 1e-6, "the element opacity overrides the global one");
  const LayoutElement &stick = element(layout, 3);
  require(stick.type == ElementType::stick && stick.bind == 1, "right_stick is the right pair");
  require(std::fabs(stick.deadzone - 0.2f) < 1e-6, "the element deadzone overrides the global one");
  require(std::fabs(stick.width - 0.2f) < 1e-6, "a stick keeps its own width");

  const auto note_says = [&parsed](const std::string &needle) {
    for (const std::string &note : parsed.notes) {
      if (note.find(needle) != std::string::npos)
        return true;
    }
    return false;
  };
  require(note_says("unknown binds \"turbo\""), "an unknown binds is logged");
  require(note_says("unknown type \"wheel\""), "an unknown type is logged");
  require(note_says("cannot carry binds \"left_stick\""), "a type/binds mismatch is logged");
  require(note_says("needs numeric x, y and size"), "a missing geometry is logged");
  require(note_says("x 1.5 clamped to 1"), "a clamped coordinate is logged");
  std::fprintf(stderr, "PASS: skipped elements (%lld kept of 8) and clamped values, each with its log line\n",
               static_cast<long long>(layout.elements.size()));
}

void check_load_fallback() {
  const std::string default_text = read_file(kDefaultLayout);
  {
    const LoadResult loaded = load_layout(nullptr, default_text);
    require(!loaded.from_player, "no player file uses the default");
    require(loaded.notes.size() >= 2 && loaded.notes[0].find("no player layout file") != std::string::npos,
            "the missing player file is logged");
  }
  {
    const std::string player = R"({"version": 1, "elements": [{"id": "only", "type": "button", "binds": "start",
                                  "x": 0.5, "y": 0.5, "size": 0.2}]})";
    const LoadResult loaded = load_layout(&player, default_text);
    require(loaded.from_player, "a valid player file is used");
    require_equal(static_cast<long long>(loaded.layout.elements.size()), 1, "the player layout's one element");
    require(loaded.notes[0].find("player layout: 1 elements") != std::string::npos, "the player layout is logged");
  }
  {
    const std::string broken = "{\"version\": 1, \"elements\": [{\"binds\": \"nope\"}]}";
    const LoadResult loaded = load_layout(&broken, default_text);
    require(!loaded.from_player, "a broken player file falls back to the default");
    require_equal(static_cast<long long>(loaded.layout.elements.size()), 12, "the default is complete");
    require(loaded.notes[0].find("not usable") != std::string::npos, "the fallback is logged with its reason");
  }
  std::fprintf(stderr, "PASS: the player file, the default, and the fallback with its reason\n");
}

void check_stick_maths() {
  const StickBytes rest = stick_bytes(0.0f, 0.0f, 0.08f);
  require(rest.x == 128 && rest.y == 128, "the centre is rest");
  const StickBytes inside = stick_bytes(0.04f, 0.0f, 0.08f);
  require(inside.x == 128 && inside.y == 128, "a touch inside the dead zone is rest");
  const StickBytes edge_of_zone = stick_bytes(0.08f, 0.0f, 0.08f);
  require(edge_of_zone.x == 128 && edge_of_zone.y == 128, "the dead zone's edge is rest");
  const StickBytes right = stick_bytes(1.0f, 0.0f, 0.08f);
  require(right.x == 255 && right.y == 128, "full right is 255 and keeps the other axis at rest");
  const StickBytes up = stick_bytes(0.0f, -1.0f, 0.08f);
  require(up.x == 128 && up.y == 255, "a thumb above the landing point reads above 128");
  const StickBytes half = stick_bytes(0.54f, 0.0f, 0.08f);
  require(half.x == 192 && half.y == 128, "past the dead zone the travel scales to 0..127");
  const StickBytes corner = stick_bytes(1.0f, 1.0f, 0.08f);
  require(corner.x == 218 && corner.y == 38, "the range is circular: a corner keeps the direction at full travel");
  const StickBytes no_zone = stick_bytes(0.5f, 0.0f, 0.0f);
  require(no_zone.x == 192, "a zero dead zone scales the whole travel");
  // A dead zone past 0.9 is clamped to 0.9, so 0.95 of the travel reads half deflection and not rest.
  const StickBytes clamped_deadzone = stick_bytes(0.95f, 0.0f, 0.95f);
  require(clamped_deadzone.x == 192, "a dead zone past 0.9 is clamped to 0.9");
  const StickBytes not_finite = stick_bytes(std::nanf(""), 0.0f, 0.08f);
  require(not_finite.x == 128 && not_finite.y == 128, "a not-a-number reading is rest");
  std::fprintf(stderr, "PASS: stick maths (dead zone, range, circular clamp, Y direction, recentre)\n");
}

void check_dpad_maths() {
  require(dpad_buttons(0.0f, 0.0f) == 0, "the centre presses nothing");
  require(dpad_buttons(0.2f, 0.2f) == 0, "a touch in the middle is not a direction");
  require(dpad_buttons(0.5f, 0.0f) == (1u << pbutton::right), "right of the centre presses right");
  require(dpad_buttons(-0.5f, -0.5f) == ((1u << pbutton::left) | (1u << pbutton::up)), "a diagonal presses both");
  require(dpad_buttons(0.0f, 0.9f) == (1u << pbutton::down), "below the centre presses down");
  std::fprintf(stderr, "PASS: d-pad directions (threshold, diagonals)\n");
}

void check_merge() {
  gt4::pad::PadState physical;
  physical.buttons = static_cast<std::uint16_t>(1u << pbutton::l1);
  physical.pressures[pbutton::l1] = 200;
  physical.axes = {128, 128, 200, 90};

  Contribution virtual_pad;
  virtual_pad.buttons = static_cast<std::uint16_t>((1u << pbutton::cross) | (1u << pbutton::l1));
  virtual_pad.pressures[pbutton::l1] = 160;
  virtual_pad.pressures[pbutton::cross] = 255;
  virtual_pad.axes = {128, 60, 100, 200}; // RX, RY, LX, LY

  const gt4::pad::PadState merged = merge(physical, virtual_pad);
  require(merged.buttons == ((1u << pbutton::l1) | (1u << pbutton::cross)), "buttons combine (OR)");
  require(merged.pressures[pbutton::l1] == 200, "the higher pressure wins");
  require(merged.pressures[pbutton::cross] == 255, "the virtual button presses at full pressure");
  require(merged.axes[0] == 128 && merged.axes[1] == 60, "a virtual axis away from rest reaches the pad");
  require(merged.axes[2] == 200, "the physical stick wins when it is further from rest");
  require(merged.axes[3] == 200, "the virtual stick wins on the other axis");

  const gt4::pad::PadState neutral = merge(physical, Contribution{});
  require(neutral.buttons == physical.buttons && neutral.axes[2] == 200, "an idle virtual pad changes nothing");
  std::fprintf(stderr, "PASS: merge with the physical pad (buttons OR, pressure max, axis furthest from rest)\n");
}

void check_live_pad() {
  Pad pad;
  const std::string default_text = read_file(kDefaultLayout);
  const LoadResult loaded = load_layout(nullptr, default_text);
  pad.set_layout(loaded.layout, loaded.notes);
  require(!pad.active(), "a new pad is inactive until the activity shows it");
  pad.touch(2, 0.0f, 0.0f, true);
  require(pad.contribution().buttons == 0, "an inactive pad contributes nothing");

  pad.set_active(true);
  require(pad.active(), "the pad reports its active state");
  require(pad.haptics(), "haptics follow the layout");
  require(pad.take_log().size() >= 1, "the layout load left its lines for the log");

  // index 2 is cross in the contract's layout
  pad.touch(2, 0.0f, 0.0f, true);
  Contribution pressed = pad.contribution();
  require(pressed.buttons == (1u << pbutton::cross), "Cross reaches the pad state");
  require(pressed.pressures[pbutton::cross] == 255, "a digital button presses at full pressure");
  require(pressed.axes[2] == 128 && pressed.axes[3] == 128, "a button leaves the sticks at rest");
  pad.touch(2, 0.0f, 0.0f, false);
  require(pad.contribution().buttons == 0, "the release lets the button go");

  // index 0 is the left stick
  pad.touch(0, 1.0f, 0.0f, true);
  pressed = pad.contribution();
  require(pressed.axes[2] == 255 && pressed.axes[3] == 128, "the left stick feeds LX and LY (bytes 2 and 3)");
  pad.touch(0, 0.0f, -1.0f, true);
  pressed = pad.contribution();
  require(pressed.axes[3] == 255, "the same touch moved to the top reads forward");
  pad.touch(0, 0.0f, 0.0f, false);
  require(pad.contribution().axes[2] == 128 && pad.contribution().axes[3] == 128, "the stick recentres on release");

  // index 1 is the d-pad
  pad.touch(1, -0.6f, -0.6f, true);
  pressed = pad.contribution();
  require(pressed.buttons == ((1u << pbutton::left) | (1u << pbutton::up)), "the d-pad presses its directions");
  require(pressed.pressures[pbutton::up] == 255, "a d-pad direction presses at full pressure");
  pad.release_all();
  require(pad.contribution().buttons == 0, "release_all lets everything go");

  // A hidden pad releases what it held: no button the player cannot release.
  pad.touch(2, 0.0f, 0.0f, true);
  pad.set_active(false);
  require(pad.contribution().buttons == 0, "hiding the pad stops its input");

  // Invisible elements and out-of-range indices do nothing.
  Layout with_hidden = loaded.layout;
  with_hidden.elements[2].visible = false;
  Pad hidden;
  hidden.set_layout(with_hidden);
  hidden.set_active(true);
  hidden.touch(2, 0.0f, 0.0f, true);
  require(hidden.contribution().buttons == 0, "an invisible element takes no input");
  hidden.touch(99, 0.0f, 0.0f, true);
  require(hidden.contribution().buttons == 0, "an index past the layout is ignored");

  // The lines the game thread prints: a press, a release and the stick's values.
  const std::vector<std::string> lines = pad.take_log();
  const auto saw = [&lines](const std::string &needle) {
    for (const std::string &line : lines) {
      if (line.find(needle) != std::string::npos)
        return true;
    }
    return false;
  };
  require(saw("cross down") && saw("cross up"), "the press and release are logged");
  require(saw("lstick x=255 y=128"), "the stick's values are logged");
  require(saw("lstick centred"), "the stick's recentre is logged");
  require(pad.take_log().empty(), "taking the log empties it");

  // The geometry the activity draws: 3 header floats then 8 per element.
  const std::vector<float> geometry = pad.geometry();
  require_equal(static_cast<long long>(geometry.size()), 3 + 12 * 8, "geometry size");
  require_equal(static_cast<long long>(geometry[0]), 12, "geometry element count");
  require(std::fabs(geometry[1] - 0.6f) < 1e-6, "geometry global opacity");
  require(geometry[2] == 1.0f, "geometry haptics flag");
  require_equal(static_cast<long long>(geometry[3]), 2, "element 0 is a stick");
  require_equal(static_cast<long long>(geometry[3 + 8 * 2]), 0, "element 2 is a button");
  require_equal(static_cast<long long>(geometry[3 + 8 * 2 + 1]), pbutton::cross, "element 2 binds Cross");
  std::fprintf(stderr, "PASS: the live pad (press, stick, d-pad, hide, invisible, log lines, geometry)\n");
}

} // namespace

int main() {
  try {
    check_shipped_default();
    check_rejected_files();
    check_skipped_elements_and_clamping();
    check_load_fallback();
    check_stick_maths();
    check_dpad_maths();
    check_merge();
    check_live_pad();
    std::fprintf(stderr, "PASS: virtual pad fixture (all checks)\n");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
