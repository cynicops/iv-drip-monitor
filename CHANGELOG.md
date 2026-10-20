# Changelog

## 1.0.1 — 2026-10-21

### Fixed
- **Alarm buzzer silenced after ~24.8 days of uptime.** `muteUntil` started at 0, and the code treated "`millis() − muteUntil` is negative" as muted. Once `millis()` passed 2³¹ ms, that test became true without anyone pressing MUTE, so every alarm was silent and the screen showed `(mute)`. Mute is now tracked with a `muted` flag (`startMute()` / `isMuted()`). The test suite found this bug (`longrun/buzzer_works_after_25_days`).

### Added
- Host-side test suite (75 tests), mutation check, ESP32 compile check and GitHub Actions CI.
- Documentation: setup, hardware, theory of operation, test report and bench checklist.

## 1.0.0 — 2026-10-12
- First ESP32 + 0.96" OLED version: IR drop counting with ambient-light cancelling, flow-rate calculation, STOPPED / TOO FAST / TOO SLOW alarms, buzzer, buttons, Telegram alerts and commands, calibration mode.
