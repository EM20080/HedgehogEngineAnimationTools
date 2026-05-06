# Hedgehog Engine Animation Tools

Blender import/export addon for Hedgehog Engine skeleton and animation files from Havok all the way to PXD.

## Build

```powershell
cmake --preset windows-x64-release
cmake --build --preset windows-x64-release
```

You can find the init_py and the compiled DLL in:

```text
out/build/windows-x64-release/src
```


## Package

```powershell
cmake --build --preset windows-x64-release-package
```

An Installable zip is is compressed in:

```text
out/build/windows-x64-release/HedgehogEngineAnimationTools.zip
```
