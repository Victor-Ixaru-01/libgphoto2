# R50 On-Device Test Checklist

One pass to verify everything stacked up since the last hardware run (Phases 0–5 + the EDSDK
capability build-out: commands, event-driven update, auto-download-preview, EVF AF mode).
Run **top to bottom** — later steps assume the engine is open. The on-screen scroll view shows
the `[GP2]` log; the exact strings to look for are in parentheses. If a step fails, note the
line and move on — most are independent.

**Prep**
- [ ] `bash ios/build.sh xcframework` already run + copied to `Vendor/` (done this session).
- [ ] Build **Canon Test** to the **iPhone** (real device — the Simulator can't see USB).
- [ ] R50: powered on, **charged**, USB-C to the iPhone, lens attached, card inserted.
- [ ] Not claimed by another app (quit Photos/Image Capture/EOS Utility).

---

## 1. Connect + engine (Phase 2)
- [ ] Tap the camera row → session opens (`session opened ✓`).
- [ ] Engine opens (`✅ libgphoto2 opened the camera and parsed DeviceInfo`).
- [ ] DeviceInfo is real (`Canon.Inc / Canon EOS R50 … NNN ops`) and handshake OK (`SetRemoteMode=0x2001, SetEventMode=0x2001`).
  - Fail: `❌ gp_iccamera_new failed` → transport/vendor-fixup issue; capture the whole log.

## 2. Settings read + set (Phase 4)
- [ ] Curated panel populates (`read N settings`) with iso / shutter / aperture / whitebalance / **afmethod** / picturestyle / drivemode / meteringmode / imageformat / AE mode / exp-comp.
- [ ] Change **iso** from the app picker → `set iso = … ✓` and the value updates.
  - Fail: `set … failed (rc)` → note the name + rc (value string may not match a choice).

## 3. Event-driven update (step 3) — the key "reflects the camera" test
- [ ] Turn a dial **on the camera body** (e.g. change ISO or mode) → within ~0.5 s the app's value updates on its own (event loop → `refreshSettings`).
- [ ] Half-press the shutter on the body → expect `event: focus info (AF result)` and/or `event: camera status …`.

## 4. Full config tree (step 1)
- [ ] **Load all settings** → `loaded N settings (full config tree)` (expect ~100+).
- [ ] Change one obscure setting from the full list → applies (`set … ✓`).
- [ ] Change something on the body → full list also updates (perf-gated ≤ once/2 s).

## 5. Commands (step 2) — command bar
- [ ] **AF** → `AF drive ✓` (lens hunts focus).
- [ ] **DoF** → `DoF preview on ✓` (aperture stops down — audible/visible).
- [ ] **Flash ▲** → `popup flash ✓` (built-in flash pops, if present).
- [ ] **UI Lock** then **Unlock** → `UI lock ✓` / `UI unlock ✓` (body controls freeze/thaw).
- [ ] **Lens Near / Far** (needs live view active) → `drive lens 0x2 ✓` / `0x8002 ✓` (focus shifts).

## 5b. Roll/pitch level — discovery (electronic level)
ptp2 doesn't parse the level, so this run is to *find* how the camera sends it.
- [ ] Tap **Level: Off → On** → `roll/pitch on ✓`.
- [ ] **Tilt/rotate the camera** slowly through level → watch the log for new lines:
  - `RAW unknown EOS event 0x…, N bytes: …` (level as an unhandled event — note the code + how the bytes change with tilt), and/or
  - `PROP <hex>` that fires only while tilting (level as a device prop — note the hex code).
- [ ] Capture ~5–10 of those lines at different angles (level, tilted left, tilted forward) so the byte↔angle mapping is clear.
- [ ] Tap **Level: On → Off** when done.
  - Report the `RAW`/`PROP` lines back → I parse `EdsCameraPos` (4×int32: status/position/rolling/pitching) into a live value.

## 6. Live view (Phase 5)
- [ ] **Live View** → stream appears after a brief spin-up (first ~1 s of `0xA102` "not ready" is normal).
- [ ] **The camera's own rear screen STAYS ON** during LV (the TFT+PC fix — this was the bug).
  - Fail: screen goes dark → `EVFOutputDevice=TFT+PC failed 0x…` in the log.
- [ ] **EVF Zoom** Fit / 5× / 10× → `EVF zoom N ✓` and the feed zooms.
- [ ] **Histogram** appears under the live feed and moves as the scene brightness changes.
  - If it's **missing** while LV runs, tap **Inspect EVF** and read the `EVF records (N): type=… len=…` log — find the histogram record's size (likely 1024 or 4096) and report it so the parser's size heuristic can be corrected. Also note the channel count/order (R,G,B,Y?).
- [ ] **Stop LV** → feed stops, histogram clears, camera returns to normal; no crash.

## 7. EVF AF Mode (step 5)
- [ ] Change **afmethod** picker (Quick / Live / LiveFace / …) → `set afmethod = … ✓`; the AF-area mode changes on the camera.

## 8. Capture — preview only, **stays on camera** (Phase 3 + policy)
- [ ] Set **imageformat** to a JPEG option, then **Capture** → `✅ captured … (.jpg) … stays on camera` + inline preview.
- [ ] Confirm **nothing is written to the device** (no `saved:` line — that path was removed).
- [ ] Set imageformat to **RAW**, Capture → shot fires, no preview (CR3 can't render), file remains on the card.

## 8b. imageformat / HDR PQ (driver fix — **not yet hardware-verified**)
See `canon-imageformat-heif.md` for what each case is proving.
- [ ] HDR PQ **On** (set on the body) → set `L` / `M` / `S1` / `S2` → each `set imageformat = … ✓`.
  - Before the fix *every* one of these failed while HDR PQ was on; that was the whole bug.
- [ ] HDR PQ **On** → set the `c` variants (`cL`, `cS1`, …) → succeed. Capture → files are `.HIF`.
- [ ] HDR PQ **On** → set `RAW` alone → succeeds (this one worked before the fix too).
- [ ] HDR PQ **Off** → repeat the whole list → succeed, files are `.JPG`.
- [ ] Dual **`cRAW + L`** in both HDR PQ states → **two** files recorded, not just the CR3.
- [ ] Choice list shows `S1` before `S2` and no duplicate labels.

## 9. Auto-download-preview of body shots (step 4)
- [ ] With imageformat = **JPEG**, shoot **on the camera body** → `event: object <hex> (…bytes) — auto-downloading` then `⬇︎ previewed …-byte JPEG (stays on camera)` + inline preview. Nothing saved.
- [ ] With imageformat = **RAW**, shoot on the body → `shot on camera (fmt 0x….) — left on card` (no transfer).
- [ ] Verify the same shot isn't previewed twice (dedupe).
  - Watch: if JPEG preview fails with `getpartialobject@… failed 0x…` **and SaveTo=host**, the host/SDRAM path may need `ptp_canon_eos_getpartialobject` (see map caveat).

## 10. Keep-alive / stability
- [ ] Leave connected idle ~1–2 min with LV off → camera doesn't sleep/drop (`event: camera status … — keep-alive`).
- [ ] **Close Session** → clean teardown (`session closed`), no crash; can reconnect.

---

## Report back with
- The full `[GP2]` log for any failing step (the rc / `0x….` codes are the diagnosis).
- For §9: the `fmt 0x….` value and whether SaveTo was card or host.
- For §6: whether the rear screen stayed lit.

Results feed **step 7** in `EDSDK-CAPABILITY-MAP.md` (flip 🔨 → ✅, and resolve the ❓ items:
GPS, mode-dial disable, movie SW, focus-shift, ApertureLock, `STATUS` code meanings).
