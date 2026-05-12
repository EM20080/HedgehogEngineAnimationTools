# Hedgehog Engine Animation Tools

Hedgehog Engine Animation Tools (HEAT) is a **Blender addon** for importing and exporting Hedgehog Engine animation files. It supports Havok skeletons and animations, plus PXD skeletons and animations.

## Features

- Import/export Havok skeletons: `.skl.hkx`
- Import/export Havok animations: `.anm.hkx`
- Optional spline-compressed HKX animation export
- Import/export PXD skeletons: `.skl.pxd`
- Import/export compressed PXD animations: `.anm.pxd`
- Root motion import/export support where available

Batch exporting is not supported.

## Compatibility

HEAT can import tons of HKX files, including :
- Sonic Unleashed (Wii/PS2/Xbox 360/PS3)
- Sonic Generations (PC/Xbox 360/PS3/Remaster)
- Sonic Forces



- HKX export is limited to :

- Sonic Generations (PC/Xbox 360/PS3)
- Sonic Unleashed (Xbox 360/PS3)
- Sonic Lost World (PC/Wii U)

## Install

Install the packaged addon zip from Blender:

1. Open Blender.
2. Go to `Edit > Preferences > Add-ons`.
3. Use `Install from Disk`.
4. Pick `HedgehogEngineAnimationTools.zip`.
5. Enable `Hedgehog Engine Animation Tools`.

The addon appears under:

```text
File > Import/Export > Hedgehog Engine Animation
```

## Important Skeleton Note

When importing animations, import the matching skeleton with HEAT and use that armature. Do not use the armature that came from a `.model` import. Animation tracks, bone order, rest pose, and orientation need to match the real skeleton file.

Skeletons imported through ModelFBX can work for previewing, but animations exported from them can appear flipped sideways in-game. Use a skeleton imported with HEAT for animation export; due to time constraints, ModelFBX skeleton export issues are not being fixed right now.

If the model already has an armature from a `.model` import, clear the model parent, delete that old skeleton, and run `File > Clean Up > Purge Unused Data` before importing the real skeleton with HEAT. This matters because Blender can otherwise import the HEAT skeleton with a `.001` name, which can break animation matching. After cleanup, import the skeleton with HEAT and parent the model to that imported skeleton.

## Basic Workflow

1. Import the skeleton first with HEAT.
2. Select the imported skeleton armature.
3. Parent the model to that skeleton, then import the matching animation.
4. Export edited skeletons or animations through the HEAT export menu.

For PXD files, keep the YX orientation option consistent. If a skeleton was imported with YX orientation, use the matching YX option for animation import/export and skeleton export.

## Credits

PXD support builds on work by @AdelQue and @WistfulHopes. Havok handling uses an external fork of [HavokLib](https://github.com/PredatorCZ/HavokLib). Related third-party libraries are included under `extern/` and `licenses/`.
