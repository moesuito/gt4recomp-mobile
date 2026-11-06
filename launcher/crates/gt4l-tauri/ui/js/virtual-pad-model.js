/*
 * Pure rules of the virtual controller layout (TASK-185; no DOM, tested in dev/nav-test.html):
 *
 *   GT4.virtualPadModel.ASPECTS           the three preview shapes: 20:9 (default), 16:9 and 21:9
 *   GT4.virtualPadModel.RESERVED          the F7 report button's corner, in screen fractions (top left)
 *   GT4.virtualPadModel.rect(element, ratio)
 *                                         the element as screen fractions {x0, y0, x1, y1, width, height}
 *   GT4.virtualPadModel.move/resize/setSize/setWidth/setOpacity/setVisible
 *                                         a new layout with one element changed, kept inside the screen
 *   GT4.virtualPadModel.overlapsReserved(layout, ratio)
 *                                         the elements that sit on the F7 corner
 *   GT4.virtualPadModel.problems(layout)  the contract's rules, for the Save button and the message
 *   GT4.virtualPadModel.clone(layout)     a deep copy (the editor never edits the loaded object)
 *
 * The format is the TASK-185 contract (docs/CONTROLS.md, `android/virtual-pad/default-layout.json`): `x` and `y` are the
 * element's centre, `size` its height as a fraction of the screen height, and the optional `width` its width as a
 * fraction of the screen width. Without `width` the element is square, so its width in x depends on the screen shape:
 * `size / (2 * ratio)`. That is the only place the aspect enters, which is why the editor previews other shapes.
 *
 * Elements are kept inside the screen while they are moved or resized here (the contract's editor rule). A layout the
 * player already has is shown as it is: the editor never rewrites coordinates it was not asked to change.
 */
