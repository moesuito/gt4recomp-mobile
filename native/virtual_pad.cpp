// SPDX-License-Identifier: MIT
// TASK-185: the virtual pad's layout file, its validation, the stick and d-pad maths, and the merge with the physical
// pad. Pure C++ on purpose: virtual_pad_fixture (native/CMakeLists.txt) checks every rule on the build machine, and the
// Android activity only draws and forwards touches.
#include "virtual_pad.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>

namespace gt4::vpad {
namespace {

// ---------------------------------------------------------------- JSON (the strict subset the layout uses)

struct Json {
  enum class Kind { null_value, boolean, number, string, array, object } kind = Kind::null_value;
  bool boolean = false;
  double number = 0.0;
  std::string text;
  std::vector<Json> items;
  std::vector<std::pair<std::string, Json>> members;

  const Json *find(std::string_view name) const {
    if (kind != Kind::object)
      return nullptr;
    for (const auto &member : members) {
      if (member.first == name)
        return &member.second;
    }
    return nullptr;
  }
};

class JsonReader {
public:
  explicit JsonReader(std::string_view text) : text_(text) {}

  Json document() {
    Json value = value_at(0);
    skip_space();
    if (position_ != text_.size())
      fail("unexpected text after the layout object");
    return value;
  }

private:
  static constexpr int max_depth = 16;

  [[noreturn]] void fail(const std::string &message) const {
    // "line 3, column 17: <message>": the editor's file is small, and a plain position is what fixes a typo.
    std::size_t line = 1, column = 1;
    for (std::size_t index = 0; index < position_ && index < text_.size(); ++index) {
      if (text_[index] == '\n') {
        ++line;
        column = 1;
      } else {
        ++column;
      }
    }
    throw LayoutError("line " + std::to_string(line) + ", column " + std::to_string(column) + ": " + message);
  }

  void skip_space() {
    while (position_ < text_.size()) {
      const char character = text_[position_];
      if (character == ' ' || character == '\t' || character == '\n' || character == '\r')
        ++position_;
      else
        break;
    }
  }

  bool consume(char expected) {
    skip_space();
    if (position_ < text_.size() && text_[position_] == expected) {
      ++position_;
      return true;
    }
    return false;
  }

  char peek() {
    skip_space();
    if (position_ >= text_.size())
      fail("the layout ends in the middle of a value");
    return text_[position_];
  }

  Json value_at(int depth) {
    if (depth > max_depth)
      fail("the layout nests too deeply");
    switch (peek()) {
      case '{': return object_at(depth);
      case '[': return array_at(depth);
      case '"': {
        Json out;
        out.kind = Json::Kind::string;
        out.text = string_at();
        return out;
      }
      case 't':
      case 'f': {
        Json out;
        out.kind = Json::Kind::boolean;
        if (text_.compare(position_, 4, "true") == 0) {
          out.boolean = true;
          position_ += 4;
        } else if (text_.compare(position_, 5, "false") == 0) {
          out.boolean = false;
          position_ += 5;
        } else {
          fail("expected true or false");
        }
        return out;
      }
      case 'n': {
        if (text_.compare(position_, 4, "null") != 0)
          fail("expected null");
        position_ += 4;
        return Json{};
      }
      default: {
        Json out;
        out.kind = Json::Kind::number;
        out.number = number_at();
        return out;
      }
    }
  }

  Json object_at(int depth) {
    Json out;
    out.kind = Json::Kind::object;
    if (!consume('{'))
      fail("expected {");
    if (consume('}'))
      return out;
    for (;;) {
      skip_space();
      if (position_ >= text_.size() || text_[position_] != '"')
        fail("expected a member name in double quotes");
      std::string name = string_at();
      if (!consume(':'))
        fail("expected : after a member name");
      out.members.emplace_back(std::move(name), value_at(depth + 1));
      if (consume(','))
        continue;
      if (consume('}'))
        return out;
      fail("expected , or } in an object");
    }
  }

