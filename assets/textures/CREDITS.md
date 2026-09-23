# Texture credits

Every material here is from [ambientCG](https://ambientcg.com), created by
Lennart Demes and released under the
[Creative Commons CC0 1.0 Universal](https://creativecommons.org/publicdomain/zero/1.0/)
public-domain dedication. CC0 asks for no attribution; this file gives it
anyway, and records where each set came from so it can be fetched again at
another resolution.

All sets are the 1K JPG downloads, retrieved 2026-09-22. Only the maps the
renderer reads were kept: Color, NormalGL, Roughness, and AmbientOcclusion and
Metalness where the set has them. Each set's NormalDX, Displacement, preview
and scene files were left out.

| Set | Source | Maps | Tile size used |
| --- | --- | --- | --- |
| Ground110 | <https://ambientcg.com/view?id=Ground110> | colour, normal, roughness, AO | 2.1 m square |
| Concrete034 | <https://ambientcg.com/view?id=Concrete034> | colour, normal, roughness | 1.1 x 0.55 m |
| Planks037A | <https://ambientcg.com/view?id=Planks037A> | colour, normal, roughness, AO, metalness | 2.0 m square |
| PaintedMetal006 | <https://ambientcg.com/view?id=PaintedMetal006> | colour, normal, roughness, AO, metalness | 1.5 m square |
| Metal041B | <https://ambientcg.com/view?id=Metal041B> | colour, normal, roughness, metalness | 1.0 m square |
| MetalPlates013 | <https://ambientcg.com/view?id=MetalPlates013> | colour, normal, roughness, AO, metalness | 1.6 m square |

Ground110, Concrete034 and MetalPlates013 carry physical dimensions in
ambientCG's metadata and use them. The others have none, so their sizes are
judged by eye.

Download URLs follow the pattern
`https://ambientcg.com/get?file=<Set>_1K-JPG.zip`; swap `1K` for `2K`, `4K`
or `8K` for larger maps. The loader's file names include the resolution, so a
swap also needs `kResolution` in `src/render/material.cpp` changed.