(function (root) {
  "use strict";
  var GT4 = (root.GT4 = root.GT4 || {});

  // 20:9 is the phone the owner tests on; 16:9 and 21:9 show how the same layout fits other screens.
  var ASPECTS = [
    { id: "20:9", ratio: 20 / 9 },
    { id: "16:9", ratio: 16 / 9 },
    { id: "21:9", ratio: 21 / 9 }
  ];
  var DEFAULT_ASPECT = "20:9";

  // The F7 report button: 44dp at an 8dp margin plus the window insets, in the top-left corner (GameActivity.kt). The
  // zone is generous (5% of the width, 13% of the height); a real overlap is an intersection of both axes.
  var RESERVED = { x0: 0, y0: 0, x1: 0.05, y1: 0.13 };
  var OVERLAP_EPSILON = 0.004;

  var MIN_SIZE = 0.04;
  var MAX_SIZE = 0.9;
  var GRID = 0.02;

  var BUTTON_BINDS = ["cross", "circle", "square", "triangle", "l1", "r1", "l2", "r2", "l3", "r3", "select", "start"];
  var TYPES = ["button", "dpad", "stick"];

  function clone(layout) {
    return JSON.parse(JSON.stringify(layout));
  }

  function ratioOf(aspectId) {
    for (var i = 0; i < ASPECTS.length; i++) {
      if (ASPECTS[i].id === aspectId) {
        return ASPECTS[i].ratio;
      }
    }
    return ASPECTS[0].ratio;
  }

  function aspectIdOf(ratio) {
    for (var i = 0; i < ASPECTS.length; i++) {
      if (Math.abs(ASPECTS[i].ratio - ratio) < 0.0001) {
        return ASPECTS[i].id;
      }
    }
    return DEFAULT_ASPECT;
  }

  function find(layout, id) {
    var elements = (layout && layout.elements) || [];
    for (var i = 0; i < elements.length; i++) {
      if (elements[i].id === id) {
        return elements[i];
      }
    }
    return null;
  }

  function isWide(element) {
    return !!element && typeof element.width === "number" && isFinite(element.width);
  }

  // The element's half width in screen x fractions: a given width, or half of a square of `size` of the height.
  function halfWidth(element, ratio) {
    if (isWide(element)) {
      return element.width / 2;
    }
    return element.size / (2 * (ratio || ASPECTS[0].ratio));
  }

  function rect(element, ratio) {
    var half = halfWidth(element, ratio);
    var height = element.size;
    return {
      x0: element.x - half,
      x1: element.x + half,
      y0: element.y - height / 2,
      y1: element.y + height / 2,
      width: half * 2,
      height: height
    };
  }

  function clamp(value, min, max) {
    return Math.min(max, Math.max(min, value));
  }

  function snapValue(value, step) {
    return Math.round(value / step) * step;
  }

  // Keeps a square or a wide element inside the screen of this shape (the contract's editor rule).
  function place(element, x, y, ratio) {
    var half = halfWidth(element, ratio);
    var halfHeight = element.size / 2;
    element.x = half > 0.5 ? 0.5 : clamp(x, half, 1 - half);
    element.y = halfHeight > 0.5 ? 0.5 : clamp(y, halfHeight, 1 - halfHeight);
    return element;
  }

  function withElement(layout, id, change) {
    var next = clone(layout);
    var element = find(next, id);
    if (element) {
      change(element);
    }
    return next;
  }

  function move(layout, id, dx, dy, ratio) {
    return withElement(layout, id, function (element) {
      place(element, element.x + dx, element.y + dy, ratio);
    });
  }

  // A uniform scale (the size slider and the shoulder buttons): a wide element keeps its shape.
  function resize(layout, id, factor, ratio) {
    return withElement(layout, id, function (element) {
      if (!(factor > 0) || !isFinite(factor)) {
        return;
      }
      var size = clamp(element.size * factor, MIN_SIZE, MAX_SIZE);
      var scale = element.size > 0 ? size / element.size : 1;
      element.size = size;
      if (isWide(element)) {
        element.width = clamp(element.width * scale, 0.02, 1);
      }
      place(element, element.x, element.y, ratio);
    });
  }

  function setSize(layout, id, size, ratio) {
    return withElement(layout, id, function (element) {
      var target = clamp(Number(size) || 0, MIN_SIZE, MAX_SIZE);
      var scale = element.size > 0 ? target / element.size : 1;
      element.size = target;
      if (isWide(element)) {
        element.width = clamp(element.width * scale, 0.02, 1);
      }
      place(element, element.x, element.y, ratio);
    });
  }

  // The width slider: only for a wide button, and only its own axis (the height stays where it is).
  function setWidth(layout, id, width, ratio) {
    return withElement(layout, id, function (element) {
      if (!isWide(element)) {
        return;
      }
      element.width = clamp(Number(width) || 0, 0.02, 1);
      place(element, element.x, element.y, ratio);
    });
  }

  function setOpacity(layout, id, opacity) {
    return withElement(layout, id, function (element) {
      element.opacity = clamp(Number(opacity) || 0, 0, 1);
    });
  }

  function setVisible(layout, id, visible) {
    return withElement(layout, id, function (element) {
      if (visible) {
        delete element.visible;
      } else {
        element.visible = false;
      }
    });
  }

  // The F7 corner's overlap: an intersection that is more than a rounding error on both axes.
  function overlapsReserved(layout, ratio) {
    var ids = [];
    ((layout && layout.elements) || []).forEach(function (element) {
      var box = rect(element, ratio);
      var width = Math.min(box.x1, RESERVED.x1) - Math.max(box.x0, RESERVED.x0);
      var height = Math.min(box.y1, RESERVED.y1) - Math.max(box.y0, RESERVED.y0);
      if (width > OVERLAP_EPSILON && height > OVERLAP_EPSILON) {
        ids.push(element.id);
      }
    });
    return ids;
  }

  function bindAllowed(kind, binds) {
    if (kind === "button") {
      return BUTTON_BINDS.indexOf(binds) >= 0;
    }
    if (kind === "dpad") {
      return binds === "dpad";
    }
    if (kind === "stick") {
      return binds === "left_stick" || binds === "right_stick";
    }
    return false;
  }

  function inUnit(value) {
    return typeof value === "number" && isFinite(value) && value >= 0 && value <= 1;
  }

  // The same rules as gt4l_core::virtual_pad::Layout::problems, as short codes the page translates. The editor's Save
  // is refused while this list is not empty (and the Rust side checks again).
  function problems(layout) {
    var found = [];
    if (!layout || layout.version !== 1) {
      found.push({ code: "version", id: "" });
    }
    if (!layout) {
      return found;
    }
    if (typeof layout.opacity !== "number" || !inUnit(layout.opacity)) {
      found.push({ code: "opacity", id: "" });
    }
    var elements = layout.elements || [];
    if (!elements.length) {
      found.push({ code: "elements", id: "" });
    }
    if (elements.length > 32) {
      found.push({ code: "too_many", id: "" });
    }
    var seen = {};
    elements.forEach(function (element) {
      var id = String(element.id || "");
      if (!id || id.length > 64) {
        found.push({ code: "id", id: id });
      } else if (seen[id]) {
        found.push({ code: "duplicate_id", id: id });
      }
      seen[id] = true;
      if (TYPES.indexOf(element.type) < 0) {
        found.push({ code: "type", id: id });
      } else if (!bindAllowed(element.type, element.binds)) {
        found.push({ code: "binds", id: id });
      }
      if (!inUnit(element.x)) {
        found.push({ code: "x", id: id });
      }
      if (!inUnit(element.y)) {
        found.push({ code: "y", id: id });
      }
      if (typeof element.size !== "number" || !isFinite(element.size) || element.size <= 0 || element.size > 1) {
        found.push({ code: "size", id: id });
      }
      if (typeof element.width === "number" && (!isFinite(element.width) || element.width <= 0 || element.width > 1)) {
        found.push({ code: "width", id: id });
      }
      if (typeof element.opacity === "number" && !inUnit(element.opacity)) {
        found.push({ code: "element_opacity", id: id });
      }
      if (typeof element.deadzone === "number") {
        if (element.type !== "stick") {
          found.push({ code: "deadzone", id: id });
        } else if (!isFinite(element.deadzone) || element.deadzone < 0 || element.deadzone > 0.5) {
          found.push({ code: "deadzone", id: id });
        }
      }
    });
    return found;
  }

  // The name of a layout element in the interface language: the Controls page's own names (Cross, L1, Left stick),
  // plus the D-pad. The bind is the fallback when the key or GT4.t is missing (the pure tests run without locales).
  function nameOf(element) {
    var bind = (element && element.binds) || "";
    var key = element && element.type === "dpad" ? "vpad.name.dpad"
      : element && element.type === "stick" ? (bind === "right_stick" ? "controls.right_stick" : "controls.left_stick")
      : "controls.button." + bind;
    if (GT4.t && GT4.i18n && GT4.i18n.has(key)) {
      return GT4.t(key);
    }
    return bind === "left_stick" ? "Left stick" : bind === "right_stick" ? "Right stick" : bind === "dpad" ? "D-pad" : bind;
  }

  GT4.virtualPadModel = {
    ASPECTS: ASPECTS,
    DEFAULT_ASPECT: DEFAULT_ASPECT,
    RESERVED: RESERVED,
    BUTTON_BINDS: BUTTON_BINDS,
    MIN_SIZE: MIN_SIZE,
    MAX_SIZE: MAX_SIZE,
    GRID: GRID,
    clone: clone,
    ratioOf: ratioOf,
    aspectIdOf: aspectIdOf,
    find: find,
    isWide: isWide,
    halfWidth: halfWidth,
    rect: rect,
    snapValue: snapValue,
    move: move,
    resize: resize,
    setSize: setSize,
    setWidth: setWidth,
    setOpacity: setOpacity,
    setVisible: setVisible,
    overlapsReserved: overlapsReserved,
    problems: problems,
    nameOf: nameOf
  };
})(window);
