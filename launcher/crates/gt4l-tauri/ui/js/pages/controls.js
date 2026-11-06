/*
 * Controls tab: the DualShock 2 drawing (controller-svg.js) as the hero, live input from the controller and the
 * keyboard, the XInput control or key beside each DS2 button, and a side panel with the selected button and the
 * analog settings. Every change is written at once (no Save).
 *
 * Commands (ui/dev/README.md):
 *   get_controls()                          -> {controls: [{id, name, binding, sources, xinput, key, is_default}],
 *                                               triggers: [row], locked}
 *   capture_binding({control})              -> {control, source}   (the next new press on the active pad, SDL3)
 *   cancel_capture()                        -> ends a capture
 *   pad_status()                            -> the last pad-status payload (name, kind, count...)
 *   assign_binding({control, source, swap}) -> the get_controls view plus {swapped_with, unchanged}
 *   reset_binding({control})                -> one button back to its default
 *   reset_all_bindings()                    -> the get_controls view, everything back to defaults
 *   set_setting({id, value})                -> analog rows: l2_deadzone, l2_curve, r2_*, lstick_deadzone, rstick_deadzone
 * Events (Rust pad service): pad-status (on change), pad-live (about 60 Hz while a pad is connected).
 * Keyboard view: the fixed modern layout (GT4.controlsModel.KEYBOARD_LAYOUT), read only; rebinding is off for keys.
 *
 * Navigation (two levels, js/focus-levels.js): A on the tab list enters on the last selected button (Cross at
 * first). Inside, the D-pad moves between DS2 buttons by position and the selection follows focus. A rebinds, Y resets
 * the button, X resets everything (after a confirmation), B returns to the tab list. Drawing the page never moves focus.
 * Mouse and touch: a click or tap selects the nearest button; a second one (or Rebind) starts the rebind.
 */