  Json array_at(int depth) {
    Json out;
    out.kind = Json::Kind::array;
    if (!consume('['))
      fail("expected [");
    if (consume(']'))
      return out;
    for (;;) {
      out.items.push_back(value_at(depth + 1));
      if (consume(','))
        continue;
      if (consume(']'))
        return out;
      fail("expected , or ] in an array");
    }
  }

  std::string string_at() {
    skip_space();
    if (position_ >= text_.size() || text_[position_] != '"')
      fail("expected a string");
    ++position_;
    std::string out;
    while (position_ < text_.size()) {
      const unsigned char character = static_cast<unsigned char>(text_[position_++]);
      if (character == '"')
        return out;
      if (character < 0x20)
        fail("a control character inside a string");
      if (character != '\\') {
        out.push_back(static_cast<char>(character));
        continue;
      }
      if (position_ >= text_.size())
        fail("the layout ends inside an escape");
      const char escape = text_[position_++];
      switch (escape) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          unsigned int code = 0;
          if (!hex4(code))
            fail("a \\u escape needs four hex digits");
          if (code >= 0xD800 && code <= 0xDBFF) {
            // A surrogate pair: the low half must follow.
            if (position_ + 1 < text_.size() && text_[position_] == '\\' && text_[position_ + 1] == 'u') {
              position_ += 2;
              unsigned int low = 0;
              if (!hex4(low) || low < 0xDC00 || low > 0xDFFF)
                fail("a high surrogate needs its low half");
              code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
            } else {
              fail("a high surrogate needs its low half");
            }
          } else if (code >= 0xDC00 && code <= 0xDFFF) {
            fail("a low surrogate without its high half");
          }
          append_utf8(out, code);
          break;
        }
        default: fail("unknown escape in a string");
      }
    }
    fail("the layout ends inside a string");
  }

  bool hex4(unsigned int &out) {
    out = 0;
    for (int digit = 0; digit < 4; ++digit) {
      if (position_ >= text_.size())
        return false;
      const char character = text_[position_];
      unsigned int value = 0;
      if (character >= '0' && character <= '9')
        value = static_cast<unsigned int>(character - '0');
      else if (character >= 'a' && character <= 'f')
        value = static_cast<unsigned int>(character - 'a' + 10);
      else if (character >= 'A' && character <= 'F')
        value = static_cast<unsigned int>(character - 'A' + 10);
      else
        return false;
      out = (out << 4) | value;
      ++position_;
    }
    return true;
  }

  static void append_utf8(std::string &out, unsigned int code) {
    if (code < 0x80) {
      out.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (code >> 6)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (code >> 12)));
      out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (code >> 18)));
      out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
  }

  double number_at() {
    skip_space();
    const std::size_t start = position_;
    if (position_ < text_.size() && text_[position_] == '-')
      ++position_;
    if (position_ >= text_.size() || !is_digit(text_[position_]))
      fail("expected a number");
    if (text_[position_] == '0') {
      ++position_;
    } else {
      while (position_ < text_.size() && is_digit(text_[position_])) ++position_;
    }
    if (position_ < text_.size() && text_[position_] == '.') {
      ++position_;
      if (position_ >= text_.size() || !is_digit(text_[position_]))
        fail("expected digits after the decimal point");
      while (position_ < text_.size() && is_digit(text_[position_])) ++position_;
    }
    if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E')) {
      ++position_;
      if (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-'))
        ++position_;
      if (position_ >= text_.size() || !is_digit(text_[position_]))
        fail("expected digits in the exponent");
      while (position_ < text_.size() && is_digit(text_[position_])) ++position_;
    }
    const std::string digits(text_.substr(start, position_ - start));
    return std::strtod(digits.c_str(), nullptr);
  }

  static bool is_digit(char character) { return character >= '0' && character <= '9'; }

  std::string_view text_;
  std::size_t position_ = 0;
};

// ---------------------------------------------------------------- the layout's fields

std::string quote(const std::string &text) { return "\"" + text + "\""; }

const Json *member(const Json &object, const char *name) { return object.find(name); }

