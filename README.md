# Collectibles Tracker for Saints Reborn

Shows **CDs / 60** and **Graffiti / 75** in a panel beside the map, using the game's own menu fonts. Optional markers show only missing locations:

- Yellow CD badges: CDs.
- Purple spray-can badges: graffiti.
- **F7** cycles **Both → CDs → Graffiti → Off**. Counts stay visible with markers off.

The minimap also shows nearby missing collectibles using the same marker mode. Icons appear only when the whole badge fits inside the radar. The icon textures use native 38×38 badge dimensions, stored without block compression; the F7 prompt uses the port's rounded keyboard-button style. The side panel scales and fades with the map's animation.

The tracker reads locations from your own game files and completion from your loaded save. It does not collect items, change progress or replace your waypoint. It is for single-player.

## Install

Close the game, copy this `CollectiblesTracker` folder into `SaintsReborn/dist/mods/`, enable **Collectibles Tracker** in Whompay's Mod Loader and restart. Open the map with **M**.

Set `markers` in `mod.ini` to choose the initial mode: `0` off, `1` CDs, `2` graffiti, `3` both. Disable the mod and restart to remove it.

Targets Saints Reborn V1.2.2 / source `b81a18a`. Check `dist/mods/wml.log` for `[Collectibles Tracker]` messages if locations or progress are unavailable.

## Build

Use a local Saints Reborn source/build installation with its WML, ReXGlue SDK and zlib build files:

```powershell
.\Build.ps1 -GameRoot "C:\Games\SaintsReborn"
.\Build.ps1 -GameRoot "C:\Games\SaintsReborn" -Test
```

To rebuild the UI atlas from the included PNGs, run `python BakeAssets.py` with Pillow installed. No game files are used by the asset baker.

Author: **kxrxh**. Built for [Saints Reborn](https://github.com/whompay/SaintsReborn) and Whompay's Mod Loader, using the ReXGlue SDK and zlib. No game files or extracted location tables are included.

Local prototype: automated checks passed. In-game checks confirmed the UI, marker modes, clean icons, panel animation and CD progress updating after collection.