(function (root) {
  "use strict";
  var GT4 = (root.GT4 = root.GT4 || {});
  var h = GT4.ui.h;
  var M = GT4.controlsModel;
  // The names of the buttons and actions in the current language (TASK-102). The model keeps the English names.
  function nameOf(id) {
    var key = "controls.button." + id;
    return GT4.i18n.has(key) ? GT4.t(key) : id;
  }
  function titleOf(id) {
    return nameOf(id);
  }
  var ACTION_KEYS = {
    "Nitro": "controls.action.nitro",
    "Gear up": "controls.action.gear_up",
    "Gear down": "controls.action.gear_down",
    "Look back": "controls.action.look_back",
    "Camera": "controls.action.camera",
    "Pause": "controls.action.pause"
  };
  function actionName(name) {
    return ACTION_KEYS[name] ? GT4.t(ACTION_KEYS[name]) : name;
  }


  var CAPTURE_SECONDS = 5;
  var remembered = { selected: "cross", mode: "xinput" }; // kept while the app runs

  function call(command, args) {
    return GT4.backend.call(command, args);
  }

  // An HTML chip for a source (panel, legend, dialogs): the same look as the chips in the drawing.
  function chip(source) {
    var info = M.label(source);
    return h("span", { class: "xchip kind-" + info.kind, title: info.long }, info.text);
  }

  function legendChip(kind, label) {
    return h("span", { class: "xchip kind-" + kind }, label);
  }

  function svgNode(tag, attrs, parent) {
    var node = document.createElementNS("http://www.w3.org/2000/svg", tag);
    Object.keys(attrs).forEach(function (key) {
      node.setAttribute(key, String(attrs[key]));
    });
    if (parent) {
      parent.appendChild(node);
    }
    return node;
  }

  GT4.tabPages.controls = function (content, host) {
    content.classList.add("is-controls");
    var dev = GT4.devControls || {};
    var state = {
      controls: [],
      analog: [],
      analogKey: "",
      locked: false,
      selected: dev.select || remembered.selected,
      mode: dev.mode || remembered.mode,
      capture: null
    };

    var pad = GT4.controllerSvg({ onPick: onPick });

    // ---- the chip mode switch (header) ----
    var modeButtons = {};
    var seg = h("div", { class: "cx-seg", role: "group", "aria-label": GT4.t("controls.show_for") },
      [["xinput", GT4.t("controls.view_controller")], ["key", GT4.t("controls.view_keyboard")]].map(function (pair) {
        var button = h("button", {
          class: "cx-seg-btn",
          type: "button",
          "aria-pressed": "false",
          data: { mode: pair[0] },
          on: { click: function () { setMode(pair[0]); } }
        }, pair[1]);
        modeButtons[pair[0]] = button;
        return button;
      }));
    // ---- header: title, the honest note, controller status ----
    var statusName = h("span", { class: "cx-status-name", text: GT4.t("controls.no_controller") });
    var statusSub = h("span", { class: "cx-status-sub", text: "" });
    var status = h("div", { class: "cx-status", role: "status" }, [
      h("span", { class: "dot", "aria-hidden": "true" }),
      h("span", { class: "cx-status-text" }, [statusName, statusSub])
    ]);
    var tipText = GT4.t("controls.tip");
    var info = h("button", {
      class: "info",
      type: "button",
      "aria-label": GT4.t("controls.about_saving"),
      "aria-expanded": "false",
      on: { click: function () { GT4.ui.toggleTip(info); } }
    }, ["i", h("span", { class: "tip", role: "tooltip" }, [h("b", { text: GT4.t("controls.title") }), tipText])]);
    var lockedNote = h("p", { class: "cx-locked", hidden: true, text: "" });
    content.appendChild(h("div", { class: "cx-head" }, [
      h("div", { class: "cx-title" }, [
        h("h1", { text: GT4.t("controls.title") }),
        h("p", { class: "note" }, [h("span", { text: GT4.t("controls.saves_at_once") }), info])
      ]),
      h("div", { class: "cx-head-right" }, [seg, status])
    ]));
    // TASK-148: which controller the game reads (the `controller` setting), from the pads the Rust service sees.
    var pickBox = h("div", { class: "cx-pick" });
    var pickNote = h("p", { class: "cx-pick-note", hidden: true, text: "" });
    content.appendChild(h("div", { class: "cx-pick-row" }, [pickBox, pickNote]));
    content.appendChild(lockedNote);

    var legend = h("div", { class: "cx-legend" });
    var stage = h("div", { class: "cx-stage" }, [
      h("div", { class: "cx-pad" }, [pad.svg]),
      h("div", { class: "cx-under" }, [legend])
    ]);

    // ---- side panel: the selected button ----
    var cardGlyph = h("span", { class: "cx-glyph" });
    var cardName = h("div", { class: "cx-name" });
    var cardState = h("div", { class: "cx-state" });
    var padChipSlot = h("span", { class: "cx-src-v" });
    var keyChipSlot = h("span", { class: "cx-src-v" });
    var rebindButton = h("button", { class: "btn primary small", type: "button", on: { click: function () { rebind(state.selected); } } }, GT4.t("controls.rebind"));
    var resetButton = h("button", { class: "btn small", type: "button", on: { click: function () { resetOne(state.selected); } } }, GT4.t("controls.reset"));
    var cardHint = h("p", { class: "cx-hint", text: "" });
    var card = h("section", { class: "cx-card" }, [
      h("div", { class: "cx-card-head" }, [cardGlyph, h("div", { class: "cx-card-title" }, [cardName, cardState])]),
      h("div", { class: "cx-src" }, [h("span", { class: "cx-src-k", text: GT4.t("controls.view_controller") }), padChipSlot]),
      h("div", { class: "cx-src" }, [h("span", { class: "cx-src-k", text: GT4.t("controls.view_keyboard") }), keyChipSlot]),
      h("div", { class: "cx-actions" }, [rebindButton, resetButton]),
      cardHint
    ]);

    // ---- side panel: analog settings with live meters ----
    var meters = {};
    var analogRows = {};
    function triggerBlock(id, title) {
      var raw = h("i", { class: "raw" });
      var out = h("i", { class: "out" });
      var dz = h("b", { class: "dz" });
      var value = h("span", { class: "cx-meter-v", text: "0%" });
      // Reserve the widest text the meter can show (100%, the whole byte) plus a margin, so the bar keeps its size.
      var widest = GT4.t("controls.meter_value", { percent: 100, pressure: 255, full: 255 });
      value.style.minWidth = (widest.length + 1) + "ch";
      meters[id] = { raw: raw, out: out, dz: dz, value: value };
      analogRows[id] = h("div", { class: "cx-rows" });
      return h("div", { class: "cx-block" }, [
        h("div", { class: "cx-block-head" }, [
          h("span", { class: "cx-block-name", text: title }),
          h("span", { class: "cx-meter", "aria-hidden": "true", title: GT4.t("controls.meter_help") }, [dz, raw, out]),
          value
        ]),
        analogRows[id]
      ]);
    }
    function stickBlock(id, title) {
      var scope = svgNode("svg", { viewBox: "-50 -50 100 100", class: "cx-scope", "aria-hidden": "true" });
      svgNode("circle", { class: "ring", cx: 0, cy: 0, r: 44 }, scope);
      svgNode("path", { class: "axis", d: "M-44 0 H44 M0 -44 V44" }, scope);
      var zone = svgNode("rect", { class: "zone", x: -10, y: -10, width: 20, height: 20, rx: 2 }, scope);
      var dot = svgNode("circle", { class: "dot", cx: 0, cy: 0, r: 7 }, scope);
      meters[id] = { zone: zone, dot: dot };
      analogRows[id] = h("div", { class: "cx-rows" });
      return h("div", { class: "cx-block cx-block-stick" }, [
        h("div", { class: "cx-block-head" }, [h("span", { class: "cx-block-name", text: title }), scope]),
        analogRows[id]
      ]);
    }
    var resetAllButton = h("button", { class: "btn danger small cx-reset-all", type: "button", on: { click: confirmResetAll } }, GT4.t("controls.reset_all_button"));
    var panel = h("aside", { class: "cx-panel", "aria-label": GT4.t("controls.panel_label") }, [
      card,
      h("section", { class: "cx-analog" }, [
        h("h2", null, [GT4.t("controls.triggers"), h("small", { text: GT4.t("controls.game_reads") })]),
        analogRows.triggers = h("div", { class: "cx-rows" }),
        triggerBlock("l2", "L2"),
        triggerBlock("r2", "R2"),
        h("h2", { text: GT4.t("controls.sticks") }),
        stickBlock("lstick", GT4.t("controls.left_stick")),
        stickBlock("rstick", GT4.t("controls.right_stick"))
      ]),
      resetAllButton
    ]);
    content.appendChild(h("div", { class: "cx-body" }, [stage, panel]));

    // ---- TASK-185: the virtual controller (Android, and the dev mock), with its layout editor ----
    // The row the game reads through GT4_VIRTUAL_PAD (Auto / Always / Off, default Auto); the layout itself lives in
    // `user/virtual-pad.json` and is edited full screen by js/pages/virtual-pad-editor.js. Drawn above the analog
    // settings so a phone shows it without scrolling. Hidden when the backend says this platform has no on-screen pad.
    var vpad = null;
    if (GT4.features.virtualPad) {
      vpad = buildVirtualPad();
    }

    function buildVirtualPad() {
      var view = null;
      var modeSlot = h("div", { class: "cx-vpad-mode" });
      var status = h("p", { class: "cx-vpad-status", role: "status", text: "" });
      var rows = h("div", { class: "cx-vpad-rows" });
      var editButton = h("button", {
        class: "btn primary small", type: "button", data: { action: "vpad-edit" },
        on: { click: openEditor }
      }, GT4.t("vpad.edit"));
      var resetButton = h("button", {
        class: "btn small", type: "button", data: { action: "vpad-reset" },
        on: { click: confirmReset }
      }, GT4.t("vpad.reset_default"));
      var tipText = GT4.t("vpad.section_help");
      var info = h("button", {
        class: "info", type: "button", "aria-label": GT4.t("vpad.section_title"), "aria-expanded": "false",
        on: { click: function () { GT4.ui.toggleTip(info); } }
      }, ["i", h("span", { class: "tip", role: "tooltip" }, [h("b", { text: GT4.t("vpad.section_title") }), tipText])]);
      var section = h("section", { class: "cx-vpad", "aria-labelledby": "cx-vpad-title" }, [
        h("h2", { id: "cx-vpad-title" }, [GT4.t("vpad.section_title"), info]),
        status,
        h("div", { class: "cx-vpad-mode-row" }, [h("span", { class: "cx-vpad-k", text: GT4.t("vpad.mode") }), modeSlot]),
        rows,
        h("div", { class: "cx-vpad-actions" }, [editButton, resetButton])
      ]);
      panel.insertBefore(section, panel.querySelector(".cx-analog"));

      function paintStatus() {
        if (!view) {
          status.textContent = "";
          return;
        }
        var warn = !!view.locked || (view.notes && view.notes.length > 0);
        if (view.locked) {
          status.textContent = GT4.t("vpad.locked");
        } else if (view.notes && view.notes.length) {
          status.textContent = GT4.t("vpad.notes");
          status.title = view.notes[0];
        } else {
          status.textContent = view.source === "player" ? GT4.t("vpad.source_player") : GT4.t("vpad.source_default");
          status.title = view.path || "";
        }
        status.classList.toggle("is-warning", warn);
      }

      function paintMode() {
        GT4.ui.clear(modeSlot);
        if (!view) {
          return;
        }
        modeSlot.appendChild(GT4.optionSelector({
          id: "virtual_pad",
          label: GT4.t("vpad.mode"),
          options: [
            { value: "auto", text: GT4.t("choice.virtual_pad.auto") },
            { value: "always", text: GT4.t("choice.virtual_pad.always") },
            { value: "off", text: GT4.t("choice.virtual_pad.off") }
          ],
          current: view.mode,
          disabled: !!view.locked,
          onChange: setMode
        }));
      }

      function paintRows() {
        var focused = rows.contains(document.activeElement) ? document.activeElement.getAttribute("data-setting") : null;
        GT4.ui.clear(rows);
        if (!view) {
          return;
        }
        var context = {
          table: { rows: [] },
          content: rows,
          selfDrawn: {},
          commit: function (id, value) { return commitRow(id, value); },
          refresh: function () { paintRows(); return Promise.resolve(); }
        };
        [
          {
            id: "virtual_pad_opacity", control: "slider", label: GT4.t("vpad.global_opacity"),
            values: { min: 5, max: 100, step: 5, unit: "%" },
            current: Math.round(view.layout.opacity * 100), disabled: !!view.locked
          },
          {
            id: "virtual_pad_haptics", control: "toggle", label: GT4.t("vpad.haptics"),
            values: null, current: view.layout.haptics !== false, disabled: !!view.locked
          }
        ].forEach(function (row) {
          var node = GT4.settingsRows.rowNode(context, row);
          if (node) {
            rows.appendChild(node);
          }
        });
        if (focused) {
          var again = rows.querySelector("[data-setting=\"" + focused + "\"]");
          if (again) {
            again.focus({ preventScroll: true });
          }
        }
      }

      function paint() {
        paintStatus();
        paintMode();
        paintRows();
        editButton.disabled = !view || !!view.locked;
        resetButton.disabled = !view || !!view.locked || !view.has_file;
      }

      function applyView(next) {
        view = next || null;
        paint();
      }

      function setMode(value) {
        if (!view) {
          return;
        }
        call("set_setting", { id: "virtual_pad", value: value }).then(function () {
          view.mode = value;
        }, function (error) {
          GT4.ui.toast(GT4.errorText(error), true);
          paintMode();
        });
      }

      // A slider or a toggle on the section writes the whole layout at once (the same file the game reads).
      function commitRow(id, value) {
        if (!view || view.locked) {
          return Promise.resolve();
        }
        var layout = JSON.parse(JSON.stringify(view.layout));
        if (id === "virtual_pad_opacity") {
          layout.opacity = Math.round(Number(value)) / 100;
        } else if (id === "virtual_pad_haptics") {
          layout.haptics = !!value;
        }
        return call("save_virtual_pad", { layout: layout }).then(applyView, function (error) {
          GT4.ui.toast(GT4.errorText(error), true);
          paintRows();
        });
      }

      function load() {
        return call("get_virtual_pad").then(applyView, function (error) {
          GT4.ui.toast(GT4.errorText(error), true);
        });
      }

      function openEditor() {
        if (!view || view.locked) {
          return;
        }
        GT4.virtualPadEditor.open({
          view: view,
          returnFocus: editButton,
          onClosed: function () { load(); }
        });
      }

      function confirmReset() {
        if (!view || view.locked) {
          return;
        }
        GT4.ui.openDialog({
          title: GT4.t("vpad.reset_title"),
          text: GT4.t("vpad.reset_text"),
          buttons: [
            { label: GT4.t("common.cancel") },
            {
              label: GT4.t("vpad.reset_default"), primary: true, danger: true,
              run: function () {
                call("reset_virtual_pad").then(function (next) {
                  applyView(next);
                  GT4.ui.toast(GT4.t("vpad.reset_done"));
                }, function (error) {
                  GT4.ui.toast(GT4.errorText(error), true);
                });
              }
            }
          ],
          onClose: function () { root.setTimeout(paintHints, 0); }
        });
        GT4.ui.setHints([
          { key: "A", label: GT4.t("controls.choose"), run: function () { GT4.nav.activate(); } },
          { key: "B", label: GT4.t("common.cancel"), run: function () { GT4.back(); } }
        ]);
      }

      return { load: load };
    }

    var vpadReady = vpad ? vpad.load() : Promise.resolve();

    function findControl(id) {
      return state.controls.filter(function (control) { return control.id === id; })[0] || null;
    }

    function firstOf(control, family) {
      return control ? (control.sources || []).filter(function (source) { return M.family(source) === family; })[0] || null : null;
    }

    // ---- painting ----
    function paintCard() {
      var id = state.selected;
      var control = findControl(id);
      GT4.ui.clear(cardGlyph);
      cardGlyph.appendChild(GT4.controllerSvg.glyph(id));
      cardName.textContent = nameOf(id);
      cardState.textContent = control ? (control.is_default ? GT4.t("controls.default") : GT4.t("controls.changed")) : "";
      cardState.classList.toggle("is-changed", !!control && control.is_default === false);
      var padSource = firstOf(control, "xinput");
      GT4.ui.clear(padChipSlot);
      padChipSlot.appendChild(chip(padSource));
      padChipSlot.appendChild(h("span", { class: "cx-src-t", text: M.label(padSource).long }));
      // The keyboard layout is fixed in this build: every key of the button, read only.
      var keyNames = M.KEYBOARD_LAYOUT[id] || [];
      GT4.ui.clear(keyChipSlot);
      keyNames.forEach(function (name) {
        keyChipSlot.appendChild(chip("key:" + name));
      });
      keyChipSlot.appendChild(h("span", { class: "cx-src-t", text: keyNames.length ? GT4.t("controls.fixed_layout") : GT4.t("controls.not_in_layout") }));
      var keysMode = state.mode === "key";
      rebindButton.disabled = state.locked || keysMode;
      resetButton.disabled = state.locked || keysMode || !control || control.is_default;
      var pointer = GT4.inputMode && GT4.inputMode() === "mouse";
      if (keysMode) {
        cardHint.textContent = GT4.t("controls.keyboard_fixed");
      } else {
        cardHint.textContent = state.locked ? "" : (pointer ? GT4.t("controls.click_again") : GT4.t("controls.press_a"));
      }
    }

    function paintLegend() {
      GT4.ui.clear(legend);
      if (state.mode === "key") {
        var actions = M.KEYBOARD_ACTIONS.map(function (action) {
          return action.name + " " + action.keys.join(" or ") + " (" + action.button + ")";
        }).join(", ");
        legend.appendChild(h("span", { class: "cx-legend-note", text: GT4.t("controls.legend_fixed") + " " +
          "" + GT4.t("controls.in_a_race", { actions: actions }) }));
        return;
      }
      [
        [["face", "A"], ["face", "B"], ["face", "X"], ["face", "Y"], GT4.t("controls.group_buttons")],
        [["bumper", "LB"], ["bumper", "RB"], GT4.t("controls.group_bumpers")],
        [["trigger", "LT"], ["trigger", "RT"], GT4.t("controls.group_triggers_pressure")],
        [["thumb", "LS"], ["thumb", "RS"], GT4.t("controls.group_stick_clicks")],
        [["menu", "Back"], ["menu", "Start"], GT4.t("controls.group_menu")]
      ].forEach(function (group) {
        var chips = group.slice(0, -1).map(function (pair) { return legendChip(pair[0], pair[1]); });
        legend.appendChild(h("span", { class: "cx-legend-item" }, chips.concat([h("span", { class: "cx-legend-t", text: group[group.length - 1] })])));
      });
    }

    function setMode(mode) {
      state.mode = mode;
      remembered.mode = mode;
      Object.keys(modeButtons).forEach(function (key) {
        modeButtons[key].setAttribute("aria-pressed", String(key === mode));
      });
      paintBadges();
      paintLegend();
      paintCard();
    }

    // The chips: the controller sources (Controller view) or the fixed keyboard layout (Keyboard view).
    function paintBadges() {
      if (state.mode === "key") {
        pad.setBadges(state.controls.map(function (control) {
          return { id: control.id, keys: M.KEYBOARD_LAYOUT[control.id] || [], is_default: true, sources: [] };
        }), "key");
      } else {
        pad.setBadges(state.controls, "xinput");
      }
    }

    function select(id) {
      if (!M.find(id)) {
        return;
      }
      state.selected = id;
      remembered.selected = id;
      pad.setSelected(id);
      paintCard();
    }

    // The analog rows use the settings row shape and its controls (slider, dropdown); set_setting writes them.
    function paintAnalog(focusId) {
      var key = JSON.stringify(state.analog) + state.locked;
      if (key === state.analogKey && !focusId) {
        return;
      }
      state.analogKey = key;
      var context = {
        table: { rows: state.analog },
        content: panel,
        selfDrawn: {},
        refresh: function (id) { return load(id); }
      };
      [["triggers", ["trigger_full_range"]], ["l2", ["l2_deadzone", "l2_curve"]], ["r2", ["r2_deadzone", "r2_curve"]],
        ["lstick", ["lstick_deadzone"]], ["rstick", ["rstick_deadzone"]]].forEach(function (pair) {
        var box = analogRows[pair[0]];
        GT4.ui.clear(box);
        pair[1].forEach(function (rowId) {
          var row = state.analog.filter(function (candidate) { return candidate.id === rowId; })[0];
          if (!row) {
            return;
          }
          var label = rowId === "trigger_full_range" ? GT4.t("setting.trigger_full_range.label")
            : rowId.indexOf("curve") > 0 ? GT4.t("controls.curve") : GT4.t("controls.deadzone");
          var node = GT4.settingsRows.rowNode(context, Object.assign({}, row, {
            label: label,
            disabled: state.locked || !!row.disabled
          }));
          if (node) {
            box.appendChild(node);
          }
        });
      });
      if (focusId && content.contains(document.activeElement)) {
        var target = panel.querySelector("[data-setting=\"" + focusId + "\"]");
        if (target) {
          target.focus({ preventScroll: true });
        }
      }
    }

    function analogFlag(id) {
      var row = state.analog.filter(function (candidate) { return candidate.id === id; })[0];
      return !!row && (row.current === true || row.current === "on");
    }

    function analogValue(id, fallback) {
      var row = state.analog.filter(function (candidate) { return candidate.id === id; })[0];
      return row && typeof row.current === "number" ? row.current / 100 : fallback;
    }

    function apply(view, focusId) {
      state.controls = view.controls || [];
      state.analog = view.triggers || [];
      state.locked = !!view.locked;
      lockedNote.hidden = !state.locked;
      lockedNote.textContent = state.locked
        ? GT4.t("controls.locked")
        : "";
      paintBadges();
      paintCard();
      paintAnalog(focusId);
    }

    var reads = GT4.settingsModel.readGate();
    function load(focusId) {
      var ticket = reads.ticket();
      return call("get_controls").then(function (view) {
        if (reads.isLatest(ticket)) {
          apply(view, focusId);
        }
      }, function (err) {
        GT4.ui.toast(GT4.errorText(err), true);
      });
    }

    // ---- live input: the pad from the Rust service (SDL3, the one the game reads) and the keyboard ----
    var keysDown = {};
    var lastStatus = "";
    var latest = { live: null, status: null };
    function paintStatus(summary) {
      var unavailable = summary.available === false;
      var connected = !!summary.connected && !unavailable;
      var key = [unavailable, connected, summary.name, summary.kind, summary.count, summary.error].join("|");
      if (key === lastStatus) {
        return;
      }
      lastStatus = key;
      status.classList.toggle("is-on", connected);
      statusName.textContent = connected ? summary.name : (unavailable ? GT4.t("controls.input_unavailable") : GT4.t("controls.no_controller"));
      if (connected) {
        statusSub.textContent = summary.count > 1 ? GT4.t("controls.connected_many", { count: summary.count }) : GT4.t("controls.connected");
      } else {
        statusSub.textContent = unavailable ? (summary.error || GT4.t("controls.sdl_not_started")) : GT4.t("controls.connect_hint");
      }
    }
    // TASK-148: the controller dropdown: the connected pads by name (their `controller` value is the pad's `key`), the one
    // the game reads selected, then the joysticks SDL3 has no mapping for, greyed out with the reason below the row. There
    // is no "Automatic" entry (owner, 2026-11-04): the pad service stores the pad it reads when the stored one is not
    // connected, so at every start a connected controller is selected. With no pad the row says so and is disabled.
    var lastPick = "";
    function paintPick(summary) {
      var pads = summary.pads || [];
      var unknown = summary.unrecognized || [];
      var active = pads.filter(function (one) { return one.active; })[0] || pads[0];
      var key = JSON.stringify([pads, unknown, state.locked]);
      if (key === lastPick) {
        return;
      }
      lastPick = key;
      var values = pads.map(function (one) {
        return { value: one.key, label: one.name };
      });
      if (!values.length) {
        values.push({ value: "none", label: GT4.t("controls.no_controller"), disabled: true });
      }
      unknown.forEach(function (one, index) {
        values.push({ value: "unmapped-" + index, label: GT4.t("controls.controller_unmapped", { name: one.name }), disabled: true });
      });
      var row = {
        id: "controller",
        control: "dropdown",
        label: GT4.t("setting.controller.label"),
        values: values,
        current: active ? active.key : "none",
        default: active ? active.key : "none",
        disabled: state.locked || !pads.length
      };
      var context = { table: { rows: [row] }, content: content, selfDrawn: {}, refresh: function () { return Promise.resolve(); } };
      var hadFocus = pickBox.contains(document.activeElement);
      GT4.ui.clear(pickBox);
      var node = GT4.settingsRows.rowNode(context, row);
      if (node) {
        pickBox.appendChild(node);
      }
      if (hadFocus) {
        var target = pickBox.querySelector("[data-setting=\"controller\"]");
        if (target) {
          target.focus({ preventScroll: true });
        }
      }
      var notes = [];
      if (unknown.length) {
        notes.push(GT4.t("controls.controller_unmapped_note", { names: unknown.map(function (one) { return one.name; }).join(", ") }));
      }
      pickNote.hidden = notes.length === 0;
      pickNote.textContent = notes.join(" ");
    }
    // Pad events from the Rust service. A page that is gone (another route) ignores them.
    GT4.backend.listen("pad-live", function (payload) {
      if (pad.svg.isConnected) {
        latest.live = payload;
      }
    });
    GT4.backend.listen("pad-status", function (payload) {
      if (pad.svg.isConnected) {
        latest.status = payload;
        if (!payload || !payload.connected) {
          latest.live = null; // no pad: nothing is pressed (pad-live stops)
        }
        paintStatus(payload || {});
        paintPick(payload || {});
      }
    });
    call("pad_status").then(function (payload) {
      latest.status = payload;
      paintStatus(payload || {});
      paintPick(payload || {});
    }, function () {});

    function paintMeters(live) {
      ["l2", "r2"].forEach(function (id) {
        var reading = live.buttons[id] || { value: 0, output: 0 };
        var meter = meters[id];
        meter.raw.style.setProperty("--w", (reading.value * 100).toFixed(1) + "%");
        meter.out.style.setProperty("--w", (reading.output * 100).toFixed(1) + "%");
        meter.dz.style.setProperty("--w", (analogValue(id + "_deadzone", M.TRIGGER_DEFAULT_DEADZONE) * 100).toFixed(1) + "%");
        // TASK-161: the percentage of the game's 100%, and the pressure byte over that 100% (45 or 84 on the game's own
        // scale, 255 with the full trigger range), so the scale the game uses shows.
        var control = findControl(id);
        var full = M.triggerFull(id, control ? control.sources : [], analogFlag("trigger_full_range"));
        meter.value.textContent = GT4.t("controls.meter_value", {
          percent: Math.round(reading.output * 100),
          pressure: Math.round(reading.output * full),
          full: full
        });
      });
      [["lstick", live.sticks.left, 0.1], ["rstick", live.sticks.right, 0.1]].forEach(function (triple) {
        var meter = meters[triple[0]];
        var dz = analogValue(triple[0] + "_deadzone", triple[2]);
        var half = Math.max(0.5, dz * 44);
        meter.zone.setAttribute("x", String(-half));
        meter.zone.setAttribute("y", String(-half));
        meter.zone.setAttribute("width", String(half * 2));
        meter.zone.setAttribute("height", String(half * 2));
        meter.dot.setAttribute("cx", String(Math.round(triple[1].x * 440) / 10));
        meter.dot.setAttribute("cy", String(Math.round(triple[1].y * 440) / 10));
        // The game reads each axis on its own: an axis inside the deadzone reads as centred.
        var outside = Math.abs(triple[1].x) > dz || Math.abs(triple[1].y) > dz;
        meter.dot.setAttribute("class", "dot" + (outside ? " is-out" : ""));
      });
    }

    function onKeyDown(event) {
      var key = M.keyFromEvent(event);
      if (key.name) {
        keysDown[key.name] = true;
      }
    }
    function onKeyUp(event) {
      var key = M.keyFromEvent(event);
      if (key.name) {
        delete keysDown[key.name];
      }
    }
    function onBlur() {
      keysDown = {};
    }
    document.addEventListener("keydown", onKeyDown, true);
    document.addEventListener("keyup", onKeyUp, true);
    root.addEventListener("blur", onBlur);

    // One reading per animation frame. Windows that produce no frames (occluded, or a headless test run) still get a
    // reading every 100 ms from the timer. Both stop when the page is gone (another tab or route).
    var lastTick = 0;
    var watchdog = null;
    function alive() {
      if (pad.svg.isConnected) {
        return true;
      }
      document.removeEventListener("keydown", onKeyDown, true);
      document.removeEventListener("keyup", onKeyUp, true);
      root.removeEventListener("blur", onBlur);
      root.clearInterval(watchdog);
      return false;
    }
    function tick() {
      lastTick = Date.now();
      paintStatus(latest.status || {});
      var live = M.liveState(latest.live, state.controls, keysDown);
      pad.setLive(live);
      paintMeters(live);
    }
    function frame() {
      if (alive()) {
        tick();
        root.requestAnimationFrame(frame);
      }
    }
    root.requestAnimationFrame(frame);
    watchdog = root.setInterval(function () {
      if (alive() && Date.now() - lastTick > 90) {
        tick();
      }
    }, 100);

    // ---- footer (content level; the tab list keeps the shell's footer) ----
    function onDrawing() {
      var active = document.activeElement;
      return !!(active && active.closest && active.closest(".pad-ctl"));
    }
    function paintHints() {
      var tabs = { key: "LB RB", label: GT4.t("common.tabs"), run: function () { if (GT4.hooks.tabs) { GT4.hooks.tabs(1); } } };
      var back = { key: "B", label: GT4.t("common.back"), run: function () { GT4.back(); } };
      var resetAll = { key: "X", label: GT4.t("controls.reset_all_hint"), run: confirmResetAll };
      if (onDrawing() || !content.contains(document.activeElement)) {
        host.hints([
          { key: "A", label: GT4.t("controls.rebind"), run: function () { rebind(state.selected); } },
          { key: "Y", label: GT4.t("controls.reset_button"), run: function () { resetOne(state.selected); } },
          resetAll, back, tabs
        ], { replace: true });
      } else {
        host.hints([
          { key: "A", label: GT4.t("controls.select_hint"), run: function () { GT4.nav.activate(); } },
          resetAll, back, tabs
        ], { replace: true });
      }
    }

    content.addEventListener("focusin", function (event) {
      var group = event.target && event.target.closest ? event.target.closest(".pad-ctl") : null;
      if (group) {
        select(group.getAttribute("data-control"));
      }
      paintHints();
    });

    // ---- picking, rebinding, conflicts ----
    function onPick(id, how) {
      if (state.capture) {
        return;
      }
      var group = pad.groups[id];
      if (how === "activate") {
        select(id);
        rebind(id);
        return;
      }
      var again = id === state.selected && document.activeElement === group;
      select(id);
      if (group && document.activeElement !== group) {
        group.focus({ preventScroll: true });
      }
      if (again) {
        rebind(id);
      }
    }

    function captureBody(id) {
      var ring = svgNode("svg", { viewBox: "0 0 100 100", class: "cx-ring", "aria-hidden": "true" });
      svgNode("circle", { class: "track", cx: 50, cy: 50, r: 46 }, ring);
      var arc = svgNode("circle", { class: "arc", cx: 50, cy: 50, r: 46, pathLength: 100, "stroke-dasharray": "100 100",
        "stroke-dashoffset": 0, transform: "rotate(-90 50 50)" }, ring);
      var seconds = h("span", { class: "cx-count", text: String(CAPTURE_SECONDS) });
      var message = h("p", { class: "cx-capture-msg", role: "status", text: "" });
      var body = h("div", { class: "cx-capture" }, [
        h("div", { class: "cx-capture-art" }, [ring, h("span", { class: "cx-capture-glyph" }, [GT4.controllerSvg.glyph(id)]), seconds]),
        h("div", { class: "cx-capture-text" }, [
          h("p", { class: "modal-text", text: GT4.t("controls.capture_text") }),
          h("p", { class: "cx-capture-sub" }, [h("span", { class: "key", text: "B" }), GT4.t("controls.capture_or"), h("span", { class: "key", text: "Esc" }), " " + GT4.t("controls.capture_cancels")]),
          message,
          h("p", { class: "cx-capture-foot", text: GT4.t("controls.capture_foot") })
        ])
      ]);
      return { body: body, arc: arc, seconds: seconds, message: message };
    }

    function captureTitle(id) {
      var button = M.find(id);
      var face = ["triangle", "square", "circle", "cross"].indexOf(id) >= 0;
      return face ? GT4.t("controls.press_for_face", { symbol: button.symbol, button: nameOf(id) }) : GT4.t("controls.press_for", { button: nameOf(id) });
    }

    function rebind(id) {
      if (state.capture || !M.find(id)) {
        return;
      }
      if (state.locked) {
        GT4.ui.toast(GT4.t("controls.file_kept"), true);
        return;
      }
      if (state.mode === "key") {
        GT4.ui.toast(GT4.t("controls.keys_fixed_rebind"));
        return;
      }
      select(id);
      var parts = captureBody(id);
      var capture = { id: id, done: false, started: Date.now(), timer: null, dialog: null };
      state.capture = capture;

      function stop(reason, detail) {
        if (capture.done) {
          return;
        }
        capture.done = true;
        root.clearInterval(capture.timer);
        GT4.capturing = false;
        GT4.cancelCapture = null;
        GT4.captureKey = null;
        state.capture = null;
        pad.setCapturing(null);
        call("cancel_capture").catch(function () {});
        capture.dialog.close();
        paintHints();
        if (reason === "timeout") {
          GT4.ui.toast(GT4.t("controls.no_button_pressed"));
        } else if (reason === "error") {
          GT4.ui.toast(GT4.errorText(detail), true);
        } else if (reason === "source" && detail !== "xinput:B") {
          // xinput:B is a cancel: the XInput capture may see B before the page's own pad poll does.
          resolveSource(id, detail);
        }
      }

      GT4.capturing = true;
      GT4.cancelCapture = function () { stop("cancel"); };
      // Keyboard rebinding is off: a key only cancels (Esc); the layout is fixed.
      GT4.captureKey = function (event) {
        if (event.repeat) {
          return true; // a key held since before the rebind
        }
        var key = M.keyFromEvent(event);
        if (key.cancel) {
          stop("cancel");
        } else {
          parts.message.textContent = GT4.t("controls.keys_fixed_capture");
        }
        return true;
      };
      pad.setCapturing(id);
      capture.dialog = GT4.ui.openDialog({
        title: captureTitle(id),
        body: parts.body,
        wide: true,
        dismissable: false,
        buttons: [{ label: GT4.t("common.cancel"), closes: false, run: function () { stop("cancel"); } }]
      });
      capture.dialog.el.classList.add("is-capture");
      GT4.ui.setHints([{ key: "B", label: "Cancel", run: function () { stop("cancel"); } }]);

      capture.timer = root.setInterval(function () {
        if (dev.capture === "hold" || dev.capture === "conflict") {
          // Dev screenshots: a still countdown.
          parts.seconds.textContent = "4";
          parts.arc.setAttribute("stroke-dashoffset", "22");
          return;
        }
        var left = Math.max(0, CAPTURE_SECONDS - (Date.now() - capture.started) / 1000);
        parts.seconds.textContent = String(Math.ceil(left));
        parts.arc.setAttribute("stroke-dashoffset", String(Math.round((1 - left / CAPTURE_SECONDS) * 1000) / 10));
        if (left <= 0) {
          stop("timeout");
        }
      }, 100);

      call("capture_binding", { control: id }).then(function (result) {
        if (result && result.source) {
          stop("source", result.source);
        }
      }, function (err) {
        if (!capture.done && String(GT4.errorText(err)).indexOf("cancel") < 0) {
          stop("error", err);
        }
      });
    }

    function sourceText(source) {
      var info = M.label(source);
      if (info.family === "key" || info.kind === "face" || info.kind === "dpad" || info.kind === "menu") {
        return info.long;
      }
      return info.long + " (" + info.text + ")";
    }

    function resolveSource(id, source) {
      var clash = M.conflict(state.controls, id, source);
      if (clash && clash.same) {
        GT4.ui.toast(GT4.t("controls.already_uses", { button: titleOf(id), source: sourceText(source) }));
        return;
      }
      if (clash && clash.holder) {
        openConflict(id, source, clash);
        return;
      }
      assign(id, source, false);
    }

    function openConflict(id, source, clash) {
      var holder = clash.holder;
      var swap = !!clash.previous;
      var what = M.family(source) === "key" ? GT4.t("controls.what_key") : GT4.t("controls.what_button");
      var text = swap
        ? GT4.t("controls.swap_text", { name: titleOf(id), source: M.label(source).text, holder: titleOf(holder), previous: M.label(clash.previous).text })
        : GT4.t("controls.move_text", { name: titleOf(id), source: M.label(source).text, holder: titleOf(holder), what: what });
      var row = function (target, incoming) {
        return h("div", { class: "cx-swap-row" }, [
          h("span", { class: "cx-swap-glyph" }, [GT4.controllerSvg.glyph(target)]),
          h("span", { class: "cx-swap-name", text: M.find(target).name }),
          h("span", { class: "cx-swap-arrow", "aria-hidden": "true", text: "←" }),
          chip(incoming)
        ]);
      };
      GT4.ui.openDialog({
        title: GT4.t("controls.already_on", { source: sourceText(source), holder: titleOf(holder) }),
        text: text,
        body: h("div", { class: "cx-swap" }, [row(id, source), row(holder, swap ? clash.previous : null)]),
        buttons: [
          { label: GT4.t("common.cancel") },
          { label: swap ? GT4.t("controls.swap") : GT4.t("controls.move"), primary: true, run: function () { assign(id, source, true); } }
        ],
        onClose: function () { root.setTimeout(paintHints, 0); }
      });
      GT4.ui.setHints([
        { key: "A", label: GT4.t("controls.choose"), run: function () { GT4.nav.activate(); } },
        { key: "B", label: GT4.t("common.cancel"), run: function () { GT4.back(); } }
      ]);
    }

    function assign(id, source, swap) {
      var ticket = reads.ticket();
      call("assign_binding", { control: id, source: source, swap: swap }).then(function (view) {
        if (reads.isLatest(ticket)) {
          apply(view);
        }
        var changed = [id];
        var message = titleOf(id) + " ← " + sourceText(source);
        if (view && view.swapped_with) {
          changed.push(view.swapped_with);
          message += GT4.t("controls.swapped_with", { button: titleOf(view.swapped_with) });
        }
        if (M.family(source) !== state.mode) {
          setMode(M.family(source));
        }
        pad.flash(changed);
        GT4.ui.toast(message);
      }, function (err) {
        GT4.ui.toast(GT4.errorText(err), true);
        load();
      });
    }

    function resetOne(id) {
      if (state.locked || state.capture) {
        return;
      }
      if (state.mode === "key") {
        GT4.ui.toast(GT4.t("controls.keys_fixed_toast"));
        return;
      }
      var control = findControl(id);
      if (control && control.is_default) {
        GT4.ui.toast(GT4.t("controls.already_default", { button: titleOf(id) }));
        return;
      }
      call("reset_binding", { control: id }).then(function () {
        return load().then(function () {
          pad.flash([id]);
          GT4.ui.toast(GT4.t("controls.back_to_default", { button: titleOf(id) }));
        });
      }, function (err) {
        GT4.ui.toast(GT4.errorText(err), true);
      });
    }

    function confirmResetAll() {
      if (state.locked || state.capture) {
        return;
      }
      GT4.ui.openDialog({
        title: GT4.t("controls.reset_all_title"),
        text: GT4.t("controls.reset_all_text"),
        buttons: [
          { label: GT4.t("common.cancel") },
          {
            label: GT4.t("controls.reset_all_hint"),
            primary: true,
            danger: true,
            run: function () {
              call("reset_all_bindings").then(function (view) {
                apply(view);
                pad.flash(M.BUTTONS.map(function (button) { return button.id; }));
                GT4.ui.toast(GT4.t("controls.reset_all_done"));
              }, function (err) {
                GT4.ui.toast(GT4.errorText(err), true);
              });
            }
          }
        ],
        onClose: function () { root.setTimeout(paintHints, 0); }
      });
      GT4.ui.setHints([
        { key: "A", label: GT4.t("controls.choose"), run: function () { GT4.nav.activate(); } },
        { key: "B", label: GT4.t("common.cancel"), run: function () { GT4.back(); } }
      ]);
    }

    host.setHooks({
      x: confirmResetAll,
      y: function () { resetOne(state.selected); }
    });
    // A enters on the last selected button (Cross at first). Drawing the page does not move focus.
    host.entry(function () { return pad.groups[state.selected] || pad.groups.cross; });

    pad.setSelected(state.selected);
    setMode(state.mode);
    paintCard();
    paintHints();
    // The virtual pad section is read beside the controls, so the first paint has both.
    return Promise.all([load(), vpadReady]).then(function () {
      if (dev.capture) {
        // Dev harness only (screenshots): start a rebind on the selected button once the page is entered.
        root.setTimeout(function () { rebind(state.selected); }, 300);
      }
    });
  };
})(window);
