# MaterialX To DwaBase Mapping (HdMoonray)

This translator is implemented as three independent layers:
- Layer 1: `standard_surface -> DwaBaseMaterial` parameter mapping
- Layer 2: `mtlximage/mtlxtiledimage -> ImageMap` native mapping
- Layer 3: UV transform policy (`UsdTransform2d`/`UVTransformMap`) kept separate

The canonical mapping table lives in:
- `lib/hydramoonray/Material.cc`
- `getStandardSurfaceMappingRules()`

That table contains all 42 standard_surface inputs and classifies each as:
- `Exact`
- `Approximate`
- `Unsupported`

Translator behavior:
- Approximate mappings emit one-time warnings per material node.
- Unsupported authored inputs emit one-time warnings per material node.
- Debug logs include summary counts (`exact`, `approximate`, `unsupported`).

Layer 2 notes (`ImageMap` bridge):
- `image`/`tiledimage` are bridged to `ImageMap`.
- Native attrs used by the bridge are intentionally limited to:
  - `texture`
  - `scale`
  - `offset`
  - `wrap_around`
  - `input_texture_coordinates`
  - `texture_coordinates`
- `file -> texture`
- `uvtiling -> scale`
- `uvoffset -> offset`
- wrap/address modes are normalized into `wrap_around`
- `texcoord/st/in` bindings are remapped to `input_texture_coordinates`
- when `input_texture_coordinates` is driven by a connection, translator forces
  `texture_coordinates = 2` (`input texture coordinates`)

Known approximations and gaps:
- Scalar-opacity to Dwa `presence` uses RGB average.
- `transmission_extra_roughness` maps approximately to
  `independent_transmission_roughness` and enables
  `use_independent_transmission_roughness`.
- Channel extraction for `ImageMap` is limited: alpha channel uses `alpha_only`; isolated `r/g/b` extraction is not directly supported.
- MaterialX frame controls, filter type, and default/fallback color are currently logged as unsupported for `ImageMap`.
- `transform2d` continues to bridge to `UsdTransform2d`; it is intentionally not
  folded into Layer 1/Layer 2 parameter mapping.