double number_or(const Json &object, const char *name, double fallback, const char *where) {
  const Json *value = member(object, name);
  if (value == nullptr)
    return fallback;
  if (value->kind != Json::Kind::number)
    throw LayoutError(std::string(where) + ": " + name + " is not a number");
  return value->number;
}

bool boolean_or(const Json &object, const char *name, bool fallback, const char *where) {
  const Json *value = member(object, name);
  if (value == nullptr)
    return fallback;
  if (value->kind != Json::Kind::boolean)
    throw LayoutError(std::string(where) + ": " + name + " is not true or false");
  return value->boolean;
}

std::string string_or(const Json &object, const char *name, const std::string &fallback, const char *where) {
  const Json *value = member(object, name);
  if (value == nullptr)
    return fallback;
  if (value->kind != Json::Kind::string)
    throw LayoutError(std::string(where) + ": " + name + " is not a string");
  return value->text;
}

struct BindName {
  const char *name;
  ElementType type;
  std::uint8_t bind;
};

// The contract's `binds` values.
constexpr BindName bind_names[] = {
    {"cross", ElementType::button, pbutton::cross},   {"circle", ElementType::button, pbutton::circle},
    {"square", ElementType::button, pbutton::square}, {"triangle", ElementType::button, pbutton::triangle},
    {"l1", ElementType::button, pbutton::l1},         {"r1", ElementType::button, pbutton::r1},
    {"l2", ElementType::button, pbutton::l2},         {"r2", ElementType::button, pbutton::r2},
    {"l3", ElementType::button, pbutton::l3},         {"r3", ElementType::button, pbutton::r3},
    {"select", ElementType::button, pbutton::select}, {"start", ElementType::button, pbutton::start},
    {"dpad", ElementType::dpad, 0},                   {"left_stick", ElementType::stick, 0},
    {"right_stick", ElementType::stick, 1},
};

const BindName *find_bind(const std::string &name) {
  for (const BindName &candidate : bind_names) {
    if (name == candidate.name)
      return &candidate;
  }
  return nullptr;
}

float clamped(float value, float low, float high) { return std::min(high, std::max(low, value)); }

std::string format_float(float value) {
  char text[32];
  std::snprintf(text, sizeof text, "%.3f", static_cast<double>(value));
  // "0.600" reads better as "0.6": the trailing zeros of the print precision go.
  std::string out(text);
  if (out.find('.') != std::string::npos) {
    while (!out.empty() && out.back() == '0') out.pop_back();
    if (!out.empty() && out.back() == '.') out.pop_back();
  }
  return out;
}

