# VirtTilt

A small Windows 11 tray tool. Tilting the mouse wheel left or right switches to the previous or next virtual desktop.

- Single exe of about 100 KB with no dependencies (static CRT).
- About 1.3 MB private memory. The CPU is idle except when you tilt the wheel.

## Build

```powershell
.\build.ps1          # -> build\Release\VirtTilt.exe
```

You need Visual Studio with the C++ workload. The script uses the CMake that ships with VS.

To install, run this from your own terminal:

```powershell
.\install.ps1        # builds, copies to %LOCALAPPDATA%\Programs\VirtTilt, enables autostart, starts it
```

Diagnostics: run `VirtTilt.exe --log` to log mouse, tray and hook events to `%TEMP%\VirtTilt.log`.

## Usage

Run `VirtTilt.exe`. On the first run it adds itself to autostart. Click the tray icon to open the menu:

| Menu item | Effect |
|---|---|
| Enabled | Pause or resume. When paused, the mouse hook is fully removed. |
| Reverse direction | Tilt right switches to the previous desktop instead of the next. |
| Block horizontal scrolling | Stops the tilt from also scrolling sideways in apps. |
| Desktop switch animation | Enables smooth slide animation during desktop transition (API mode). |
| Double-tilt speed | Submenu to adjust debounce: Fast (150 ms), Normal (180 ms), Relaxed (220 ms), Safe (260 ms). |
| Switch via API (works in RDP) | Switches desktops directly via Windows Shell COM API. Bypasses RDP/fullscreen key capture. |
| Switch via Keyboard | Injects Ctrl+Win+Left/Right keystrokes. |
| Start with Windows | Adds or removes the `HKCU\...\Run` entry. |
| Exit | Closes the tool. |

## Settings

Settings are stored in `HKCU\Software\VirtTilt`:

| Value | Default | Meaning |
|---|---|---|
| `Enabled` | 1 | Turns the tool on or off. |
| `Invert` | 0 | Reverses the direction. |
| `BlockHorizontalScroll` | 1 | Blocks horizontal scrolling. |
| `Animate` | 1 | 1 = smooth slide animation, 0 = instant switch. |
| `UseApi` | 1 | 1 = Windows Shell COM API (works in RDP), 0 = keystrokes. |
| `GestureGapMs` | 180 | Cooldown time in ms between consecutive same-direction tilts. Blocks hardware tilt release (~105 ms) while allowing deliberate rapid double clicks (~220 ms). |

## Notes & Limitations

- **API Mode (Default)**: Directly instructs Explorer to switch virtual desktops. It works even when full-screen Remote Desktop (RDP), VMs, or elevated administrator windows are active and capturing keyboard shortcuts.
- **Keyboard Mode**: Injects Ctrl+Win+Arrow. When Remote Desktop is fullscreen, keys may be captured by the remote session unless focus is diverted.
- Some vendor drivers (Logitech Options or G Hub, for example) remap tilt to other actions. Set tilt back to "horizontal scroll" in the driver.
- Two-finger horizontal scrolling on a precision touchpad may also trigger a switch.
