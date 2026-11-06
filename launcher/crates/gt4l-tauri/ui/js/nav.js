/*
 * Input: spatial focus movement and the keyboard. Pages describe their actions through
 * GT4.hooks = {x, y, start, back, tabs(delta), horizontal(delta), vertical(delta)}; the router resets it per page.
 * horizontal and vertical return true when the page took the press (the virtual pad editor moves its element with the
 * arrows); a false return keeps the launcher's focus movement and slider stepping.
 *
 * The pad comes from the launcher's Rust side (SDL3, the layer the game uses): the `pad-nav` event carries the press,
 * and Rust does the D-pad and left-stick repeat (first press at once, then after 420 ms and every 110 ms while held).
 * The `pad-live` event carries the right stick, which scrolls the focused scroll container. No Web Gamepad polling.
 *
 * Pad mapping (SDL names as the page receives them):
 *   a (SOUTH) activate            b (EAST) back / cancel
 *   x (WEST) page action 1        y (NORTH) page action 2
 *   lb previous tab               rb next tab
 *   start: page start (Play on Home)
 *   up / down / left / right: D-pad and the left stick
 *
 * Face buttons on a page: X and Y run GT4.hooks.x / GT4.hooks.y when the page
 * sets them. B closes an open help tip, then the top dialog, then goes back.
 *
 * Rebind (Controls): while GT4.capturing is set only B does anything on the pad (GT4.cancelCapture), and every key
 * goes to GT4.captureKey(event) first (it returns true when it used the key). The Wheel page's set-up wizard (TASK-181)
 * may also set GT4.capturePad(name): it gets the other pad presses while a capture runs (only when the wheel being set up
 * is not itself a gamepad, so a wheel button is never taken for a pad press).
 */
