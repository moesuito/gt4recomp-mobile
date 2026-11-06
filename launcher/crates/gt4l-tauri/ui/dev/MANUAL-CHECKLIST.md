# Manual checklist (launcher UI)

Run the dev page (`dev/index.html`) in a browser, then the Tauri build with the real Rust side.
Do not launch the game for these checks. Tick each item in the Tauri build before merging.

## Layout (1280 x 720, 1920 x 1080, 960 x 600 and 844 x 390 landscape)

- [ ] Setup, Home and every Settings tab fit without horizontal scroll at 960 x 600 and 844 x 390.
- [ ] At 844 x 390 nothing is clipped: the Settings nav scrolls (with a finger or the right stick) and the footer hints stay visible.
- [ ] The Play button is glass when idle, solid white with a glow when focused or hovered, and static under reduced motion.
- [ ] Settings rows: label on the left, control in one right-aligned column; sliders line up with the option selectors.
- [ ] Hover or focus on an "i" shows its help text; the card stays inside the window.
- [ ] Home: the cover fills the window, the logo sits bottom-left, Play sits under it, the gear is top-right.
- [ ] Footer hints show the right buttons per page. Home shows hints only after a pad or key is used.
- [ ] The home slideshow is unchanged: six slots, 72 s cycle, bottom-up fade.

## Keyboard

