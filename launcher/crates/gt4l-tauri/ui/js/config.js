/*
 * UI feature flags. A flag set to false removes the feature from navigation
 * (tabs, LB/RB and Q/E cycling, routes) and hides its settings rows.
 * mods: the mod manager is release 1.1.0 (owner decision, 2026-10-16).
 */
(function (root) {
  "use strict";
  var GT4 = (root.GT4 = root.GT4 || {});
  GT4.features = {
    mods: false,
    // Racing wheels (TASK-181): the Wheel page and its tab. The backend turns it off on Android (app_state.wheel).
    wheel: true,
    // Virtual controller and its layout editor (TASK-185): the backend turns it on where the game draws the pad
    // (Android; app_state.virtual_pad) and the dev mock shows it for testing.
    virtualPad: false
  };
  // Home background slideshow (six slots, in order); see ui/assets/backgrounds/README.md. Each slot has a CSS
  // class bg-slide-1..6 in css/app.css, so the list length must match the CSS slots.
  GT4.config = {
    backgrounds: [
      "assets/backgrounds/bg-01.webp",
      "assets/backgrounds/bg-02.webp",
      "assets/backgrounds/bg-03.webp",
      "assets/backgrounds/bg-04.webp",
      "assets/backgrounds/bg-05.webp",
      "assets/backgrounds/bg-06.webp"
    ]
  };
})(window);
