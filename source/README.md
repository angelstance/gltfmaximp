# gltfmaximp

Native `3ds Max 2026` scene importer project for `.gltf` and `.glb`.

## What It Does

- Builds as a native `.dli` `SceneImport` plugin
- Imports static mesh geometry
- Preserves scene hierarchy using real Max nodes
- Imports base color materials and textures
- Imports node animation for `translation`, `rotation`, and `scale`
- Supports `TRIANGLES`, `TRIANGLE_STRIP`, and `TRIANGLE_FAN`

## Current Limits

- Imports only the first animation clip in the file
- Imports object/node `TRS` animation only in this first version
- Does **not** yet import skinning / bones / weighted mesh deformation
- Does **not** yet import morph target animation
- Does **not** yet import advanced PBR material conversion
- Does **not** yet import renderer-specific materials

## Files

- `gltfmaximp.cpp`: native importer implementation
- `gltfmaximp.vcxproj`: Visual Studio project targeting the `3ds Max 2026 SDK`
- `gltfmaximp.def`: exported Max plugin entry points
- `gltfmaximp.rc`: version/resource strings
- `vendor/`: vendored single-header third-party parsing dependencies

## Build

1. Open `gltfmaximp.vcxproj` in Visual Studio.
2. Retarget the project if Visual Studio asks for a different Windows SDK.
3. Build `Release|x64`.
4. The output plugin is:

```text
build\Release\gltfmaximp.dli
```

## Install

1. Copy `gltfmaximp.dli` into a `3ds Max` plugin path.
2. Start `3ds Max`.
3. Use `File > Import` and choose a `.gltf` or `.glb` file.

## Notes

- Embedded textures are written out to a sibling folder next to the source file:

```text
<model_name>_gltf_textures
```

- The importer currently targets the coordinate conversion used in the original Python tool: glTF `Y-up` to Max `Z-up`