- [ ] Arrows move focus to the visually nearest control in that direction; no control is skipped.
- [ ] Left and right change a focused slider by one step and step a focused option selector, in an endless loop, with the value sliding in from that side.
- [ ] Enter on an option selector goes to its next value; the Version row opens its list and Esc closes it; Esc on a page goes Home.
- [ ] A step that changes the rows (Renderer): the value's slide does not stop, the rows that stay glide, the new ones fade in and the old ones fade out.
- [ ] Q and E switch Settings tabs in the order Graphics, Controls, Saves, Replays, About (no Mods; no Display or Advanced tab).
- [ ] Graphics shows three sections in order: "Display" (Monitor, Resolution, Fullscreen, VSync), "Graphics" (Renderer, Render scale, FXAA, CAS) and "Advanced" (then "Reset to defaults"); the D-pad moves down through all three to "Reset to defaults"; B from any of them returns to the tab list on Graphics; Y resets.
- [ ] No "Input resolution", "Widescreen 16:9", "Aspect ratio", "Custom ratio", "Internal resolution", "Show FPS", "Show GPU name" or "Show resolution" row anywhere (480p is fixed, the aspect follows the Resolution, the internal resolution follows the Render scale; PCSX2's own OSD is always off).
- [ ] Tab and Shift+Tab move through controls in document order.
- [ ] The focus ring shows after a key press and disappears after a mouse click.

## Virtual controller (TASK-185; Android, and the dev mock)

- [ ] Controls > Virtual controller: the mode steps Auto, Always and Off; the opacity and vibration rows write at once.
- [ ] Edit layout opens a full-screen editor; the Save button is disabled until something changes.
- [ ] Drag a button; pinch or drag its corner handle to resize; tap an empty spot to deselect.
- [ ] The F7 corner is drawn; moving a button onto it shows the warning line; Undo puts it back.
- [ ] The 20:9, 16:9 and 21:9 buttons reshape the canvas without changing any button.
- [ ] Save closes the editor and the section says "Your layout"; Reset to default asks first and deletes the file.
- [ ] Keyboard: Tab and Shift+Tab stay inside the editor, arrows move, Shift+arrows resize, Esc asks before losing changes.
- [ ] Pad: the D-pad and A move the focus and select, the left stick moves the selected button, LB/RB resize, B goes back.
- [ ] On Windows the section is absent (the game draws no virtual pad there).

## Mouse

- [ ] Every control responds to a click. Sliders drag and commit on release.
- [ ] Clicking a footer hint runs its action.
- [ ] Dialogs close on Cancel and on Esc; Reset to defaults asks first.

## Touch (touchscreen or touch emulation)

- [ ] Every button, the "i" marks, the option selectors' arrows, tabs and footer hints respond to a tap, and each is easy to hit (48 px).
- [ ] Tap an "i": its help opens. Tap it again, or tap anywhere else, and it closes. Nothing needs a hover.
- [ ] Drag a slider with a finger: the value follows the finger and commits on release. A vertical drag on a slider scrolls the page.
- [ ] Tap an arrow of an option selector: it steps that way; tap the value: it goes to the next one.
- [ ] Tap the Version row: its list opens; tap an entry to choose it; tap Cancel or outside to close.
- [ ] Long lists (Settings, Saves, Replays) scroll with a finger and stop at their ends (no scroll trap).
- [ ] Swipe left on Settings (over 60 px, mostly sideways) goes to the next tab; swipe right goes to the previous one. A short or diagonal drag does nothing.
- [ ] A swipe that starts on a slider or an open list does not change the tab.
- [ ] Tap a footer hint: A Select acts on the focused item, B Back goes back, LB RB Tabs switches tabs.

## Controller (XInput, WebView2)

- [ ] Settings has two levels. Opening it focuses the tab list on the current tab (Graphics by default); the content shows the highlighted tab as you move up and down.
- [ ] A, or D-pad right, on a tab enters its content on the first row, or on the row you last used in that tab (remembered for the session).
- [ ] In the content: up and down move between rows; left and right change a slider or an option selector; A goes to a selector's next value.
- [ ] B in the content goes back to the tab list on the same tab, and nothing is reset. B on the tab list goes Home.
- [ ] LB and RB change tabs from either level; from the content the focus stays in the content on the new tab.
- [ ] After the Version list or a dialog closes, focus is back on the control that opened it.
- [ ] The tab list highlight is a soft pill that glides between tabs; the tab shown while you are in the content keeps a quiet accent edge.
- [ ] A focused row has a lighter band with a thin accent edge; the focused control has a soft glow. There is no hard box.
- [ ] With Windows "Show animations" off (or reduced motion), the pill and the glow change instantly.
- [ ] Footer: the tab list shows A Enter, B Back, LB RB Tabs; the content shows A Select, B Tabs, then the tab's own keys (Graphics: Y Reset; Saves and Replays: X Import). Controls shows A Rebind, Y Reset button, X Reset all, B Back, LB RB Tabs on the drawing, and A Select on the panel. No tab shows Save.
- [ ] Mouse and touch: a click or tap focuses what was clicked; a click on a tab shows it without entering its rows.
- [ ] The D-pad and the left stick move focus; holding repeats after about 0.4 s, then about every 0.1 s.
- [ ] Stick deadzone: a slight drift does not move focus.
- [ ] A activates; B goes back (closes a dialog first); Start on Home plays.
- [ ] LB and RB switch Settings tabs; X and Y do the page action shown in the footer.
- [ ] While a rebind is waiting, only B cancels and the other buttons do nothing.
- [ ] Before the first button press the page still works with the keyboard and mouse.
- [ ] Every interactive element can be reached with the D-pad or the left stick (no control is skipped); the focus ring shows on each.
- [ ] Right stick up and down scrolls the focused scroll area (Settings content, Saves, Replays, dialogs) at a steady speed; a slight push does nothing.
- [ ] There is no Save: X does nothing on Graphics. X on Saves and Replays imports; X on Controls asks to reset all controls.
- [ ] Pad: the highlight moves over Controls, Saves and Replays and focus stays in the tab list; only A or D-pad right enters a tab. Entering Saves or Replays focuses the first row (Import only when the list is empty); entering Controls focuses the DS2 button selected last (Cross the first time).
- [ ] A changed value shows the new value at once (resolution selector, Off and On selector, slider), and it still shows after leaving and re-entering the tab.
- [ ] Y on Graphics asks "Reset to defaults?"; Y on Saves makes the selected save active; Y on Replays deletes after a confirm; Y on Controls resets the selected button.
- [ ] B closes an open "i" tip first, then an open dialog or list, then goes back.
- [ ] Left and right on a focused slider change it one step; on an option selector they step the values, and the last goes back to the first.
- [ ] Start on Home plays; A on Home plays; Y on Home opens Settings.

## Settings behaviour

- [ ] Display section (top of Graphics): Monitor lists every connected monitor by name and desktop size, the primary one marked and chosen by default; Resolution lists that monitor's modes with its desktop mode marked; picking another monitor resets Resolution to its desktop mode.
- [ ] Graphics: Render scale moves 50% to 200% in steps of 25% and its readout names the result ("100% · 4x · 2560x1792" on 2560x1080); it shows only with the PCSX2 GS.
- [ ] Changing the scaling filter to FSR shows FSR sharpness; changing CAS mode to CAS shows CAS sharpness.
- [ ] CoreGS 2.0 is greyed and not selectable.
- [ ] Graphics, Advanced section: "Reset to defaults" asks, then restores the values and says so in a toast.
- [ ] Mods is not in the nav column; LB/RB and Q/E skip it; `#/settings/mods` opens Graphics; no row has page "mods".
- [ ] An error from `set_setting` shows a red toast and the row shows the value the launcher reads back.

## Controls (needs a real XInput pad; the dev mock cannot test this)

- [ ] With the pad connected, the status pill names it and says Connected; unplugged, it says No controller. With two pads, it says "controller 1 of 2, the one the game uses".
- [ ] Press every button: the matching DS2 button lights with a glow and its chip turns white. The D-pad arms and face buttons sink a little.
- [ ] Pull LT and RT slowly: L2 and R2 fill from the bottom with the percentage, and the panel meters show the pressure the game reads (zero inside the striped deadzone). Change the curve and the deadzone and watch the meter follow.
- [ ] Move both sticks: the caps move in their wells to the real position; the panel scopes show the dot leave the deadzone square. L3 and R3 clicks light the caps.
- [ ] The keyboard lights the buttons its keys drive (Z Cross, arrows the D-pad...).
- [ ] From the tab list, A enters on Cross (later: the last selected button). The D-pad and the left stick move between DS2 buttons by position; the panel follows the selection.
- [ ] A rebinds: the overlay says "Press a button for (symbol) (Cross)..." and counts down 5 s; the selected button pulses. A pad button or a key binds; B or Esc cancels; after 5 s it ends with "No button was pressed". The A press that opened it does not bind A.
- [ ] Binding a control another button already has opens "... is already on ...": Swap exchanges them, Cancel keeps both as they were. The chips of both buttons update and flash.
- [ ] Y resets the selected button; X asks, then resets every button, trigger and stick setting.
- [ ] The Controller / Keyboard switch shows XInput chips or key chips; after a key rebind the view switches to Keyboard.
- [ ] Mouse and touch: a click or tap anywhere near a button selects it (even at 844 x 390); a second click or tap, or Rebind in the panel, starts the rebind.
- [ ] `%LOCALAPPDATA%\GT4Recomp\controls.toml` changes after every change (no Save). Remapped buttons take effect in the game only from the next game build (the "i" next to "Changes save at once" says so).

## Saves and Replays

- [ ] Empty states show the text from the mockup ("No saves yet", "No replays yet").
- [ ] X imports; a cancelled picker changes nothing.
- [ ] Y or A on a save makes it the next-play save; the "Next play" tag moves.
- [ ] Delete asks first; the list refreshes from the returned items.

## Setup

- [ ] Checking shows a progress bar that moves with `progress`.
- [ ] Wrong disc shows the red message and "Choose another file", which opens the picker.
- [ ] Done shows Ready and Play; Play starts the game and opens Home.

## Pads (SDL3 in the Rust pad service, TASK-075 launcher side)

- [ ] With no pad, Controls shows "No controller" and "Connect one, or press a button on it".
- [ ] Connect a DualShock 4, a DualSense, a Switch Pro and an Xbox pad: each shows SDL's name, and its Cross (bottom face button) lights the DS2 Cross.
- [ ] With two pads, the status says "controller 1 of 2"; the drawing follows the first one (the lowest SDL id).
- [ ] Unplug the pad on Controls: within about a second the status returns to "No controller" and nothing stays lit.
- [ ] Pad navigation: D-pad and the left stick move focus, with a first step at once and repeat after about 0.4 s; A activates, B goes back, X and Y run their page actions, LB and RB change tabs, Start is Play on Home.
- [ ] The right stick scrolls the focused list.
- [ ] Rebind: A on a DS2 button, then a pad button binds it (the name is in the controls-file form, `xinput:...`). A button held when the rebind starts does not bind. B cancels.
- [ ] Keyboard view: the fixed layout is shown (D-pad left and right: A or D and the arrows, D-pad up also E; Cross: W and Enter; Square: S; Circle: Space; Triangle: Backspace and Esc; R1 nitro: Shift; R2 gear up: R; L2 gear down: F; L1 look back: Q; Select camera: C or Tab; Start: P); Rebind and Reset are disabled there.
- [ ] Play hides the window; the game then owns the pad. When the game ends the launcher is back and the status shows the pad again.

## Known gaps (not yet built, from LAUNCHER-PLAN section 14)

- Export of saves and replays, the `.max`/`.cbs`/`.xps` importers, and the Replays "On card" choice.
- Repair game files and the open-source licences list on About.
- Background cross-fade: Home uses the single owner cover, with no rotation.