// One element of the layout. std::nullopt when it has to be skipped, with a note saying why: one wrong element never
// takes the player's whole layout away.
std::optional<LayoutElement> parse_element(const Json &entry, std::size_t index, float layout_opacity,
                                           float layout_deadzone, std::vector<std::string> &notes) {
  std::string label = "element[" + std::to_string(index) + "]";
  try {
    const std::string where = "layout " + label;
    const std::string id = string_or(entry, "id", "", where.c_str());
    if (!id.empty())
      label += " (" + quote(id) + ")";
    const std::string type_text = string_or(entry, "type", "", where.c_str());
    const std::string binds_text = string_or(entry, "binds", "", where.c_str());
    const BindName *bind = find_bind(binds_text);
    if (bind == nullptr) {
      notes.push_back("layout " + label + ": unknown binds " + quote(binds_text) + ", skipped");
      return std::nullopt;
    }
    ElementType type = ElementType::button;
    if (type_text == "button")
      type = ElementType::button;
    else if (type_text == "dpad")
      type = ElementType::dpad;
    else if (type_text == "stick")
      type = ElementType::stick;
    else {
      notes.push_back("layout " + label + ": unknown type " + quote(type_text) + ", skipped");
      return std::nullopt;
    }
    if (type != bind->type) {
      notes.push_back("layout " + label + ": type " + quote(type_text) + " cannot carry binds " + quote(binds_text) +
                      ", skipped");
      return std::nullopt;
    }
    const Json *x = member(entry, "x");
    const Json *y = member(entry, "y");
    const Json *size = member(entry, "size");
    if (x == nullptr || y == nullptr || size == nullptr || x->kind != Json::Kind::number ||
        y->kind != Json::Kind::number || size->kind != Json::Kind::number) {
      notes.push_back("layout " + label + ": needs numeric x, y and size, skipped");
      return std::nullopt;
    }
    if (!std::isfinite(x->number) || !std::isfinite(y->number) || !std::isfinite(size->number) || size->number <= 0.0) {
      notes.push_back("layout " + label + ": x, y and size must be finite and size above zero, skipped");
      return std::nullopt;
    }
    LayoutElement element;
    element.id = id.empty() ? std::string(bind_name(type, bind->bind)) : id;
    element.type = type;
    element.bind = bind->bind;
    // A coordinate outside the screen is pulled back to its edge (the editor warns in the same case).
    const auto place = [&](const Json &value, const char *axis, float &out) {
      const float raw = static_cast<float>(value.number);
      out = clamped(raw, 0.0f, 1.0f);
      if (out != raw)
        notes.push_back("layout " + label + ": " + axis + " " + format_float(raw) + " clamped to " + format_float(out));
    };
    place(*x, "x", element.x);
    place(*y, "y", element.y);
    element.height = clamped(static_cast<float>(size->number), 0.01f, 1.0f);
    if (static_cast<float>(size->number) > 1.0f)
      notes.push_back("layout " + label + ": size " + format_float(static_cast<float>(size->number)) + " clamped to 1");
    // `width` uses the same base as `size` (a fraction of the screen height), so an element without it is square.
    const double width = number_or(entry, "width", size->number, where.c_str());
    if (!std::isfinite(width) || width <= 0.0) {
      notes.push_back("layout " + label + ": width must be above zero, the element is square");
      element.width = element.height;
    } else {
      element.width = clamped(static_cast<float>(width), 0.01f, 1.0f);
    }
    const double element_opacity = number_or(entry, "opacity", layout_opacity, where.c_str());
    if (element_opacity < 0.0 || element_opacity > 1.0)
      notes.push_back("layout " + label + ": opacity " + format_float(static_cast<float>(element_opacity)) +
                      " is outside 0..1, clamped");
    element.opacity = clamped(static_cast<float>(element_opacity), 0.0f, 1.0f);
    const double element_deadzone = number_or(entry, "deadzone", layout_deadzone, where.c_str());
    element.deadzone = clamped(static_cast<float>(element_deadzone), 0.0f, 0.9f);
    element.visible = boolean_or(entry, "visible", true, where.c_str());
    return element;
  } catch (const LayoutError &error) {
    notes.push_back("layout " + label + ": " + error.what() + ", skipped");
    return std::nullopt;
  }
}

} // namespace

const char *type_name(ElementType type) {
  switch (type) {
    case ElementType::button: return "button";
    case ElementType::dpad: return "dpad";
    case ElementType::stick: return "stick";
  }
  return "button";
}

const char *bind_name(ElementType type, std::uint8_t bind) {
  for (const BindName &candidate : bind_names) {
    if (candidate.type == type && candidate.bind == bind)
      return candidate.name;
  }
  return "unknown";
}

