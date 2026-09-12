EQG `.zon` Zone Format (EQGZ v2)
======

Notes on the layout of EQG version-2 zone files, and on how MQ2Nav's loaders
currently handle them. Everything below was derived by decoding the shipped
client data and checking the result against every zone file in a Live
installation, rather than from documentation.

This covers **EQGZ** only. The v4 format (magic `EQTZP`) is a different file
and is handled separately by `eqg_v4_loader.cpp`.


File layout
-----------

A `.zon` lives either as a standalone file next to `<zone>.eqg`, or as a member
of the archive itself. `EQGLoader::Load` prefers the archived copy and falls
back to the loose file.

```
zon_header              28 bytes, packed
string table            list_length bytes
model list              model_count  x uint32
placeables              object_count x variable size   <-- see below
regions                 region_count x 40 bytes
lights                  light_count  x 32 bytes
```

There is no trailing data; the last light ends the file.

### Header

```c
struct zon_header            // 28 bytes
{
    char     magic[4];       // "EQGZ"
    uint32_t version;        // 2 for every zone currently shipped
    uint32_t list_length;    // byte size of the string table
    uint32_t model_count;
    uint32_t object_count;
    uint32_t region_count;
    uint32_t light_count;
};
```

The header is 28 bytes, not 32. Every `loc` field elsewhere in the file is a
byte offset measured from the **end of the header**, i.e. from absolute offset
`sizeof(zon_header)`. Getting this wrong shifts every string by one dword and
silently produces plausible-looking but incorrect names, so it is worth
asserting.

### String table and model list

The string table is `list_length` bytes of NUL-separated names. The model list
that follows is `model_count` uint32 offsets into it, naming the `.MOD` and
`.TER` files this zone uses. Each name resolves to a member of the companion
`.eqg` by a case-insensitive exact match.

Names carry their role as a prefix: `TER_<zone>.TER` for terrain,
`OBJ_<name>.MOD` for placed objects.

### Placeables

Each placeable is a fixed 36-byte record followed, in version 2 and later, by a
variable-length block:

```c
struct zon_placable          // 36 bytes
{
    int32_t  id;             // index into the model list
    uint32_t loc;            // offset of this instance's name
    float    x, y, z;
    float    rx, ry, rz;     // radians
    float    scale;          // uniform
};

uint32_t color_count;        // version >= 2 only
uint32_t colors[color_count];
```

`colors` is baked per-instance vertex lighting, packed `0xAARRGGBB`. Its length
is the referenced model's `vert_count`, or an exact multiple of it where a zone
stores more than one lighting set per instance. The count field is
authoritative, so a loader that only wants geometry can skip
`color_count * sizeof(uint32_t)` bytes without consulting the model.

This block is the only variable-length part of the file. Because records cannot
be indexed, the placeable array has to be walked sequentially.

### Regions and lights

Both are fixed size and parse as described by `zon_region` (40 bytes) and
`zon_light` (32 bytes) in `eqg_structs.h`. Region `extend_*` values are
half-extents about the region centre.


How this was verified
---------------------

Walking `36 + 4 + color_count * 4` bytes per placeable lands exactly on the
region block, byte for byte, in **265 of 265** zone files, with regions and
lights then decoding into sensibly named data. Across those zones:

* 429,691 placeables, none with a model id outside the model list
* 26,845 model references, all resolving to real `EQGM` / `EQGT` members
* `color_count` equal to the model's vertex count, or an exact multiple of it
* 160,298 placeables (37%) carry a scale other than 1.0

The 2x multiple on `color_count` shows up on `roost` and `xorbb` among others.

Mapping onto `eqglib`
---------------------

`eqglib/eqg_structs.h` names the same records differently, and the fields below
line up one for one with the layout above:

| Above          | `eqglib`       | Notes                                     |
| -------------- | -------------- | ----------------------------------------- |
| `zon_header`   | `SZONHeader`   | 28 bytes; `list_length` is `string_pool_size` |
| model list     | `num_meshes`   | uint32 offsets into the string pool        |
| `zon_placable` | `SZONInstance` | `mesh_id`, `name`, `translation`, `rotation`, `scale` |
| `zon_region`   | `SZONArea`     | `extents` are half-extents about `center`  |
| `zon_light`    | `SZONLight`    |                                            |

`EQGLoader::ParseZone` reads the v2 baked-lighting block correctly - it takes the
`uint32` count and then that many colours, as `litVerts`, rather than skipping
it blind - and it hands those colours to the actor it creates for each instance.
The `.TER`/`.MOD` suffix tests use `ci_ends_with`, so the empty-name underflow
that the older `zone-utilities` loader had does not exist here.


