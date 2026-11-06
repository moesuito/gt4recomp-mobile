/*
 * Pure input rules: swipe direction, right-stick scroll speed, slider value
 * from a pointer position. No DOM access, so ui/dev/nav-test.html can test them.
 *
 * GT4.input.swipeStep(dx, dy)            -> +1 (next tab), -1 (previous tab), 0 (not a swipe)
 * GT4.input.stickScroll(value)           -> scroll distance in px for one frame (value -1..1)
 * GT4.input.valueFromPointer(x, left, width, min, max, step) -> stepped slider value
 * GT4.input.stepsFromActivation(detail)  -> true for a keyboard or assistive click, false for a pointer one
 */
(function (root) {
  "use strict";
  var GT4 = (root.GT4 = root.GT4 || {});

  var SWIPE_MIN_PX = 60;
  var SWIPE_RATIO = 1.5; // horizontal distance must exceed 1.5 times the vertical distance
  var STICK_DEADZONE = 0.2;
  var STICK_PX_PER_FRAME = 18; // full deflection at 60 Hz is about 1080 px per second

  // A finger that moves left (dx < 0) asks for the next tab, as in a carousel.
  function swipeStep(dx, dy) {
    var across = Math.abs(dx);
    var along = Math.abs(dy);
    if (across < SWIPE_MIN_PX || across <= SWIPE_RATIO * along) {
      return 0;
    }
    return dx < 0 ? 1 : -1;
  }

  function stickScroll(value) {
    var size = Math.abs(Number(value) || 0);
    if (size <= STICK_DEADZONE) {
      return 0;
    }
    var scaled = (size - STICK_DEADZONE) / (1 - STICK_DEADZONE);
    var sign = value < 0 ? -1 : 1;
    return sign * Math.min(1, scaled) * STICK_PX_PER_FRAME;
  }

  // The track runs from left to left + width; min and max are the slider bounds.
  function valueFromPointer(x, left, width, min, max, step) {
    var ratio = width > 0 ? (x - left) / width : 0;
    ratio = Math.min(1, Math.max(0, ratio));
    var raw = min + ratio * (max - min);
    var stepSize = step > 0 ? step : 1;
    var stepped = min + Math.round((raw - min) / stepSize) * stepSize;
    return Math.min(max, Math.max(min, stepped));
  }

  // A click made by the keyboard, by the pad or by assistive technology has no click count (the DOM's `detail` is 0: nav.js
  // activate() calls click() on the focused control); a mouse, finger or pen click carries its count (1, 2...). An option
  // row only steps on the first kind, so its label and value stop changing the value under a pointer (TASK-193).
  function stepsFromActivation(detail) {
    return !(Number(detail) > 0);
  }

  // Which action a face or shoulder button triggers. state = {capturing, dialogOpen, tipOpen, hooks: {x, y, start, tabs}}.
  // Returns one of: "cancel-capture", "activate", "close-tip", "dialog-back", "back", "hook-x", "hook-y",
  // "hook-start", "tab-prev", "tab-next", or null (nothing happens). While a rebind waits only B does anything.
  function padRoute(button, state) {
    var hooks = state.hooks || {};
    if (state.capturing) {
      return button === "b" ? "cancel-capture" : null;
    }
    switch (button) {
      case "a":
        return "activate";
      case "b":
        if (state.tipOpen) {
          return "close-tip";
        }
        return state.dialogOpen ? "dialog-back" : "back";
      case "x":
      case "y":
      case "start":
        if (state.dialogOpen || !hooks[button]) {
          return null;
        }
        return "hook-" + button;
      case "lb":
      case "rb":
        if (state.dialogOpen || !hooks.tabs) {
          return null;
        }
        return button === "lb" ? "tab-prev" : "tab-next";
      default:
        return null;
    }
  }

  GT4.input = {
    padRoute: padRoute,
    swipeStep: swipeStep,
    stickScroll: stickScroll,
    valueFromPointer: valueFromPointer,
    stepsFromActivation: stepsFromActivation,
    SWIPE_MIN_PX: SWIPE_MIN_PX,
    STICK_DEADZONE: STICK_DEADZONE
  };
})(window);