ParseResult parse_layout(std::string_view json) {
  const Json root = JsonReader(json).document();
  if (root.kind != Json::Kind::object)
    throw LayoutError("the layout is not a JSON object");
  ParseResult result;
  Layout &layout = result.layout;
  layout.version = static_cast<int>(number_or(root, "version", 1.0, "layout"));
  if (layout.version != 1)
    throw LayoutError("layout version " + std::to_string(layout.version) + " is not supported (this build reads version 1)");
  layout.name = string_or(root, "name", "", "layout");
  const double opacity = number_or(root, "opacity", 0.6, "layout");
  if (opacity < 0.0 || opacity > 1.0)
    result.notes.push_back("layout: opacity " + format_float(static_cast<float>(opacity)) + " is outside 0..1, clamped");
  layout.opacity = clamped(static_cast<float>(opacity), 0.0f, 1.0f);
  layout.haptics = boolean_or(root, "haptics", true, "layout");
  const double deadzone = number_or(root, "deadzone", 0.08, "layout");
  if (deadzone < 0.0 || deadzone >= 1.0)
    result.notes.push_back("layout: deadzone " + format_float(static_cast<float>(deadzone)) + " is outside 0..1, clamped");
  layout.deadzone = clamped(static_cast<float>(deadzone), 0.0f, 0.9f);

  const Json *elements = member(root, "elements");
  if (elements == nullptr || elements->kind != Json::Kind::array)
    throw LayoutError("the layout has no elements array");
  for (std::size_t index = 0; index < elements->items.size(); ++index) {
    const Json &entry = elements->items[index];
    if (entry.kind != Json::Kind::object) {
      result.notes.push_back("layout element[" + std::to_string(index) + "]: not an object, skipped");
      continue;
    }
    if (auto element = parse_element(entry, index, layout.opacity, layout.deadzone, result.notes))
      layout.elements.push_back(std::move(*element));
  }
  if (layout.elements.empty())
    throw LayoutError("the layout has no usable element (checked " + std::to_string(elements->items.size()) + ")");
  return result;
}

LoadResult load_layout(const std::string *player_text, std::string_view default_text) {
  LoadResult result;
  if (player_text != nullptr) {
    try {
      ParseResult parsed = parse_layout(*player_text);
      result.layout = std::move(parsed.layout);
      result.from_player = true;
      std::string line = "player layout: " + std::to_string(result.layout.elements.size()) + " elements";
      if (!result.layout.name.empty())
        line += ", " + quote(result.layout.name);
      result.notes.push_back(line);
      result.notes.insert(result.notes.end(), parsed.notes.begin(), parsed.notes.end());
      return result;
    } catch (const LayoutError &error) {
      result.notes.push_back(std::string("player layout is not usable (") + error.what() + "); the default is used");
    }
  } else {
    result.notes.push_back("no player layout file; the default is used");
  }
  ParseResult parsed = parse_layout(default_text);
  result.layout = std::move(parsed.layout);
  result.notes.push_back("default layout: " + std::to_string(result.layout.elements.size()) + " elements");
  result.notes.insert(result.notes.end(), parsed.notes.begin(), parsed.notes.end());
  return result;
}

StickBytes stick_bytes(float dx, float dy, float deadzone) {
  if (!std::isfinite(dx) || !std::isfinite(dy))
    return {};
  float length = std::sqrt(dx * dx + dy * dy);
  if (length <= 0.0f)
    return {};
  if (length > 1.0f) {
    // The circular range: past the edge the direction stays and the deflection does not.
    dx /= length;
    dy /= length;
    length = 1.0f;
  }
  const float zone = clamped(deadzone, 0.0f, 0.9f);
  const float rest_length = length;
  if (rest_length <= zone)
    return {};
  const float magnitude = (rest_length - zone) / (1.0f - zone);
  const float unit_x = dx / length;
  const float unit_y = dy / length;
  const auto byte = [](float value) {
    const long rounded = std::lround(value * 127.0f);
    return static_cast<std::uint8_t>(std::clamp<long>(128 + rounded, 0, 255));
  };
  // The screen's Y grows downwards and the PS2's analog Y byte grows upwards.
  return {byte(unit_x * magnitude), byte(-unit_y * magnitude)};
}

std::uint16_t dpad_buttons(float dx, float dy) {
  if (!std::isfinite(dx) || !std::isfinite(dy))
    return 0;
  const float x = clamped(dx, -1.0f, 1.0f);
  const float y = clamped(dy, -1.0f, 1.0f);
  constexpr float threshold = 1.0f / 3.0f;
  std::uint16_t buttons = 0;
  if (x <= -threshold)
    buttons |= static_cast<std::uint16_t>(1u << pbutton::left);
  if (x >= threshold)
    buttons |= static_cast<std::uint16_t>(1u << pbutton::right);
  if (y <= -threshold)
    buttons |= static_cast<std::uint16_t>(1u << pbutton::up);
  if (y >= threshold)
    buttons |= static_cast<std::uint16_t>(1u << pbutton::down);
  return buttons;
}

