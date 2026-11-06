/*
 * TASK-185: the virtual controller layout editor (Android; shown in the Windows dev mock for testing).
 *
 * A full-screen landscape editor over a dimmed race scene: every element of `user/virtual-pad.json` where the game
 * draws it, on the 20:9, 16:9 or 21:9 shape the player picks. Touch drags an element, the corner handle or a pinch
 * resizes it, a tap selects it; the dock shows the selected element's size, width (wide buttons), opacity and
 * visibility, and the layout's global opacity, vibration and snap-to-grid. Save writes the file through
 * `save_virtual_pad`; Cancel throws the changes away; Undo steps back; Reset to default loads the built-in layout.
 *
 * Keyboard: Tab walks the controls (a chip selects its element), the arrows move the selected element, Shift+arrows or
 * [ ] resize it, Enter/Space activates, Esc closes. Pad: the D-pad and A walk the controls and select, the left stick
 * moves the selected element, LB/RB resize it, X undoes, Y resets, B goes back (A/B keep the launcher's meaning).
 *
 * The F7 report button's corner is drawn as a reserved zone; an element that overlaps it gets a warning (the contract
 * asks the editor to warn; saving is still allowed). Elements are kept inside the screen while they are moved or
 * resized here, and the file always keeps screen fractions, so the layout follows any phone.
 *
 * `GT4.virtualPadEditor.open({view, returnFocus, onSaved, onClosed})`; `view` is the `get_virtual_pad` payload, and
 * `returnFocus` is the button that opened the editor (it gets the focus back when the editor closes).
 */