(function (root) {
  "use strict";
  var GT4 = (root.GT4 = root.GT4 || {});

  var FOCUSABLE = [
    "button:not([disabled])",
    "a[href]",
    "input:not([disabled])",
    "select:not([disabled])",
    "[data-focusable]:not([disabled]):not([aria-disabled=\"true\"])"
  ].join(",");

  var mode = "mouse";

  GT4.hooks = {};
  GT4.capturing = false;

  function setMode(next) {
    if (mode === next) {
      return;
    }
    mode = next;
    var body = document.body;
    body.classList.remove("input-mouse", "input-keys", "input-pad");
    body.classList.add("input-" + next);
  }

  // The top-most dialog owns navigation while it is open.
  // A page may narrow the scope (Settings: the tab list or the tab's rows) with GT4.focusScope().
  function scopeRoot() {
    var dialogs = document.querySelectorAll(".modal");
    if (dialogs.length) {
      return dialogs[dialogs.length - 1];
    }
    var narrowed = GT4.focusScope ? GT4.focusScope() : null;
    return narrowed || document.getElementById("app");
  }

  function isShown(el) {
    if (el.closest("[hidden],[inert],[aria-hidden=\"true\"]")) {
      return false;
    }
    var box = el.getBoundingClientRect();
    return box.width > 0 && box.height > 0;
  }

  function focusables() {
    var scope = scopeRoot();
    if (!scope) {
      return [];
    }
    return Array.prototype.filter.call(scope.querySelectorAll(FOCUSABLE), isShown);
  }

  function focusElement(el) {
    el.focus({ preventScroll: true });
    markFocus(el);
    if (el.scrollIntoView) {
      el.scrollIntoView({ block: "nearest", inline: "nearest" });
    }
  }

  function move(direction) {
    if (GT4.capturing) {
      return;
    }
    var list = focusables();
    if (!list.length) {
      return;
    }
    var index = list.indexOf(document.activeElement);
    if (index < 0) {
      focusElement(list[0]);
      return;
    }
    var rects = list.map(function (el) {
      var box = el.getBoundingClientRect();
      return { left: box.left, top: box.top, width: box.width, height: box.height };
    });
    var next = GT4.spatial.pickNext(rects, index, direction);
    if (next >= 0) {
      focusElement(list[next]);
    }
  }

  function isAdjustable(el) {
    return !!el && (el.type === "range" || el.hasAttribute("data-adjust"));
  }

  // Up/down: a page may take the press first (the virtual pad editor: the arrows move the selected element); otherwise
  // focus moves.
  function vertical(delta) {
    if (GT4.capturing) {
      return;
    }
    if (GT4.hooks.vertical && GT4.hooks.vertical(delta)) {
      return;
    }
    move(delta < 0 ? "up" : "down");
  }

  // Left/right change the focused slider or dropdown; otherwise they move focus.
  function horizontal(delta) {
    if (GT4.capturing) {
      return;
    }
    // A page may take the press first (Settings: D-pad right on a tab enters its content).
    if (GT4.hooks.horizontal && GT4.hooks.horizontal(delta)) {
      return;
    }
    var el = document.activeElement;
    if (isAdjustable(el)) {
      el.dispatchEvent(new CustomEvent("gt4-adjust", { detail: { delta: delta } }));
      return;
    }
    move(delta < 0 ? "left" : "right");
  }

  function activate() {
    if (GT4.capturing) {
      return;
    }
    var el = document.activeElement;
    if (el && el.tagName !== "INPUT" && typeof el.click === "function") {
      el.click();
    } else if (el && el.hasAttribute && el.hasAttribute("data-focusable")) {
      // SVG elements (the Controls drawing) have no click(): send the event the page listens to.
      el.dispatchEvent(new MouseEvent("click", { bubbles: true, cancelable: true }));
    }
  }

  function back() {
    if (GT4.capturing) {
      if (GT4.cancelCapture) {
        GT4.cancelCapture();
      }
      return;
    }
    if (GT4.ui.closeTips()) {
      return;
    }
    var dialog = GT4.ui.topDialog();
    if (dialog) {
      dialog.back();
      return;
    }
    if (GT4.hooks.back) {
      GT4.hooks.back();
    }
  }

  function blockedByOverlay() {
    return GT4.capturing || !!GT4.ui.topDialog();
  }

  function tabs(delta) {
    if (!blockedByOverlay() && GT4.hooks.tabs) {
      GT4.hooks.tabs(delta);
    }
  }

  function pageAction(name) {
    if (!blockedByOverlay() && GT4.hooks[name]) {
      GT4.hooks[name]();
    }
  }

  // One logical button press from the gamepad.
  // Face and shoulder buttons go through GT4.input.padRoute (pure, tested in dev/nav-test.html).
  function padPress(name) {
    if (GT4.capturing && name !== "b") {
      if (GT4.capturePad) {
        GT4.capturePad(name);
      }
      return;
    }
    setMode("pad");
    if (name === "up" || name === "down") {
      vertical(name === "up" ? -1 : 1);
      return;
    }
    if (name === "left" || name === "right") {
      horizontal(name === "left" ? -1 : 1);
      return;
    }
    var route = GT4.input.padRoute(name, {
      capturing: GT4.capturing,
      dialogOpen: !!GT4.ui.topDialog(),
      tipOpen: !!document.querySelector(".info.is-open"),
      hooks: { x: !!GT4.hooks.x, y: !!GT4.hooks.y, start: !!GT4.hooks.start, tabs: !!GT4.hooks.tabs }
    });
    switch (route) {
      case "activate": activate(); break;
      case "back": case "close-tip": case "dialog-back": case "cancel-capture": back(); break;
      case "hook-x": GT4.hooks.x(); break;
      case "hook-y": GT4.hooks.y(); break;
      case "hook-start": GT4.hooks.start(); break;
      case "tab-prev": GT4.hooks.tabs(-1); break;
      case "tab-next": GT4.hooks.tabs(1); break;
      default: break;
    }
  }

  // Nearest scrollable ancestor of the focused element, else the page's main scroll area.
  function isScrollable(el) {
    if (!el || el.scrollHeight <= el.clientHeight + 1) {
      return false;
    }
    var overflow = root.getComputedStyle(el).overflowY;
    return overflow === "auto" || overflow === "scroll";
  }

  function scrollTarget() {
    var el = document.activeElement;
    while (el && el !== document.body) {
      if (isScrollable(el)) {
        return el;
      }
      el = el.parentElement;
    }
    var fallback = scopeRoot().querySelector(".option-list, .lib-list, .content");
    return isScrollable(fallback) ? fallback : null;
  }

  // y: the right stick's vertical axis, -1..1, positive down (pad-live).
  function scrollWithStick(y) {
    if (GT4.capturing) {
      return;
    }
    var step = GT4.input ? GT4.input.stickScroll(y || 0) : 0;
    if (step) {
      var target = scrollTarget();
      if (target) {
        target.scrollTop += step;
      }
    }
  }

  // The pad's discrete presses (pad-nav) and its right stick (pad-live). Repeat is done by Rust.
  GT4.backend.listen("pad-nav", function (payload) {
    // While the 1.3.0 intro plays, a pad button only skips it (js/intro.js); it must not also press Play.
    if (GT4.intro && GT4.intro.isPlaying()) {
      return;
    }
    if (payload && payload.name) {
      padPress(payload.name);
    }
  });
  GT4.backend.listen("pad-live", function (payload) {
    scrollWithStick(payload && payload.sticks && payload.sticks.right ? payload.sticks.right.y : 0);
  });

  document.addEventListener("keydown", function (event) {
    if (GT4.capturing) {
      // A rebind that takes keys (Controls) gets every key first; Esc still cancels when it does not.
      if (GT4.captureKey && GT4.captureKey(event)) {
        event.preventDefault();
        return;
      }
      if (event.key === "Escape") {
        event.preventDefault();
        back();
      }
      return;
    }
    setMode("keys");
    switch (event.key) {
      case "ArrowUp":
        event.preventDefault();
        vertical(-1);
        break;
      case "ArrowDown":
        event.preventDefault();
        vertical(1);
        break;
      case "ArrowLeft":
        event.preventDefault();
        horizontal(-1);
        break;
      case "ArrowRight":
        event.preventDefault();
        horizontal(1);
        break;
      case "Escape":
        event.preventDefault();
        back();
        break;
      case "q":
      case "Q":
        tabs(-1);
        break;
      case "e":
      case "E":
        tabs(1);
        break;
      default:
        break;
    }
  });

  // Exactly one element carries the focus ring class; CSS shows it per input mode.
  // Set both by focusin (Tab key) and by focusElement (programmatic moves), since
  // focus events are not guaranteed when the window is not focused.
  function markFocus(target) {
    document.querySelectorAll(".ring-on").forEach(function (el) {
      if (el !== target) {
        el.classList.remove("ring-on");
      }
    });
    if (target && target.classList) {
      target.classList.add("ring-on");
    }
  }
  document.addEventListener("focusin", function (event) {
    markFocus(event.target);
  });

  document.addEventListener("pointerdown", function () { setMode("mouse"); }, true);
  document.addEventListener("pointermove", function () { setMode("mouse"); }, true);

  // "mouse", "keys" or "pad": the last kind of input used. Pages use it to tell a pad A from a mouse click.
  GT4.inputMode = function () {
    return mode;
  };

  GT4.nav = {
    move: move,
    activate: activate,
    horizontal: horizontal,
    back: back,
    focusables: focusables,
    setMode: setMode
  };
  GT4.back = back;
})(window);
