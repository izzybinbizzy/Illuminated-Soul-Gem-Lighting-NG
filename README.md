# Soul Gem Hub - SKSE plugin

Copyright (C) 2026 izzydoingit. GPL-3.0-or-later, see `LICENSE`.

The plugin of Soul Gem Hub - it is the whole mod: no plugin file, no scripts, nothing written into the
save.

## What it does
- Every filled soul gem lying in the world gets a soft light in its own color: the game's gems, Praedy's Soul Gems, Rustic
  Soul Gems, Azura's Star, the Black Star, the named quest gems and the Soul Cairn's gem bowl and chunks.
- Finds the lighting by itself (Community Shaders with or without Light Placer, ENB, Vanilla) and makes each light for it.
- Leaves every gem another mod already lights to that mod (Light Placer files, RE::Light files, gems whose mesh carries a
  light of its own), so no gem glows twice. Displays are never lit.

## Requires
SKSE and Address Library for SKSE Plugins; SKSE Menu Framework for the menu.

## Settings
SKSE Menu Framework > Soul Gem Hub, saved to `Data\SKSE\Plugins\SoulGemHub\Settings.ini`.

## Building
`build.bat` (git, xmake 3 and the Visual Studio C++ tools): CommonLib at the pinned commit, the DLL, then the off-game
tests (`tests/testgems.cpp`, the game-free maths in `src/GemMath.h`).

## Credits
The way a light is made and registered follows ReLight's, used with TrumanAE's permission.

## Third-party files
`src/SKSEMenuFramework.h` is SKSE Menu Framework's own header by Thiago Kaique, copied unchanged from
[SKSE-Menu-Framework-3-Example](https://github.com/QTR-Modding/SKSE-Menu-Framework-3-Example), MIT licensed
(see `src/SKSEMenuFramework.LICENSE.txt`).

`src/DevBenchAPI.h` and `src/DevBenchAPI.cpp` are devbench's own interface files, MIT licensed by
their author (see `src/DevBenchAPI.LICENSE.txt`) and copied unchanged. devbench is not required.