gt4::pad::PadState merge(const gt4::pad::PadState &physical, const Contribution &virtual_pad) {
  gt4::pad::PadState out = physical;
  out.buttons |= virtual_pad.buttons;
  for (std::size_t button = 0; button < out.pressures.size(); ++button)
    out.pressures[button] = std::max(out.pressures[button], virtual_pad.pressures[button]);
  for (std::size_t axis = 0; axis < out.axes.size(); ++axis) {
    const int current = std::abs(static_cast<int>(out.axes[axis]) - 128);
    const int candidate = std::abs(static_cast<int>(virtual_pad.axes[axis]) - 128);
    if (candidate > current)
      out.axes[axis] = virtual_pad.axes[axis];
  }
  return out;
}

// ---------------------------------------------------------------- the live pad

void Pad::set_layout(Layout layout, std::vector<std::string> notes) {
  const std::lock_guard<std::mutex> lock(mutex_);
  layout_ = std::move(layout);
  held_.assign(layout_.elements.size(), Held{});
  for (const std::string &note : notes) log_.push_back(note);
}

Layout Pad::layout() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return layout_;
}

bool Pad::haptics() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return layout_.haptics;
}

void Pad::touch(std::size_t element, float dx, float dy, bool down) {
  const std::lock_guard<std::mutex> lock(mutex_);
  if (element >= layout_.elements.size() || element >= held_.size())
    return;
  const LayoutElement &target = layout_.elements[element];
  if (!target.visible)
    return;
  Held before = held_[element];
  Held &after = held_[element];
  after.pressed = down;
  after.dx = down ? (std::isfinite(dx) ? clamped(dx, -1.0f, 1.0f) : 0.0f) : 0.0f;
  after.dy = down ? (std::isfinite(dy) ? clamped(dy, -1.0f, 1.0f) : 0.0f) : 0.0f;
  log_change(element, before, after);
}

void Pad::release_all() {
  const std::lock_guard<std::mutex> lock(mutex_);
  for (std::size_t element = 0; element < held_.size(); ++element) {
    if (!held_[element].pressed)
      continue;
    Held before = held_[element];
    held_[element] = Held{};
    log_change(element, before, held_[element]);
  }
}

void Pad::set_active(bool active) {
  const std::lock_guard<std::mutex> lock(mutex_);
  if (active_ == active)
    return;
  active_ = active;
  if (!active_) {
    // Hidden (or off): nothing may stay held, so the game never keeps a button the player cannot release.
    for (std::size_t element = 0; element < held_.size(); ++element) {
      if (!held_[element].pressed)
        continue;
      Held before = held_[element];
      held_[element] = Held{};
      log_change(element, before, held_[element]);
    }
  }
}

bool Pad::active() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return active_;
}

Contribution Pad::contribution() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  Contribution out;
  if (!active_)
    return out;
  for (std::size_t index = 0; index < layout_.elements.size() && index < held_.size(); ++index) {
    const LayoutElement &element = layout_.elements[index];
    const Held &held = held_[index];
    if (!element.visible || !held.pressed)
      continue;
    switch (element.type) {
      case ElementType::button:
        out.buttons |= static_cast<std::uint16_t>(1u << element.bind);
        out.pressures[element.bind] = 255; // version 1: a button is digital, full pressure while held
        break;
      case ElementType::dpad: {
        const std::uint16_t directions = dpad_buttons(held.dx, held.dy);
        out.buttons |= directions;
        for (std::uint8_t bit = pbutton::up; bit <= pbutton::left; ++bit) {
          if (directions & (1u << bit))
            out.pressures[bit] = 255;
        }
        break;
      }
      case ElementType::stick: {
        const StickBytes bytes = stick_bytes(held.dx, held.dy, element.deadzone);
        // PS2 analog byte order: RX, RY, LX, LY.
        const std::size_t first = element.bind == 0 ? 2 : 0;
        const int current_x = std::abs(static_cast<int>(out.axes[first]) - 128);
        const int candidate_x = std::abs(static_cast<int>(bytes.x) - 128);
        if (candidate_x > current_x)
          out.axes[first] = bytes.x;
        const int current_y = std::abs(static_cast<int>(out.axes[first + 1]) - 128);
        const int candidate_y = std::abs(static_cast<int>(bytes.y) - 128);
        if (candidate_y > current_y)
          out.axes[first + 1] = bytes.y;
        break;
      }
    }
  }
  return out;
}

