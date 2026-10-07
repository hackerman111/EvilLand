<div align="center">

<img src="./assets/header.svg" width="640" alt="Hyprland">

# EvilLand

**A Hyprland fork with custom screen sharing and focus controls.**

Dynamic tiling · Wayland · Custom window rules

[![Hyprland fork](https://img.shields.io/badge/Hyprland-fork-58c4e8?style=for-the-badge)](https://github.com/hyprwm/Hyprland)
![Wayland](https://img.shields.io/badge/Display-Wayland-9b8afb?style=for-the-badge)
![C++](https://img.shields.io/badge/C%2B%2B-26-58c4e8?style=for-the-badge)
[![License](https://img.shields.io/badge/License-BSD--3--Clause-9b8afb?style=for-the-badge)](./LICENSE)

**[Fork features](#what-this-fork-adds) · [Build](#build-from-source) · [Configuration](#configuration) · [Gallery](#gallery) · [Upstream](https://github.com/hyprwm/Hyprland)**

</div>

---

## Hyprland, with local changes

EvilLand is a fork of **[Hyprland](https://github.com/hyprwm/Hyprland)**, the independent dynamic tiling Wayland compositor. It keeps Hyprland's desktop, rendering, plugins, and IPC, and adds optional controls for screen capture and window focus.

This repository contains the compositor source and fork-specific changes. The executable remains **`Hyprland`**, configuration uses **`hyprland.lua`**, and **`hyprctl`** remains the control interface. For the official project and its releases, visit upstream Hyprland.

## What this fork adds

All three properties are opt-in and disabled by default.

| Property                  | Behavior                                                                                                                                                                   | Reference                                              |
| ------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------ |
| `hide_from_screen_share`  | Excludes windows or layers from capture. For windows on the same workspace and monitor, input goes to the hidden window while the previous active window, IPC title, and focus history are kept. | [Capture and focus](./FOCUS_GUARD.md) |
| `preserve_previous_focus` | Keeps the previous native Wayland client's keyboard focus notifications and activated state while delivering input to a private window. Requires `hide_from_screen_share`. | [Focus implementation](./src/managers/SeatManager.cpp) |
| `focus_guard`             | Holds the active native Wayland window's focus and geometry, blocks workspace and monitor switches, and allows input to an eligible private floating window.               | [Usage and limitations](./FOCUS_GUARD.md)              |

Preserving client focus notifications and `focus_guard` require native Wayland clients. Keeping the previous active window in IPC is independent of the client backend. See the [focus guide](./FOCUS_GUARD.md) for supported interactions and limitations. Integration coverage lives in [hyprtester](./hyprtester/src/tests/clients/private-focus.cpp); capture rule coverage lives in [unit tests](./tests/desktop/rule/ScreenShare.cpp).

## The Hyprland desktop

| Windows & workspaces                                   | Appearance & integration                                   |
| ------------------------------------------------------ | ---------------------------------------------------------- |
| Tiling, floating, pseudotiling, and fullscreen windows | Gradient borders, blur, shadows, and animations            |
| Dwindle, Master, Scrolling, and Monocle layouts        | Custom bezier and spring animation curves                  |
| Dynamic and special workspaces, window groups          | Configuration reloads when saved                           |
| Window, monitor, and layer rules                       | Plugins and the built-in `hyprpm` manager                  |
| Per-workspace layouts and custom layouts               | Socket-based IPC, global shortcuts, and native IME support |

## Build from source

Install the build dependencies described in the [Hyprland installation guide](https://wiki.hypr.land/getting-started/installation/). This checkout requires a compiler with **C++26** support; dependency requirements are defined in [CMakeLists.txt](./CMakeLists.txt).

```sh
git clone --recurse-submodules https://github.com/hackerman111/EvilLand.git
cd EvilLand
cmake -S . -B build/evilland -DCMAKE_BUILD_TYPE=Release
cmake --build build/evilland --parallel
```

For an existing checkout, initialize dependencies with `git submodule update --init --recursive` before configuring. The compositor binary is `build/evilland/Hyprland`.

Fork-specific properties require this fork's binary. After installing a new build, restart the compositor session; reloading configuration only updates settings.

## Configuration

Start with the repository's [example Lua configuration](./example/hyprland.lua), the [Hyprland configuration wiki](https://wiki.hypr.land/configuring/), and the [getting started guide](https://wiki.hypr.land/getting-started/master-tutorial/).

To exclude a floating terminal from compositor screen sharing:

```lua
hl.window_rule({
    name = "private-terminal",
    match = { class = "^private-terminal$" },
    float = true,
    hide_from_screen_share = true,
})
```

Launch it with `kitty --class private-terminal`. For focus controls and their interactions, read [FOCUS_GUARD.md](./FOCUS_GUARD.md).

</details>

## Credits & license

Hyprland is created by **vaxerski and the [Hyprland contributors](https://github.com/hyprwm/Hyprland/graphs/contributors)**. EvilLand builds on their work and retains the [BSD 3-Clause license](./LICENSE).

Upstream acknowledgements: [wlroots](https://gitlab.freedesktop.org/wlroots/wlroots), [tinywl](https://gitlab.freedesktop.org/wlroots/wlroots/-/blob/master/tinywl/tinywl.c), [Sway](https://github.com/swaywm/sway), [Vivarium](https://github.com/inclement/vivarium), [dwl](https://codeberg.org/dwl/dwl), and [Wayfire](https://github.com/WayfireWM/wayfire).

For upstream contributions, follow Hyprland's [AI policy](https://github.com/hyprwm/.github/blob/main/policies/AI_USAGE.md) and [issue guidelines](https://wiki.hypr.land/contributing-and-debugging/issue-guidelines/). Upstream GitHub interactions are the user's responsibility; violating these policies can result in an organization ban.