The terrain instance's transform must NOT be applied
----------------------------------------------------

In a v2 zone, instance 0 is the terrain: it names the `.TER` mesh and carries a
translation, rotation and scale in the same record shape as every other
instance. Those values are **not** a placement for the terrain. The `.TER`
vertices are already authored in world space, and `ParseZone` skipping instance
0 - the actor-creating branch is guarded on
`header->version == 1 || instanceId > 0` - is correct, not an oversight.

Applying that transform moves the terrain out from under everything else. It is
tempting to apply it: 223 of 265 shipped zones give the terrain instance a
non-zero position, and almost all of them a `rx = -90`, so it looks like a
placement being dropped. It is not.

### The measurement

Take the `.TER` vertices straight out of the archive, apply only the `.yzx`
swizzle that `InitFromEQGData` does, and compare against a known-good navmesh
built from the real zone geometry:

|                | source                        | x                     | y                    | z                     |
| -------------- | ----------------------------- | --------------------- | -------------------- | --------------------- |
| ruinedrelic    | `.TER`, swizzle only          | `-1897.26 .. 1975.77` | `-743.50 .. 866.30`  | `-1604.38 .. 2339.08` |
|                | navmesh (ground truth)        | `-1897.26 .. 1975.77` | `-581.30 .. 3531.55` | `-1604.40 .. 2339.08` |
| candlemakers   | `.TER`, swizzle only          | `-2016.33 .. 1894.62` | `-324.03 .. 563.78`  | `-2599.96 .. 2372.01` |
|                | navmesh (ground truth)        | `-1975.47 .. 1894.62` | `-324.03 .. 450.00`  | `-2599.96 .. 2000.00` |

The horizontal bounds agree to within 0.02 units with no transform applied. They
cannot agree afterwards: ruinedrelic's terrain instance would shift the mesh by
`(138.25, -200.64, -294.89)` and rotate it 90 degrees. Where the navmesh is
narrower than the terrain (candlemakers x min, z max) it is clipped by the
build's max extents; every unclipped edge matches exactly.

The vertex counts corroborate the pairing independently: instance 0's baked
lighting block is 33,508 entries for ruinedrelic and 540,889 for candlemakers,
matching each `.TER`'s vertex count exactly. Instance 0 does describe the
terrain - the loader is right to read its colours - it just does not place it.

### One real inconsistency nearby

`InitFromEQGData` stores each vertex as `inVertex.pos.yzx` but grows the
bounding box with `m_aabb.enclose(inVertex.pos)`, the unswizzled value, so
`Terrain::m_aabb` ends up in a different frame from `Terrain::m_vertices`. The
WLD path a few hundred lines above encloses the final swizzled vertex, which
looks like the intended behaviour. `m_aabb` currently feeds only
`BuildConvexHull` in `GeometryUtils.cpp`. This is left as found - noted here
rather than changed, since it is unrelated to placement and untested.


Instance rotations do not survive a component swap
--------------------------------------------------

`SZONInstance::rotation` is three Euler angles in the file's own coordinate
frame. Model vertices are stored pre-swizzled by `.yzx` (`InitFromEQGData`), so
the rotation has to be moved into that same frame before it is applied.

Permuting the Euler components is not that conversion. It gives the right answer
only when the rotations commute - when at most one axis is non-zero - because
`glm::quat(vec3)` composes its three angles in a fixed order and permuting the
components reorders them. The conversion that is always correct conjugates the
rotation by the coordinate permutation:

```
    P maps (x, y, z) -> (y, z, x)
    R_world = P * R_file * transpose(P)
```

where `R_file` is built in the file's frame as `quat(rz, ry, rx)`. The
pre-eqglib loader got this right by accident of structure: it applied
`quat(rz, ry, rx)` to unswizzled vertices and swizzled the result, which is the
same conjugation.

This is easy to miss because almost every zone rotates its instances about a
single axis, where the shortcut happens to work:

| zone         | rotation about `rx` only | all three axes |
| ------------ | ------------------------ | -------------- |
| ruinedrelic  | 273                      | 6 of 285       |
| candlemakers | 1731                     | 5136 of 7001   |

candlemakers is the zone that exposes it. `OBJ_CaveWallBoulder_b.MOD` at
`(-284.83, -81.76, 1412.71)` with rotation `(117.86, -64.61, -67.55)` degrees is
a clear case: the mesh sits well away from its instance origin, so a wrongly
composed rotation swings a wall-sized rock below the map, leaving the
stalactites above it floating and the navmesh built over geometry that is no
longer where it was.

The same caution applies to the terrain instance above, for the opposite reason:
there the transform must not be applied at all.
