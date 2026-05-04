# gltfmaximp

A native `.gltf` and `.glb` scene importer plugin for **3ds Max 2026**.

Built as a native `.dli` SceneImport plugin using the 3ds Max SDK and [TinyGLTF](https://github.com/syoyo/tinygltf).

---

## Features

- Imports `.gltf` and `.glb` files natively via **File > Import**
- Preserves full scene hierarchy using real Max nodes
- Imports mesh geometry (triangles, triangle strips, triangle fans)
- Imports base color materials and textures
- Imports node animation (translation, rotation, scale)
- Imports morph target animation (blend shapes)
- Automatically extracts embedded textures to a sibling folder

---

## Limitations

- Imports only the **first animation clip** in the file
- Does **not** import skinning / bones / weighted mesh deformation
- Does **not** import advanced PBR material conversion (metallic, roughness, normal maps etc.)
- Does **not** import cameras or lights

---

## Installation

1. Download the latest release from the [Releases](../../releases) page
2. Copy `gltfmaximp.dli` into your 3ds Max plugin directory, e.g.:
   ```
   C:\Program Files\Autodesk\3ds Max 2026\plugins\
   ```
3. Start 3ds Max
4. Use **File > Import** and select a `.gltf` or `.glb` file

---

## Building from Source

### Requirements

- Visual Studio 2022
- 3ds Max 2026 SDK

### Steps

1. Clone the repository
2. Open `gltfmaximp.vcxproj` in Visual Studio
3. Point the project include/lib paths to your 3ds Max 2026 SDK installation
4. Build `Release|x64`
5. Output plugin will be at:
   ```
   release\gltfmaximp.dli
   ```

---

## Third-party Libraries

All vendored as single-header files — no external dependencies needed:

| Library | Purpose |
|---|---|
| [TinyGLTF](https://github.com/syoyo/tinygltf) | GLTF/GLB parsing |
| [stb_image](https://github.com/nothings/stb) | Embedded texture decoding |
| [stb_image_write](https://github.com/nothings/stb) | Texture extraction to disk |
| [nlohmann/json](https://github.com/nlohmann/json) | JSON parsing (used by TinyGLTF) |

---

## Notes

- Coordinate system conversion is handled automatically (glTF Y-up → 3ds Max Z-up)
- Embedded textures are extracted to a folder named `<model_name>_gltf_textures` next to the source file
- External texture URIs are resolved relative to the source file location

---

## License

MIT

---

## Author

[angelstance](https://github.com/angelstance)
