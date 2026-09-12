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


The terrain instance's transform
--------------------------------

In a v2 zone, instance 0 is the terrain: it names the `.TER` mesh and carries
that mesh's translation, rotation and scale like any other instance. As
originally written, `eqglib` did not place it.

`ParseZone` skips instance 0 deliberately - the actor-creating branch is guarded
on `header->version == 1 || instanceId > 0` - and read it only for its baked
lighting. The terrain geometry arrives separately, through `ParseTerrain` on the
`EQGT` chunk, which built it with `CreateTerrain()` and `InitFromEQGData()`;
neither took a placement, so the terrain landed at the origin unrotated while
every object around it was placed by its own instance transform.

This is not a corner case. Of the 265 shipped zones, 223 give their terrain a
non-zero position, 20 a scale other than 1.0, and 4 a rotation beyond the
`rx = -90` degrees the exporter emits almost everywhere.

`ParseTerrain` now reads instance 0 from the `.zon` it already holds for the
lighting block, checks that it references a `.TER` mesh, builds the transform
with the same `translate * quat(orientation) * scale` composition and `.yzx`
swizzles `ParseZone` uses for actors, and passes it to `InitFromEQGData`, which
applies it to positions and normals. The terrain's bounding box is now taken
from the placed vertices; it was previously built from the raw, unswizzled
`.TER` positions, which put it in a different frame from `m_vertices` (the WLD
path had always used the final vertex).

Worth checking visually against a zone with a large terrain offset before
trusting regenerated meshes.
