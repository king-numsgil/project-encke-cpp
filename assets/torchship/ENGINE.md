# TorchShip export: engine conventions

Companion to `SCREENS.md` (display surfaces, view domes, seat placements). Files in this folder:

| file | contents |
|---|---|
| `torchship.gltf` + `.bin` + `textures/` | exterior: hull, hull doors, turrets |
| `torchship_interior.gltf` + `.bin` + `textures/` | interior: per-deck shells, props, hatches, screens, seat placements |
| `seat/control_seat.gltf` | the bridge control seat, its own asset (see `SCREENS.md` section 3) |
| `bake/` | raw intermediate bake maps, **not needed by the engine** |

## Coordinates

- glTF 2.0, **Y-up, 1 unit = 1 m**. Both ship files share one ship frame: load them with the same transform.
- `+Y` = bow, the thrust direction ("up" under acceleration; decks stack along +Y).
- `+Z` = docking-port side, `+X` = cargo-door side, `-Z` = torpedo-tube side.
- Deck floor heights (glTF Y): Drive 5.95, D1 10.4, D2 13.4, D3 16.2, D4 19.0, D5 22.0, D6 25.0, Avionics 28.0.

## Moving parts (joint extras)

Doors, hatches and the bridge view domes are separate nodes whose **origin is the real pivot**. Extras:

| extra | meaning |
|---|---|
| `joint` | `"hinge"` or `"slide"` |
| `axis` | rotation or slide axis, **node-local**, glTF Y-up |
| `open_deg` / `open_m` | the part is **modelled closed**; opening rotates / slides it by this much |
| `close_deg` / `close_m` | the part is **modelled open**; closing rotates / slides it by this much |

Both conventions are in use: the hull doors carry `open_*` (modelled closed), the interior pressure hatches carry
`close_*` (modelled open). Always read which key is present. Interior parts also carry
`export_kind: "interactive"`. The view domes use `open_deg` for their burn end of travel and add `follows`; see
`SCREENS.md`.

Staging and sequencing (which hatch opens before which, interlocks) are not in the files; that is engine logic.
One ordering is forced by the geometry: the **cargo door's lower leaf carries a sealing lip behind the upper leaf**,
so the upper leaf must open first and close last (the lip would otherwise sweep through it). The docking doors'
centre seal rides on the right leaf behind the left one and needs no ordering.

## Structure

- **Exterior:** one merged hull mesh, hull doors as child nodes, and four turrets as node chains:
  `Turret_<name>` (mount) → `Turret_<name>_Base` (traverse) → `Turret_<name>_Gun` (elevation).
  The turrets carry **no joint extras or limits yet**; the node split is there for the engine to rotate the base
  and the gun separately.
- **Interior:** root `TorchShip_Interior`, one child per deck (`Deck_Drive`, `Deck_D1` … `Deck_D6`,
  `Deck_Avionics`). Each deck holds:
  - `Shell_<deck>`: all static geometry of that deck merged, with its own texture set. Natural culling cells.
  - Props: instanced (several nodes share one mesh), all in one shared props texture set.
  - Interactive parts, screens (`SCREENS.md`), and seat placement nodes.

## Materials and textures

- Every material is **one-sided** (`doubleSided: false`), and faces hidden inside solids were removed at export,
  so back-face culling is safe everywhere.
- Standard glTF PBR: base colour, packed occlusion-roughness-metallic (R = AO, G = roughness, B = metallic),
  a tangent-space normal map with exported tangents, and emission with `KHR_materials_emissive_strength`.
- **One UV set per mesh** (`TEXCOORD_0`).
- Texture sizes: interior shells and the props set are 2K each; the exterior hull set is a shared 4K; turrets 2K.
  PNG for now; KTX2 is planned later.
- The export log warning "More than one shader node tex image used for a texture" is harmless: it only means
  one sampler is shared.

## Physics shapes (Jolt)

Extra mesh nodes, **never rendered** (no material, no UVs). Filter them out of rendering by name prefix. Each
node's mesh vertices are the shape, in node-local space; extras say how to build it:

| extra | shape |
|---|---|
| `physics: "convex_hull"` | `ConvexHullShape` from the mesh vertices (at most 64 per hull) |
| `physics: "box"`, `half_extents: [x, y, z]` | `BoxShape` centred on the node origin, glTF axes |

**Exterior world, `COL_*` in `torchship.gltf`:**
- `COL_Hull_NN`: the static hull, doors closed, as convex slabs 0.35 m tall stacked along +Y. Each slab is the
  convex hull of the real geometry in its height band, so tapers and protrusions stay tight. Children of the root,
  so one static compound body.
- `COL_Turret_<name>_Base` / `_Gun`: boxes parented to the turret's base and gun nodes; they move with the turret.
- `COL_Door_*` / `COL_Hatch_EVA`: boxes parented to each door node; they move with the door.

**Interior world, `VOL_*` in `torchship_interior.gltf`:** convex volumes that together enclose the whole
interior, for the handover between the two Jolt worlds (a body inside any `VOL_` is in the interior world).
- `VOL_Drive`, `VOL_D1`, `VOL_D2_D4`, `VOL_Shoulder`, `VOL_D5_D6`, `VOL_Avionics`: one per deck zone, following the
  outside of the wall liners, stacked without gaps (the shoulder and avionics crown are tapered hulls).
- `VOL_Door_Cargo`, `VOL_Door_Docking`, `VOL_Hatch_EVA`: boxes through each walkable hull door, reaching past the
  outer face of the closed door leaves, so a body walking in switches worlds before it meets the exterior hull
  collider (which is solid across the doorways). Torpedo tubes have none.

The interior's own collision (walls, props, doors) is not included: build it from the render meshes or ask for
simple shapes.

## Not in the files yet

- Portals and occlusion volumes.
- Turret joint limits.
- The view domes' ingress/egress "rest pose".
