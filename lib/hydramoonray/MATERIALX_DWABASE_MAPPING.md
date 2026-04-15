# MaterialX To DwaBase Mapping (HdMoonray)

This translator targets `ND_standard_surface_surfaceshader` and MaterialX image/tiling chains.

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

Image and UV chain notes:
- `image`/`tiledimage` are bridged to `ImageMap`.
- `file -> texture`, `uvtiling -> scale`, `uvoffset -> offset`.
- `texcoord/st/in` relationship bindings are remapped to `ImageMap.input_texture_coordinates` and force `texture_coordinates = input texture coordinates`.
- `transform2d` is bridged to `UsdTransform2d`.
- `texcoord`/`geompropvalue`/`primvarreader_float2` style nodes are bridged to `UsdPrimvarReader_float2`.

Known approximations and gaps:
- Scalar-opacity to Dwa `presence` uses RGB average.
- Channel extraction for `ImageMap` is limited: alpha channel uses `alpha_only`; isolated `r/g/b` extraction is not directly supported.
- MaterialX frame controls and filter type are currently logged as unsupported for `ImageMap`.