std::vector<std::string> Pad::take_log() {
  const std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> out;
  out.swap(log_);
  return out;
}

void Pad::note(std::string line) {
  const std::lock_guard<std::mutex> lock(mutex_);
  log_.push_back(std::move(line));
}

std::vector<float> Pad::geometry() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  std::vector<float> out;
  out.reserve(3 + layout_.elements.size() * 8);
  out.push_back(static_cast<float>(layout_.elements.size()));
  out.push_back(layout_.opacity);
  out.push_back(layout_.haptics ? 1.0f : 0.0f);
  for (const LayoutElement &element : layout_.elements) {
    out.push_back(static_cast<float>(static_cast<int>(element.type)));
    out.push_back(static_cast<float>(element.bind));
    out.push_back(element.x);
    out.push_back(element.y);
    out.push_back(element.width);
    out.push_back(element.height);
    out.push_back(element.opacity);
    out.push_back(element.visible ? 1.0f : 0.0f);
  }
  return out;
}

void Pad::log_change(std::size_t element, const Held &before, const Held &after) {
  if (element >= layout_.elements.size())
    return;
  const LayoutElement &target = layout_.elements[element];
  const std::string name = target.id;  // always set by parse_layout (the binds name when the file has no id)
  if (log_.size() >= 512)
    log_.erase(log_.begin(), log_.begin() + 256); // never let a stream of moves grow without bound
  const auto direction_text = [](std::uint16_t directions) {
    std::string text;
    const auto add = [&text](const char *word) {
      if (!text.empty())
        text += "+";
      text += word;
    };
    if (directions & (1u << pbutton::up)) add("up");
    if (directions & (1u << pbutton::down)) add("down");
    if (directions & (1u << pbutton::left)) add("left");
    if (directions & (1u << pbutton::right)) add("right");
    return text.empty() ? std::string("rest") : text;
  };
  switch (target.type) {
    case ElementType::dpad:
      if (!after.pressed) {
        if (before.pressed)
          log_.push_back(name + " " + direction_text(dpad_buttons(before.dx, before.dy)) + " -> rest");
        return;
      }
      if (!before.pressed) {
        log_.push_back(name + " " + direction_text(dpad_buttons(after.dx, after.dy)));
        return;
      }
      if (dpad_buttons(before.dx, before.dy) != dpad_buttons(after.dx, after.dy))
        log_.push_back(name + " " + direction_text(dpad_buttons(after.dx, after.dy)));
      return;
    case ElementType::stick: {
      const StickBytes bytes = stick_bytes(after.dx, after.dy, target.deadzone);
      if (!after.pressed) {
        if (before.pressed)
          log_.push_back(name + " centred");
        return;
      }
      const StickBytes previous = stick_bytes(before.dx, before.dy, target.deadzone);
      const int moved = std::max(std::abs(static_cast<int>(bytes.x) - static_cast<int>(previous.x)),
                                 std::abs(static_cast<int>(bytes.y) - static_cast<int>(previous.y)));
      // A line per eighth of the travel: enough to prove the stick reaches the pad, few enough to read.
      if (!before.pressed || moved >= 8 || (previous.x == 128 && previous.y == 128 && (bytes.x != 128 || bytes.y != 128)))
        log_.push_back(name + " x=" + std::to_string(bytes.x) + " y=" + std::to_string(bytes.y));
      return;
    }
    case ElementType::button:
      if (after.pressed && !before.pressed)
        log_.push_back(name + " down");
      else if (!after.pressed && before.pressed)
        log_.push_back(name + " up");
      return;
  }
}

} // namespace gt4::vpad
