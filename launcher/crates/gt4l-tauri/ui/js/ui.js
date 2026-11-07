/*
 * Shared DOM helpers: element builder, hint bar, toast, dialogs, dates.
 * No inline event handlers and no style attributes (CSP: style-src 'self').
 */
(function (root) {
  "use strict";
  var GT4 = (root.GT4 = root.GT4 || {});

  // h("button", {class: "x", on: {click: fn}, "aria-label": "..."}, [children])
  function h(tag, props, children) {
    var el = document.createElement(tag);
    var key;
    if (props) {
      for (key in props) {
        if (!Object.prototype.hasOwnProperty.call(props, key)) {
          continue;
        }
        var value = props[key];
        if (value === null || value === undefined || value === false) {
          continue;
        }
        if (key === "class") {
          el.className = value;
        } else if (key === "text") {
          el.textContent = String(value);
        } else if (key === "on") {
          Object.keys(value).forEach(function (name) {
            el.addEventListener(name, value[name]);
          });
        } else if (key === "data") {
          Object.keys(value).forEach(function (name) {
            el.dataset[name] = String(value[name]);
          });
        } else if (value === true) {
          el.setAttribute(key, "");
        } else {
          el.setAttribute(key, String(value));
        }
      }
    }
    append(el, children);
    return el;
  }

  function append(el, children) {
    if (children === null || children === undefined) {
      return;
    }
    if (!Array.isArray(children)) {
      children = [children];
    }
    children.forEach(function (child) {
      if (child === null || child === undefined || child === false) {
        return;
      }
      if (Array.isArray(child)) {
        append(el, child);
      } else if (child instanceof Node) {
        el.appendChild(child);
      } else {
        el.appendChild(document.createTextNode(String(child)));
      }
    });
  }

  function clear(el) {
    while (el.firstChild) {
      el.removeChild(el.firstChild);
    }
  }

  // Hint bar: [{key: "A", label: "Select", run: fn?}]. Clicks run the action
  // for mouse users; the bar itself is not focusable.
  function setHints(list) {
    var bar = document.getElementById("hints");
    if (!bar) {
      return;
    }
    clear(bar);
    (list || []).forEach(function (hint) {
      var item = h("span", { class: "hint", on: { click: function () { if (hint.run) { hint.run(); } } } }, [
        h("span", { class: "key", text: hint.key }),
        h("span", { text: hint.label })
      ]);
      bar.appendChild(item);
    });
  }

  var toastTimer = null;
  function toast(text, isError) {
    var box = document.getElementById("toast");
    if (!box) {
      return;
    }
    box.textContent = text;
    box.className = "toast is-shown" + (isError ? " is-error" : "");
    clearTimeout(toastTimer);
    toastTimer = setTimeout(function () {
      box.className = "toast";
    }, 3200);
  }

  var dialogId = 0;

  // openDialog({title, text, body: [nodes], buttons: [{label, primary, danger, run}],
  //             list: bool, dismissable: bool, onClose: fn})
  // Returns {el, close}. The top-most .modal owns navigation while open.
  function openDialog(options) {
    var previous = document.activeElement;
    dialogId += 1;
    var titleId = "dialog-title-" + dialogId;
    var closed = false;
    var overlay = h("div", { class: "modal", role: "dialog", "aria-modal": "true", "aria-labelledby": titleId }, [
      h("div", { class: "modal-box" + (options.wide ? " is-wide" : "") }, [
        h("h3", { id: titleId, text: options.title }),
        options.text ? h("p", { class: "modal-text", text: options.text }) : null,
        options.body || null,
        h("div", { class: "modal-actions" }, (options.buttons || []).map(function (button) {
          return h("button", {
            class: "btn" + (button.primary ? " primary" : "") + (button.danger ? " danger" : ""),
            "data-default": button.primary ? true : null,
            on: {
              click: function () {
                if (button.run) {
                  button.run(api);
                }
                if (button.closes !== false) {
                  api.close();
                }
              }
            }
          }, button.label);
        }))
      ])
    ]);
    var api = {
      el: overlay,
      close: function () {
        if (closed) {
          return;
        }
        closed = true;
        overlay.remove();
        if (options.onClose) {
          options.onClose();
        }
        if (previous && previous.focus && document.contains(previous)) {
          previous.focus({ preventScroll: true });
        }
      },
      // Back (B or Escape) closes the dialog unless it is not dismissable.
      back: function () {
        if (options.dismissable !== false) {
          api.close();
        }
      }
    };
    overlay.__dialog = api;
    document.body.appendChild(overlay);
    // TASK-180 test3 (owner): a dialog whose only button must not be easy to press by accident (Cancel of the shader
    // prepare) takes the focus itself: `focus: "none"` leaves no button focused, so A does nothing until the player moves
    // to one on purpose.
    var box = overlay.querySelector(".modal-box");
    if (options.focus === "none") {
      if (box) {
        box.tabIndex = -1;
        box.focus({ preventScroll: true });
      }
      return api;
    }
    var first = overlay.querySelector("[data-default]") || overlay.querySelector("button");
    if (first) {
      first.focus({ preventScroll: true });
    }
    return api;
  }

  function topDialog() {
    var modals = document.querySelectorAll(".modal");
    return modals.length ? modals[modals.length - 1].__dialog || null : null;
  }

  // ---- info cards ("i") in a top layer (TASK-199) ----------------------------------------------------------------
  // A Settings row's entrance animation (CSS .enter-item, settings.js enterAnimation) leaves a transform on the row, and
  // a transform makes every row its own stacking context: a card drawn inside its row could never paint above the rows
  // after it (owner screenshot, 2026-11-06: Graphics > "Reflexo do sol e flares das luzes" showing the next row's text
  // through it). The card is therefore moved into one layer on the body while it is open, placed from its "i" and kept
  // inside the window, and it goes back under its "i" when it closes. Every page's "i" (Settings, Controls, Wheel,
  // Achievements) draws through toggleTip, so this one path covers them all.
  //
  // A card shows while its "i" is tapped (.is-open, the state B and a tap outside clear) or while it holds the focus
  // with a keyboard or a pad (the rule the CSS used to express with `body.input-keys .info:focus .tip`). Only one
  // card is drawn at a time: a card opened by a tap keeps the screen over a focused "i" (TASK-201).
  var tipLayer = null;
  var shownTips = [];
  // TASK-201: the "i" whose card was dismissed (B, a tap outside, a tap on the card) does not open it again while
  // it keeps the focus; moving the focus anywhere else clears it. Without this, closing a card while the focus sat
  // on another row's "i" would immediately show that row's card instead.
  var dismissedInfo = null;

  function focusedInfo() {
    var active = document.activeElement;
    return active && active.closest ? active.closest(".info") : null;
  }

  // A card opened by a tap or a click keeps the screen until it is closed: moving the keyboard or pad focus to
  // another "i" must not stack a second card over it (TASK-201). The focused "i" keeps its ring, under the card.
  function tapOpenInfo() {
    return document.querySelector(".info.is-open");
  }

  function tipLayerNode() {
    if (!tipLayer) {
      tipLayer = h("div", { class: "popover-layer" });
      document.body.appendChild(tipLayer);
    }
    return tipLayer;
  }

  // The card of an "i". While the card is in the layer it is no longer a descendant of the "i", so the element is kept
  // on the button itself.
  function tipOf(info) {
    var tip = info.__infoTip || info.querySelector(".tip");
    if (tip) {
      info.__infoTip = tip;
    }
    return tip || null;
  }

  function clamp(value, low, high) {
    return Math.min(Math.max(value, low), Math.max(low, high));
  }

  // Places the card beside its "i": to the right and 8 px up, as it always was. Near the right edge it opens to the
  // left instead; near the bottom it slides up. It never leaves the window (the CSS caps its width and height too).
  function placeTip(tip) {
    var anchor = tip.__anchor;
    if (!anchor || !anchor.isConnected) {
      return;
    }
    var box = anchor.getBoundingClientRect();
    var width = tip.offsetWidth;
    var height = tip.offsetHeight;
    var gap = 8;
    var left = box.right + gap;
    if (left + width > root.innerWidth - gap) {
      left = box.left - width - gap;
    }
    left = clamp(left, gap, root.innerWidth - width - gap);
    var top = box.top - gap;
    if (top + height > root.innerHeight - gap) {
      top = root.innerHeight - height - gap;
    }
    top = clamp(top, gap, root.innerHeight - height - gap);
    tip.style.left = Math.round(left) + "px";
    tip.style.top = Math.round(top) + "px";
  }

  function showTip(info) {
    var tip = tipOf(info);
    if (!tip) {
      return;
    }
    tip.__anchor = info;
    if (!tip.classList.contains("is-shown")) {
      tip.classList.add("is-shown");
      shownTips.push(tip);
    }
    // Re-appended so the card opened last is also the one painted last inside the layer.
    var layer = tipLayerNode();
    if (layer.lastElementChild !== tip) {
      layer.appendChild(tip);
    }
    placeTip(tip);
  }

  function hideTip(info) {
    var tip = tipOf(info);
    if (!tip || !tip.classList.contains("is-shown")) {
      return;
    }
    tip.classList.remove("is-shown");
    var index = shownTips.indexOf(tip);
    if (index >= 0) {
      shownTips.splice(index, 1);
    }
    tip.style.left = "";
    tip.style.top = "";
    if (info.isConnected) {
      info.appendChild(tip);
    } else {
      tip.remove();
    }
    tip.__anchor = null;
  }

  function keysOrPad() {
    return document.body.classList.contains("input-keys") || document.body.classList.contains("input-pad");
  }

  function wantsTip(info) {
    if (info.classList.contains("is-open")) {
      return true;
    }
    if (!keysOrPad() || info !== document.activeElement) {
      return false;
    }
    return !tapOpenInfo() && info !== dismissedInfo;
  }

  // Brings every card in step with its "i", and drops a card whose row is gone (a tab change while it was open).
  function syncTips() {
    var focused = focusedInfo();
    if (dismissedInfo && dismissedInfo !== focused) {
      dismissedInfo = null;
    }
    for (var i = shownTips.length - 1; i >= 0; i -= 1) {
      var shown = shownTips[i];
      var anchor = shown.__anchor;
      if (!anchor || !anchor.isConnected) {
        shown.classList.remove("is-shown");
        shownTips.splice(i, 1);
        shown.remove();
      }
    }
    Array.prototype.forEach.call(document.querySelectorAll(".info"), function (info) {
      if (wantsTip(info)) {
        showTip(info);
      } else {
        hideTip(info);
      }
    });
  }

  function repositionTips() {
    shownTips.forEach(placeTip);
  }

  // Help tips ("i") open on tap or click and close on a second tap or a tap outside.
  // Returns true when a tip was open, so B can close it before it goes back.
  function closeTips(except) {
    var open = document.querySelectorAll(".info.is-open");
    var closed = false;
    Array.prototype.forEach.call(open, function (el) {
      if (el !== except) {
        el.classList.remove("is-open");
        el.setAttribute("aria-expanded", "false");
        closed = true;
      }
    });
    if (closed) {
      var focused = focusedInfo();
      dismissedInfo = focused && focused !== except ? focused : null;
      syncTips();
    }
    return closed;
  }

  function toggleTip(info) {
    var open = !info.classList.contains("is-open");
    closeTips(info);
    info.classList.toggle("is-open", open);
    info.setAttribute("aria-expanded", String(open));
    syncTips();
  }

  document.addEventListener("pointerdown", function (event) {
    var target = event.target;
    var closed = false;
    Array.prototype.forEach.call(document.querySelectorAll(".info.is-open"), function (el) {
      var tip = tipOf(el);
      if (!el.contains(target) && !(tip && tip.contains(target))) {
        el.classList.remove("is-open");
        el.setAttribute("aria-expanded", "false");
        closed = true;
      }
    });
    if (closed) {
      var focused = focusedInfo();
      dismissedInfo = focused && !focused.classList.contains("is-open") ? focused : null;
    }
    syncTips();
  }, true);

  // A tap on the card itself closes it, as it did while the card still lived inside its "i" (the tap reached the
  // button there and toggled it). The card is in the layer now, so this brings that back.
  document.addEventListener("click", function (event) {
    var tip = event.target && event.target.closest ? event.target.closest(".tip") : null;
    var info = tip && tip.__anchor;
    if (!info) {
      return;
    }
    info.classList.remove("is-open");
    info.setAttribute("aria-expanded", "false");
    var focused = focusedInfo();
    dismissedInfo = focused && !focused.classList.contains("is-open") ? focused : null;
    syncTips();
  }, true);

  document.addEventListener("focusin", syncTips, true);
  document.addEventListener("focusout", function () {
    // The browser moves the focus a moment later than the event; read it on the next task.
    setTimeout(syncTips, 0);
  }, true);
  document.addEventListener("keydown", syncTips, true);
  // The key itself switches the body to the keys mode (nav.js runs after this file), so the mode is read again on the
  // way up: a card whose "i" already had the focus appears on the first key press, as it did with the CSS rule.
  document.addEventListener("keyup", syncTips, true);
  root.addEventListener("resize", repositionTips, true);
  document.addEventListener("scroll", repositionTips, true);

  // A card is dropped when its row goes away (a tab change while it was open). Nothing is watched while no card is
  // open, so a page that redraws often costs nothing.
  var rowWatch = new MutationObserver(function () {
    if (shownTips.length) {
      syncTips();
    }
  });
  var appRoot = document.getElementById("app");
  if (appRoot) {
    rowWatch.observe(appRoot, { childList: true, subtree: true });
  }
  // A keyboard or a pad press shows a focused card and a mouse move hides it, so an input-mode change re-reads them.
  new MutationObserver(syncTips).observe(document.body, { attributes: true, attributeFilter: ["class"] });

  // Formats an ISO timestamp from Rust in the interface language (issue #89: the Saves dates followed en-GB, so pt-BR
  // showed English month names). The formatter is built per call, so a language change applies at once. Returns the
  // raw text when it cannot parse.
  function formatDate(iso) {
    if (!iso) {
      return "";
    }
    var date = new Date(iso);
    if (isNaN(date.getTime())) {
      return String(iso);
    }
    try {
      return new Intl.DateTimeFormat(GT4.i18n.intlLocale(), {
        day: "numeric", month: "short", year: "numeric", hour: "2-digit", minute: "2-digit"
      }).format(date);
    } catch (err) {
      return String(iso);
    }
  }

  // Keeps the page shell and the app-level hooks in one place.
  GT4.ui = {
    h: h,
    append: append,
    clear: clear,
    setHints: setHints,
    toast: toast,
    openDialog: openDialog,
    topDialog: topDialog,
    closeTips: closeTips,
    toggleTip: toggleTip,
    formatDate: formatDate
  };
})(window);