(function (root) {
  "use strict";
  var GT4 = (root.GT4 = root.GT4 || {});
  var h = GT4.ui.h;
  var M = GT4.virtualPadModel;

  // Kept while the app runs: the last preview shape and the snap choice (editor preferences, not part of the file).
  var remembered = { aspect: M.DEFAULT_ASPECT, snap: true };
  var UNDO_LIMIT = 30;

  function t(key, params) {
    return GT4.t(key, params);
  }

  function svgNode(tag, attrs, parent) {
    var node = document.createElementNS("http://www.w3.org/2000/svg", tag);
    Object.keys(attrs || {}).forEach(function (key) {
      node.setAttribute(key, String(attrs[key]));
    });
    if (parent) {
      parent.appendChild(node);
    }
    return node;
  }

  // The picture of one element: the DS2 glyph for a button, a cross for the D-pad, a ring and a knob for a stick.
  function elementArt(element) {
    var box = h("span", { class: "vp-art", "aria-hidden": "true" });
    if (element.type === "dpad") {
      var cross = document.createElementNS("http://www.w3.org/2000/svg", "svg");
      cross.setAttribute("viewBox", "0 0 64 64");
      cross.setAttribute("class", "vp-dpad");
      svgNode("path", { d: "M24 4 h16 v20 h20 v16 h-20 v20 h-16 v-20 h-20 v-16 h20 Z" }, cross);
      box.appendChild(cross);
      return box;
    }
    if (element.type === "stick") {
      var scope = document.createElementNS("http://www.w3.org/2000/svg", "svg");
      scope.setAttribute("viewBox", "0 0 100 100");
      scope.setAttribute("class", "vp-stick");
      svgNode("circle", { class: "ring", cx: 50, cy: 50, r: 46 }, scope);
      svgNode("circle", { class: "knob", cx: 50, cy: 50, r: 27 }, scope);
      box.appendChild(scope);
      return box;
    }
    box.appendChild(GT4.controllerSvg.glyph(element.binds));
    return box;
  }

  function chipArt(element) {
    var box = h("span", { class: "vp-chip-art", "aria-hidden": "true" });
    box.appendChild(elementArt(element));
    return box;
  }

  GT4.virtualPadEditor = {
    open: function (options) {
      var view = options.view || {};
      var state = {
        layout: M.clone(view.layout),
        defaultLayout: M.clone(view.default_layout),
        locked: !!view.locked,
        aspect: remembered.aspect,
        snap: remembered.snap,
        selected: null,
        history: [],
        original: JSON.stringify(view.layout),
        saving: false
      };
      var previousHooks = GT4.hooks;
      var previousFocusScope = GT4.focusScope;
      var closed = false;
      var drag = null;
      var pointers = {};
      var stickActive = false;

      function ratio() {
        return M.ratioOf(state.aspect);
      }

      // Compared with the layout the editor opened on, so undoing everything makes Save and Cancel behave as if untouched.
      function isDirty() {
        return JSON.stringify(state.layout) !== state.original;
      }

      // ---- the shell: bar, stage, warning line, dock ----
      var title = h("h2", { text: t("vpad.editor_title") });
      var subtitle = h("p", { class: "vp-sub", text: "" });
      var aspectButtons = {};
      var aspectGroup = h("div", { class: "vp-aspect", role: "group", "aria-label": t("vpad.aspect_label") },
        M.ASPECTS.map(function (aspect) {
          var button = h("button", {
            class: "vp-aspect-btn",
            type: "button",
            "aria-pressed": "false",
            data: { aspect: aspect.id },
            on: { click: function () { setAspect(aspect.id); } }
          }, aspect.id);
          aspectButtons[aspect.id] = button;
          return button;
        }));
      var undoButton = h("button", { class: "btn small", type: "button", "data-action": "undo", on: { click: undo } }, t("vpad.undo"));
      var resetButton = h("button", { class: "btn small", type: "button", "data-action": "reset", on: { click: resetLayout } }, t("vpad.reset_default"));
      var cancelButton = h("button", { class: "btn small", type: "button", "data-action": "cancel", on: { click: requestClose } }, t("common.cancel"));
      var saveButton = h("button", { class: "btn primary small", type: "button", "data-action": "save", on: { click: save } }, t("vpad.save"));
      var bar = h("header", { class: "vp-bar" }, [
        h("div", { class: "vp-heading" }, [title, subtitle]),
        aspectGroup,
        h("div", { class: "vp-actions" }, [undoButton, resetButton, cancelButton, saveButton])
      ]);

      var canvas = h("div", {
        class: "vp-canvas",
        tabindex: 0,
        role: "application",
        "data-focusable": "1",
        "aria-label": t("vpad.canvas_label")
      });
      var sky = h("div", { class: "vp-scene", "aria-hidden": "true" }, [
        h("div", { class: "vp-sky" }),
        h("div", { class: "vp-sun" }),
        h("div", { class: "vp-hills" }),
        h("div", { class: "vp-road" }),
        h("div", { class: "vp-lane" }),
        h("div", { class: "vp-dim" })
      ]);
      var reserved = h("div", { class: "vp-reserved", "aria-hidden": "true" }, [h("span", { text: "F7" })]);
      var handle = h("div", { class: "vp-handle", hidden: true, "aria-hidden": "true" });
      canvas.appendChild(sky);
      canvas.appendChild(reserved);
      var stage = h("div", { class: "vp-stage" }, [canvas]);
      var warning = h("p", { class: "vp-warning", role: "status", hidden: true, text: "" });
      var chips = h("div", { class: "vp-chips", role: "group", "aria-label": t("vpad.elements") });
      var inspector = h("div", { class: "vp-inspector" });
      var globals = h("div", { class: "vp-globals" });
      var dock = h("footer", { class: "vp-dock" }, [
        h("div", { class: "vp-dock-row" }, [
          h("span", { class: "vp-dock-label", text: t("vpad.elements") }),
          chips
        ]),
        h("div", { class: "vp-dock-row vp-dock-controls" }, [inspector, globals])
      ]);
      var editor = h("div", { class: "vp-editor", role: "dialog", "aria-modal": "true", "aria-labelledby": "vp-title" }, [
        bar, stage, warning, dock
      ]);
      title.id = "vp-title";
      document.body.appendChild(editor);
      // The page behind the editor is out of reach: no Tab, no click, no screen reader there.
      var appNode = document.getElementById("app");
      if (appNode) {
        appNode.setAttribute("inert", "");
      }

      var nodes = {};
      state.layout.elements.forEach(function (element) {
        var node = h("div", {
          class: "vp-el",
          data: { el: element.id, kind: element.type, wide: M.isWide(element) ? "1" : "0" },
          title: M.nameOf(element)
        }, [elementArt(element)]);
        node.addEventListener("pointerdown", function (event) { onElementPointerDown(event, element.id); });
        canvas.appendChild(node);
        nodes[element.id] = node;
      });
      canvas.appendChild(handle);
      handle.addEventListener("pointerdown", onHandlePointerDown);

      function paintElements() {
        var shape = ratio();
        state.layout.elements.forEach(function (element) {
          var node = nodes[element.id];
          if (!node) {
            return;
          }
          var box = M.rect(element, shape);
          node.style.left = (box.x0 * 100).toFixed(3) + "%";
          node.style.top = (box.y0 * 100).toFixed(3) + "%";
          node.style.width = (box.width * 100).toFixed(3) + "%";
          node.style.height = (box.height * 100).toFixed(3) + "%";
          node.style.opacity = String(element.opacity === undefined ? state.layout.opacity : element.opacity);
          node.classList.toggle("is-hidden", element.visible === false);
          node.classList.toggle("is-selected", element.id === state.selected);
        });
        paintHandle();
        paintChips();
      }

      function paintHandle() {
        var element = state.selected ? M.find(state.layout, state.selected) : null;
        if (!element) {
          handle.hidden = true;
          return;
        }
        var box = M.rect(element, ratio());
        handle.hidden = false;
        handle.style.left = ((box.x0 + box.width) * 100).toFixed(3) + "%";
        handle.style.top = ((box.y0 + box.height) * 100).toFixed(3) + "%";
        handle.style.width = Math.max(14, Math.round(canvas.clientWidth * 0.022)) + "px";
        handle.style.height = handle.style.width;
      }

      function paintChips() {
        Array.prototype.forEach.call(chips.children, function (chip) {
          var id = chip.getAttribute("data-el");
          var element = M.find(state.layout, id);
          var name = element ? M.nameOf(element) : id;
          chip.classList.toggle("is-selected", id === state.selected);
          chip.setAttribute("aria-pressed", String(id === state.selected));
          chip.setAttribute("aria-label", element && element.visible === false ? name + ", " + t("vpad.hidden") : name);
          var text = chip.querySelector(".vp-chip-name");
          if (text) {
            text.textContent = name + (element && element.visible === false ? " · " + t("vpad.hidden") : "");
          }
        });
      }

      function paintStatus() {
        var source = state.locked ? t("vpad.locked") : view.source === "player" ? t("vpad.source_player") : t("vpad.source_default");
        if (view.source !== "player" && view.notes && view.notes.length && !state.locked) {
          source = t("vpad.source_built_in_note", { reason: view.notes[0] });
        }
        subtitle.textContent = isDirty() ? source + " · " + t("vpad.unsaved") : source;
        Object.keys(aspectButtons).forEach(function (id) {
          aspectButtons[id].setAttribute("aria-pressed", String(id === state.aspect));
        });
        undoButton.disabled = !state.history.length || state.locked;
        resetButton.disabled = state.locked;
        saveButton.disabled = state.locked || !isDirty() || state.saving;
        cancelButton.disabled = state.saving;
      }

      function paintWarning() {
        var problem = M.problems(state.layout)[0];
        var overlapping = M.overlapsReserved(state.layout, ratio());
        var lines = [];
        if (problem) {
          lines.push(t("vpad.warn_cannot_save", { reason: t("vpad.problem." + problem.code) }));
        }
        if (overlapping.length) {
          var names = overlapping.map(function (id) {
            var element = M.find(state.layout, id);
            return element ? M.nameOf(element) : id;
          });
          lines.push(t("vpad.warn_f7", { names: names.join(", ") }));
        }
        warning.hidden = lines.length === 0;
        warning.textContent = lines.join(" ");
        warning.classList.toggle("is-error", !!problem);
      }

      function paint() {
        paintElements();
        paintStatus();
        paintWarning();
      }

      // ---- the dock's controls (the settings row shapes, so the pad and the keyboard step them the same way) ----
      function controlContext() {
        return {
          table: { rows: [] },
          content: dock,
          selfDrawn: {},
          commit: function (id, value) {
            applyControl(id, value);
            return Promise.resolve();
          },
          refresh: function () { return Promise.resolve(); }
        };
      }

      function rowSpec(id, label, spec) {
        return Object.assign({ id: id, label: label, control: "slider" }, spec);
      }

      function appendRow(into, ctx, row) {
        var node = GT4.settingsRows.rowNode(ctx, row);
        if (node) {
          into.appendChild(node);
        }
      }

      function paintInspector() {
        GT4.ui.clear(inspector);
        var element = state.selected ? M.find(state.layout, state.selected) : null;
        if (!element) {
          inspector.appendChild(h("p", { class: "vp-empty", text: t("vpad.no_selection") }));
          return;
        }
        var ctx = controlContext();
        var selection = h("div", { class: "vp-selection" }, [
          h("span", { class: "vp-selection-art" }, [elementArt(element)]),
          h("span", { class: "vp-selection-name", text: M.nameOf(element) })
        ]);
        var rows = h("div", { class: "vp-rows" });
        appendRow(rows, ctx, rowSpec("vpad-size", t("vpad.size"), {
          values: { min: Math.round(M.MIN_SIZE * 100), max: Math.round(M.MAX_SIZE * 100), step: 1, unit: "%" },
          current: Math.round(element.size * 100)
        }));
        if (M.isWide(element)) {
          appendRow(rows, ctx, rowSpec("vpad-width", t("vpad.width"), {
            values: { min: 2, max: 100, step: 1, unit: "%" },
            current: Math.round(element.width * 100)
          }));
        }
        appendRow(rows, ctx, rowSpec("vpad-opacity", t("vpad.opacity"), {
          values: { min: 0, max: 100, step: 5, unit: "%" },
          current: Math.round((element.opacity === undefined ? state.layout.opacity : element.opacity) * 100)
        }));
        appendRow(rows, ctx, {
          id: "vpad-visible", label: t("vpad.visible"), control: "toggle", values: null,
          current: element.visible !== false
        });
        inspector.appendChild(h("div", { class: "vp-selection-row" }, [selection, rows]));
      }

      function paintGlobals() {
        GT4.ui.clear(globals);
        var ctx = controlContext();
        var rows = h("div", { class: "vp-rows" });
        appendRow(rows, ctx, rowSpec("vpad-global-opacity", t("vpad.global_opacity"), {
          values: { min: 0, max: 100, step: 5, unit: "%" },
          current: Math.round(state.layout.opacity * 100)
        }));
        appendRow(rows, ctx, {
          id: "vpad-haptics", label: t("vpad.haptics"), control: "toggle", values: null,
          current: state.layout.haptics !== false
        });
        appendRow(rows, ctx, {
          id: "vpad-snap", label: t("vpad.snap"), control: "toggle", values: null, current: state.snap
        });
        globals.appendChild(h("div", { class: "vp-selection-row" }, [
          h("span", { class: "vp-selection-name", text: t("vpad.globals") }),
          rows
        ]));
      }

      function paintPanels() {
        var focused = document.activeElement && editor.contains(document.activeElement)
          ? document.activeElement.getAttribute("data-setting") : null;
        paintInspector();
        paintGlobals();
        if (focused) {
          var again = editor.querySelector("[data-setting=\"" + focused + "\"]");
          if (again) {
            again.focus({ preventScroll: true });
          }
        }
        paintHandle();
      }

      function applyControl(id, value) {
        if (id === "vpad-snap") {
          state.snap = !!value;
          remembered.snap = state.snap;
          return;
        }
        pushUndo();
        if (id === "vpad-size" && state.selected) {
          state.layout = M.setSize(state.layout, state.selected, Number(value) / 100, ratio());
        } else if (id === "vpad-width" && state.selected) {
          state.layout = M.setWidth(state.layout, state.selected, Number(value) / 100, ratio());
        } else if (id === "vpad-opacity" && state.selected) {
          state.layout = M.setOpacity(state.layout, state.selected, Number(value) / 100);
        } else if (id === "vpad-visible" && state.selected) {
          state.layout = M.setVisible(state.layout, state.selected, !!value);
        } else if (id === "vpad-global-opacity") {
          state.layout.opacity = Math.min(1, Math.max(0, Number(value) / 100));
        } else if (id === "vpad-haptics") {
          state.layout.haptics = !!value;
        }
        paint();
        paintPanels();
      }

      // ---- chips, selection ----
      state.layout.elements.forEach(function (element) {
        var chip = h("button", {
          class: "vp-chip",
          type: "button",
          data: { el: element.id },
          "aria-pressed": "false",
          on: {
            click: function () {
              select(element.id);
              if (GT4.inputMode() === "pad" || GT4.inputMode() === "keys") {
                canvas.focus({ preventScroll: true });
              }
            }
          }
        }, [chipArt(element), h("span", { class: "vp-chip-name", text: M.nameOf(element) })]);
        chips.appendChild(chip);
      });

      function chipFor(id) {
        return chips.querySelector("[data-el=\"" + id + "\"]");
      }

      // Tab or the pad's D-pad reaching a chip selects its element (the launcher's selection follows focus).
      chips.addEventListener("focusin", function (event) {
        var chip = event.target && event.target.closest ? event.target.closest(".vp-chip") : null;
        if (chip) {
          select(chip.getAttribute("data-el"));
        }
      });

      function select(id) {
        if (!M.find(state.layout, id)) {
          return;
        }
        var changed = state.selected !== id;
        state.selected = id;
        if (changed) {
          paintElements();
          paintInspector();
        }
      }

      // ---- history ----
      function pushUndo() {
        state.history.push(M.clone(state.layout));
        if (state.history.length > UNDO_LIMIT) {
          state.history.shift();
        }
      }

      function undo() {
        if (state.locked || !state.history.length) {
          return;
        }
        state.layout = state.history.pop();
        if (!M.find(state.layout, state.selected)) {
          state.selected = state.layout.elements.length ? state.layout.elements[0].id : null;
        }
        paint();
        paintPanels();
      }

      function resetLayout() {
        if (state.locked) {
          return;
        }
        GT4.ui.openDialog({
          title: t("vpad.reset_title"),
          text: t("vpad.reset_text"),
          buttons: [
            { label: t("common.cancel") },
            {
              label: t("vpad.reset_default"),
              primary: true,
              run: function () {
                pushUndo();
                state.layout = M.clone(state.defaultLayout);
                state.selected = state.layout.elements.length ? state.layout.elements[0].id : null;
                paint();
                paintPanels();
                refreshHints();
              }
            }
          ],
          onClose: function () { refreshHints(); }
        });
        setHints();
      }

      // ---- touch and mouse: drag, resize handle, pinch ----
      function onElementPointerDown(event, id) {
        if (state.locked || event.button === 2) {
          return;
        }
        event.preventDefault();
        event.stopPropagation();
        select(id);
        canvas.focus({ preventScroll: true });
        beginGesture(event, id, "move");
      }

      function onHandlePointerDown(event) {
        if (state.locked || !state.selected) {
          return;
        }
        event.preventDefault();
        event.stopPropagation();
        beginGesture(event, state.selected, "resize");
      }

      function canvasPoint(event) {
        var box = canvas.getBoundingClientRect();
        return { x: event.clientX - box.left, y: event.clientY - box.top, width: box.width, height: box.height };
      }

      function beginGesture(event, id, kind) {
        var point = canvasPoint(event);
        pointers[event.pointerId] = point;
        try {
          canvas.setPointerCapture(event.pointerId);
        } catch (error) {
          // A synthetic pointer (the dev checks) has no active pointer to capture; the drag still works.
        }
        var element = M.find(state.layout, id);
        if (!element) {
          return;
        }
        var box = M.rect(element, ratio());
        var centre = { x: (box.x0 + box.width / 2) * point.width, y: (box.y0 + box.height / 2) * point.height };
        drag = {
          kind: kind,
          id: id,
          origin: M.clone(state.layout),
          start: point,
          centre: centre,
          startDistance: Math.max(1, Math.hypot(point.x - centre.x, point.y - centre.y)),
          moved: false,
          pointerId: event.pointerId
        };
      }

      function onCanvasPointerMove(event) {
        var point = canvasPoint(event);
        if (pointers[event.pointerId]) {
          pointers[event.pointerId] = point;
        }
        if (!drag) {
          return;
        }
        var ids = Object.keys(pointers);
        if (ids.length >= 2) {
          // A pinch: the distance between the two fingers scales the element.
          var second = pointers[ids.filter(function (id) { return String(id) !== String(drag.pointerId); })[0] || ids[1]];
          var current = pointers[drag.pointerId] || point;
          var distance = Math.max(1, Math.hypot(current.x - second.x, current.y - second.y));
          var pinchStart = drag.pinchStart;
          if (!pinchStart) {
            drag.pinchStart = distance;
            drag.pinchOrigin = M.clone(drag.origin);
            return;
          }
          if (!drag.moved) {
            pushUndo();
            drag.moved = true;
          }
          state.layout = M.resize(drag.pinchOrigin, drag.id, distance / pinchStart, ratio());
          paintElements();
          paintWarning();
          return;
        }
        if (drag.kind === "resize") {
          var centre = drag.centre;
          var finger = Math.max(1, Math.hypot(point.x - centre.x, point.y - centre.y));
          if (!drag.moved && Math.abs(finger - drag.startDistance) >= 2) {
            pushUndo();
            drag.moved = true;
          }
          state.layout = M.resize(drag.origin, drag.id, finger / drag.startDistance, ratio());
        } else {
          var dx = (point.x - drag.start.x) / point.width;
          var dy = (point.y - drag.start.y) / point.height;
          if (!drag.moved && (Math.abs(point.x - drag.start.x) >= 2 || Math.abs(point.y - drag.start.y) >= 2)) {
            pushUndo();
            drag.moved = true;
          }
          if (drag.moved) {
            var element = M.find(drag.origin, drag.id);
            var x = element.x + dx;
            var y = element.y + dy;
            if (state.snap) {
              x = M.snapValue(x, M.GRID);
              y = M.snapValue(y, M.GRID);
            }
            state.layout = M.move(drag.origin, drag.id, x - element.x, y - element.y, ratio());
          }
        }
        paintElements();
        paintWarning();
      }

      function endGesture(event) {
        delete pointers[event.pointerId];
        if (!drag || drag.pointerId !== event.pointerId) {
          return;
        }
        var moved = drag.moved;
        drag = null;
        if (moved) {
          paint();
          paintPanels();
        }
      }

      canvas.addEventListener("pointermove", onCanvasPointerMove);
      canvas.addEventListener("pointerup", endGesture);
      canvas.addEventListener("pointercancel", endGesture);
      canvas.addEventListener("pointerdown", function (event) {
        if (event.target === canvas || event.target === sky || (event.target.parentNode === sky)) {
          state.selected = null;
          paintElements();
          paintInspector();
          canvas.focus({ preventScroll: true });
        }
      });

      // The aspect preview does not change the file: coordinates are fractions, so the drawing reflows.
      function setAspect(id) {
        state.aspect = id;
        remembered.aspect = id;
        fitCanvas();
        paint();
      }

      function fitCanvas() {
        if (closed) {
          return;
        }
        var box = stage.getBoundingClientRect();
        var shape = ratio();
        var width = Math.max(80, Math.min(box.width, box.height * shape));
        var height = Math.max(45, width / shape);
        if (height > box.height) {
          height = box.height;
          width = height * shape;
        }
        canvas.style.width = Math.floor(width) + "px";
        canvas.style.height = Math.floor(height) + "px";
        paintHandle();
      }

      // ---- keyboard ----
      function editorFocusables() {
        return Array.prototype.filter.call(
          editor.querySelectorAll("button:not([disabled]), input:not([disabled]), [data-focusable]"),
          function (node) { return node.getBoundingClientRect().width > 0; }
        );
      }

      function trapTab(event) {
        var list = editorFocusables();
        if (!list.length) {
          return;
        }
        var first = list[0];
        var last = list[list.length - 1];
        if (!event.shiftKey && document.activeElement === last) {
          event.preventDefault();
          first.focus();
        } else if (event.shiftKey && document.activeElement === first) {
          event.preventDefault();
          last.focus();
        }
      }

      function moveSelected(dx, dy) {
        if (!state.selected || state.locked) {
          return;
        }
        var step = state.snap ? M.GRID : 0.005;
        var element = M.find(state.layout, state.selected);
        if (!element) {
          return;
        }
        var x = element.x + dx * step;
        var y = element.y + dy * step;
        if (state.snap) {
          x = M.snapValue(x, M.GRID);
          y = M.snapValue(y, M.GRID);
        }
        if (Math.abs(x - element.x) < 0.00001 && Math.abs(y - element.y) < 0.00001) {
          return;
        }
        pushUndo();
        state.layout = M.move(state.layout, state.selected, x - element.x, y - element.y, ratio());
        paint();
        paintPanels();
      }

      function resizeSelected(direction) {
        if (!state.selected || state.locked) {
          return false;
        }
        var before = M.find(state.layout, state.selected);
        var next = M.resize(state.layout, state.selected, direction > 0 ? 1.06 : 1 / 1.06, ratio());
        var after = M.find(next, state.selected);
        if (!before || !after ||
            (before.x === after.x && before.y === after.y && before.size === after.size && before.width === after.width)) {
          return false; // already at the smallest or largest the screen allows
        }
        pushUndo();
        state.layout = next;
        paint();
        paintPanels();
        return true;
      }

      function canvasOrChipFocused() {
        var active = document.activeElement;
        return active === canvas || !!(active && active.classList && active.classList.contains("vp-chip"));
      }

      function onKeyDown(event) {
        if (closed) {
          return;
        }
        // A confirmation owns the keys while it is open (nav.js closes it with Esc/B).
        if (GT4.ui.topDialog()) {
          return;
        }
        if (event.key === "Escape") {
          event.preventDefault();
          event.stopPropagation();
          requestClose();
          return;
        }
        if (event.key === "Tab") {
          trapTab(event);
          return;
        }
        if (event.key === "q" || event.key === "Q" || event.key === "e" || event.key === "E") {
          // The Settings tabs must not switch behind the editor.
          event.stopPropagation();
          return;
        }
        if (event.key === "[" || event.key === "]") {
          if (!state.selected || !canvasOrChipFocused()) {
            return;
          }
          event.preventDefault();
          event.stopPropagation();
          resizeSelected(event.key === "]" ? 1 : -1);
          return;
        }
        var arrows = { ArrowLeft: [-1, 0], ArrowRight: [1, 0], ArrowUp: [0, -1], ArrowDown: [0, 1] };
        var direction = arrows[event.key];
        if (!direction || !state.selected) {
          return;
        }
        if (!canvasOrChipFocused()) {
          return;
        }
        event.preventDefault();
        event.stopPropagation();
        if (event.shiftKey && direction[0] !== 0) {
          resizeSelected(direction[0]);
          return;
        }
        moveSelected(direction[0], direction[1]);
      }

      document.addEventListener("keydown", onKeyDown, true);

      // ---- hints and hooks ----
      function setHints() {
        GT4.ui.setHints([
          { key: "A", label: t("vpad.hint.select"), run: function () { GT4.nav.activate(); } },
          { key: "B", label: t("common.back"), run: function () { GT4.back(); } },
          { key: "LB RB", label: t("vpad.hint.resize"), run: function () { resizeSelected(1); } },
          { key: "X", label: t("vpad.undo"), run: undo },
          { key: "Y", label: t("vpad.reset_default"), run: resetLayout }
        ]);
      }

      function refreshHints() {
        setHints();
      }

      function requestClose() {
        if (state.saving) {
          return;
        }
        if (!isDirty() || state.locked) {
          close(false);
          return;
        }
        GT4.ui.openDialog({
          title: t("vpad.discard_title"),
          text: t("vpad.discard_text"),
          buttons: [
            { label: t("common.cancel") },
            { label: t("vpad.discard"), danger: true, run: function () { close(false); } }
          ],
          onClose: function () { if (!closed) { refreshHints(); } }
        });
        setHints();
      }

      function close(saved) {
        if (closed) {
          return;
        }
        closed = true;
        GT4.virtualPadEditor.current = null;
        document.removeEventListener("keydown", onKeyDown, true);
        root.removeEventListener("resize", fitCanvas);
        editor.remove();
        if (appNode) {
          appNode.removeAttribute("inert");
        }
        GT4.hooks = previousHooks;
        GT4.focusScope = previousFocusScope;
        if (options.onClosed) {
          options.onClosed(saved);
        }
        var target = options.returnFocus;
        if (target && document.contains(target)) {
          target.focus({ preventScroll: true });
        } else {
          GT4.ui.setHints([]);
        }
      }

      function save() {
        if (state.locked || state.saving) {
          return;
        }
        var problem = M.problems(state.layout)[0];
        if (problem) {
          GT4.ui.toast(t("vpad.warn_cannot_save", { reason: t("vpad.problem." + problem.code) }), true);
          return;
        }
        state.saving = true;
        paintStatus();
        GT4.backend.call("save_virtual_pad", { layout: state.layout }).then(function (next) {
          state.saving = false;
          if (closed) {
            return;
          }
          if (options.onSaved) {
            options.onSaved(next);
          }
          close(true);
          GT4.ui.toast(t("vpad.saved"));
        }, function (error) {
          state.saving = false;
          paintStatus();
          GT4.ui.toast(GT4.errorText(error), true);
        });
      }

      GT4.hooks = {
        back: function () {
          if (state.selected && document.activeElement === canvas) {
            var chip = chipFor(state.selected);
            if (chip) {
              chip.focus({ preventScroll: true });
              return;
            }
          }
          requestClose();
        },
        // The keyboard arrow keys move the selected element; on the pad the D-pad keeps moving the focus (A on a chip
        // grabs the element), and the left stick below moves it. A D-pad press on the canvas goes back to the chips.
        vertical: function (delta) {
          if (document.activeElement === canvas) {
            var chip = chipFor(state.selected);
            if (chip) {
              chip.focus({ preventScroll: true });
              return true;
            }
          }
          if (GT4.inputMode() !== "keys" || !canvasOrChipFocused()) {
            return false;
          }
          moveSelected(0, delta);
          return true;
        },
        horizontal: function (delta) {
          if (document.activeElement === canvas) {
            var chip = chipFor(state.selected);
            if (chip) {
              chip.focus({ preventScroll: true });
              return true;
            }
          }
          if (GT4.inputMode() !== "keys" || !canvasOrChipFocused()) {
            return false;
          }
          moveSelected(delta, 0);
          return true;
        },
        tabs: function (delta) {
          resizeSelected(delta);
          return true;
        },
        x: undo,
        y: resetLayout
      };
      GT4.focusScope = function () {
        return editor;
      };

      // The pad's left stick moves the selected element: a nicer way than the D-pad for a big button on a phone.
      GT4.backend.listen("pad-live", function (payload) {
        if (closed || !state.selected || state.locked || GT4.ui.topDialog()) {
          return;
        }
        var left = payload && payload.sticks && payload.sticks.left;
        var x = left ? Number(left.x) || 0 : 0;
        var y = left ? Number(left.y) || 0 : 0;
        if (Math.abs(x) < 0.2 && Math.abs(y) < 0.2) {
          if (stickActive) {
            stickActive = false;
            paint();
            paintPanels();
          }
          return;
        }
        if (!stickActive) {
          pushUndo();
          stickActive = true;
        }
        state.layout = M.move(state.layout, state.selected, x * 0.008, y * 0.008, ratio());
        paintElements();
        paintWarning();
        paintStatus();
      });

      // ---- first paint ----
      select(state.layout.elements.length ? state.layout.elements[0].id : null);
      paintPanels();
      paint();
      setHints();
      root.addEventListener("resize", fitCanvas);
      root.requestAnimationFrame(function () {
        fitCanvas();
        paintHandle();
      });
      // A pad or keyboard player starts on the selected button's chip (the D-pad walks them, A grabs the element);
      // a mouse or touch player starts on the canvas, where the drag happens.
      var firstChip = chipFor(state.selected);
      if (firstChip && GT4.inputMode() !== "mouse") {
        firstChip.focus({ preventScroll: true });
      } else {
        canvas.focus({ preventScroll: true });
      }
      // `handle` above is the resize grip in the canvas; this is the caller's handle of the open editor.
      var api = { close: function () { close(false); }, editor: editor, state: state };
      // The dev checks and the screenshots read the open editor's state (the page never needs it).
      GT4.virtualPadEditor.current = api;
      return api;
    }
  };
})(window);
