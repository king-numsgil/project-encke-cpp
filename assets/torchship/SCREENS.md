# TorchShip interior screens: engine integration spec

Source file: `torchship_interior.gltf` (glTF 2.0, Y-up, metres). Every display surface is its own node with the
shared material `M_Int_Screen`: flat colour plus emission, **no textures**. Replace that material with a render
target at runtime. Screens are never baked into any texture atlas.

Two kinds of screen: **view domes** (curved, on the D6 bridge) and **flat screens** (everything else).

---

## 1. View domes (D6 bridge)

### Nodes

```
Deck_D6
├── Seat_Pilot            placement: load control_seat.gltf here (extras: placement, asset, role)
├── Seat_Tactical         same, rotated 180 deg about Y
├── Dome_Pilot            moving dome shell + trolley (hinge), follows Seat_Pilot
│   └── Dome_Pilot_Screen     the curved screen
└── Dome_Tactical         same, follows Seat_Tactical
    └── Dome_Tactical_Screen
```

Both screen nodes share one mesh. The screen node's transform is identity relative to its dome node, so the
**dome node's local frame is the screen's frame**.

### Dome motion (`Dome_*` extras)

| extra | value | meaning |
|---|---|---|
| `joint` | `"hinge"` | rotates about its node origin |
| `axis` | `[1, 0, 0]` | node-local X (the seat's pitch trunnion axis) |
| `open_deg` | `-64.0` | burn end of travel; the node is modelled at 0 (cruise) |
| `follows` | `"Seat_Pilot"` / `"Seat_Tactical"` | seat whose pitch drives it |

The dome node's origin sits exactly on the seat's pitch axis. Drive it as:

```
dome_angle_deg = clamp(seat_pitch_deg, -64, 0)
```

Seat pitch range is -64 (burn recline) to +15 (ingress). The dome stops at 0 and the seat tilts away from it for
ingress. A game-side "rest pose" that lifts the dome clear for ingress and egress is not modelled; add it on top.
The dome follows **pitch only**, not the seat's roll (±30 deg) or its 0.13 m crash stroke.

### Screen geometry (`Dome_*_Screen` extras)

| extra | value | meaning |
|---|---|---|
| `screen` | `"dome"` | kind |
| `fov_deg` | `[120, 60]` | horizontal, vertical coverage |
| `el_deg` | `[-15, 45]` | elevation of bottom and top edges, from the eye |
| `radius_m` | `0.9` | nominal eye-to-screen distance (actual surface is 0.906 m) |
| `eye` | `[0, 0.632, -0.091]` | design eye point, node-local glTF (Y-up), at the cruise pose |
| `uv` | text | reminder of the UV convention below |

The screen is a patch of a **sphere centred on the eye**. Node-local glTF frame, with the seat at cruise:

- `+Z` = straight ahead (the pilot's gaze at azimuth 0, elevation 0)
- `+Y` = up
- `+X` = the pilot's **left**

The direction from the eye for azimuth `az` (positive = pilot's left) and elevation `el`:

```
d = ( sin(az) * cos(el),   sin(el),   cos(az) * cos(el) )      // node-local glTF
```

### UV mapping (`TEXCOORD_0`)

UVs are angular, not planar. Each texel corresponds to a fixed viewing direction, in standard image orientation
(glTF: `(0, 0)` = top-left of the image):

```
s (u) = 0 at the pilot's left edge  -> 1 at the right edge        az = 60 - 120 * s     (degrees)
t (v) = 0 at the top edge           -> 1 at the bottom edge       el = 45 -  60 * t     (degrees)
```

Aspect ratio 2:1 in angle, so a 2:1 render target gives square angular texels.

**Rendering it as a window:** for each texel, compute `(az, el)` from `(s, t)`, build `d` above, and rotate it
into ship space. Then sample the view in that direction: a cube map from a forward sensor camera, or an
equirectangular render, for example. Draw HUD, radar and reticles in the same angular space. Because the screen
is a sphere around the eye, it shows no perspective distortion from the design eye point.

Optional refinement: the design eye sits at the cruise pose. The dome already tracks pitch, but roll and crash
stroke move the real head. For exact parallax, derive the live eye from the seat's joint state; for a sensor view
at infinity this doesn't matter.

---

## 2. Flat screens

Nodes named `Screen_<Deck>_<n>` under each `Deck_*` node, plus `<Prop>_Screen` children of props such as
`Neon_Tank_Aux_1_Screen` ... `_4_Screen` on D3 (these four share one mesh).

- **Node origin** = the screen's centre. Extras: `export_kind: "screen"`.
- **UVs**: planar projection onto the plane of the screen's largest face, stretched to fill 0-1 on both axes.
  `s` runs left to right and `t` runs top to bottom, as seen by someone facing the screen.
  (Horizontal screens use world X as the reference instead of up.)
- **Aspect is not normalised**: size each render target from the screen's physical width and height (mesh
  bounds in the projection plane), or the image will stretch.
- If a display mesh has faces off the main plane (bezel sides, curvature), those get stretched UVs. Only the
  front plane is meant to show content.

---

## 3. Seats

`Seat_Pilot` and `Seat_Tactical` carry no geometry. Instantiate `export/seat/control_seat.gltf` as a child with
no extra offset. The seat faces its node's `+Z`. Its own joints (pitch, roll, crash stroke, side consoles, MFD mast)
and its three MFD screens are described in that file's extras.
