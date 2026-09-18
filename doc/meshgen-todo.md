MeshGenerator / MQ2Nav To Do
======

Working notes on wanted features and known defects, with what has already been
established about each so the investigation does not have to be repeated.


Connections
-----------

### 1. Switch activated platforms

A connection needs to be able to say "click this switch, wait for a condition,
then continue". Wanted for platforms that only move when a switch is thrown.

Established:

* `ConnectionType` has only `Basic = 0`, in both `common/NavMeshData.h` and
  `NavMeshFile.proto`, and `OffMeshConnectionBuffer` carries a comment
  anticipating more types. The proto `Connection` message uses fields 1-7, so
  new optional fields are backward compatible with existing navmeshes.
* `OffMeshConnectionBuffer` sets `offMeshConId` into `params.offMeshConUserID`,
  so Detour stores the connection id as the off-mesh connection's `userId`. At
  runtime a traversed connection can be mapped back to its record.
* `NavigationPath` nodes already carry `polys[]` and `flags[]`, so the plugin
  can detect that it is entering an off-mesh poly and recover that id.
* The plugin does nothing with connections today - there is no reference to
  off-mesh anything under `plugin/`. This is new runtime machinery, not a
  modification of something existing.

Open questions: how to identify a switch durably (`door->ID` is an index that
can shift, `door->Name` may not be unique - probably store both); what the wait
should watch (switch state, own position, or a plain delay) and whether a
timeout aborts; whether the plugin clicks the switch itself or only waits.

### 2. Elevators should not be auto-clicked

`SwitchHandler::OnPulse` iterates every door and clicks any that passes
`ShouldAutoUseSwitch` within `c_triggerThreshold` (20 units). It is purely
proximity based with no notion of intent, so riding past an elevator call
button triggers it.

Fix shape: mark the switch as belonging to an elevator connection, and have
`ShouldAutoUseSwitch` consult the loaded connections and refuse it. Needs the
same connection metadata plumbing as item 1, so do this one first - it is
smaller, self contained, and stops a real misbehaviour.

### 3. Vertical connections oscillate (bug, deferred)

Vertical connections can look ahead too far, adjust the next waypoint, and get
stuck flipping back and forth. Suspected over-aggressive recalculation on
entering the connection tile, but the same behaviour appears away from
connections wherever vertical mesh tiles are close together, so the cause is
probably in path following rather than in connections.

### 4. Long range (teleporter) connections

Detour cannot represent these. See "Connection reach" below for why. Two
approaches:

* A routing layer above Detour: a `Teleport` connection is saved as metadata,
  not baked into `OffMeshConnectionBuffer`, and the plugin chooses between the
  direct path and `path(current -> pad) + cost + path(exit -> goal)`. Costs a
  teleport correctly regardless of distance, and shares its "path leg, action,
  path leg" machinery with items 1 and 2.
* Faked intermediate landings - see "Recovering walkable surface" below. Keeps
  everything inside Detour, but a 2000 unit hop needs about 26 platforms at the
  default tile size, and Detour costs by distance so the chain prices like
  walking.


Connection reach
----------------

The limit is real, not arbitrary, and it is **horizontal only**.

`OffMeshConnectionTool::UpdateConnection` marks a connection valid only when the
start and end tiles are within one of each other:

```cpp
if (std::abs(bmaxx - bminx) <= 1 && std::abs(bmaxy - bminy) <= 1)
    conn->valid = true;
```

That matches Detour exactly. `dtCreateNavMeshData` stores a connection in the
tile containing its **start** (`classifyOffMeshPoint` returns `0xff` for
inside), and `dtNavMesh::addTile` only calls `connectExtOffMeshLinks` for the
tile's own layers and `getNeighbourTilesAt` over the 8 immediate neighbours. An
endpoint two tiles away is never searched for, so its end is silently unlinked.

Reach is therefore a function of tile geometry, not a distance constant. Tile
world size is `tileSize * cellSize` (`NavMeshBuilder.cpp`), which at the default
128 x 0.6 is 76.8 units; a connection may span the 3x3 tile block around its
start, 230.4 units across. Raising `tileSize` to 256 doubles that, at the cost
of slower and coarser per-tile rebuilds.

**Vertical is unconstrained.** Tiles are full height columns - `tileBmin[1] =
bmin[1]`, `tileBmax[1] = bmax[1]`, only x and z are subdivided - and
`classifyOffMeshPoint` tests only `pt[0]` and `pt[2]`. A vertical drop of any
height stays in one tile column and is linked by
`connectExtOffMeshLinks(tile, tile, -1)`. Long vertical connections should stay
`Basic`: they already work natively with real costs, and moving them to a
routing layer would be a regression.

The one genuine vertical constraint is on endpoints, not length: both
`baseOffMeshLinks` and `connectExtOffMeshLinks` search with
`ext = { rad, walkableClimb, rad }`, so each end must sit within `walkableClimb`
of an actual polygon.

### Validity check gap

`UpdateConnection` merges the start and end tile lists before measuring their
span, so when `GetTileRefsForPoint(conn->end)` returns empty - the end is off
the mesh entirely - the span collapses to zero and the connection is marked
valid. It then fails to link at runtime with nothing said. It also never checks
the `walkableClimb` proximity above. Small, self contained fix.


Recovering walkable surface Recast will not generate
----------------------------------------------------

**Done** - the mesh patch tool (`MeshPatchTool`) covers this. Two clicks lay a
strip between the points; a block form takes a position and L/W/H. The geometry
is injected into the collision mesh, so Recast voxelizes it like any other
geometry and connections can link to it normally.

The reasoning is worth keeping, because it is what decides whether a given
problem can be fixed with an area type or needs geometry:

* `rcErodeWalkableArea` nulls **areas** on spans that are already in the compact
  heightfield, so `rcMarkConvexPolyArea` can put them back. A Ground convex
  volume rescuing a narrow ledge is doing exactly this.
* `rcFilterLedgeSpans` nulls **spans** on the solid heightfield, before
  compaction, and `rcBuildCompactHeightfield` skips `RC_NULL_AREA` spans
  entirely. The span does not exist by the time any area marking runs, so
  nothing downstream can recover it.

Steep hillsides past the walkable slope, gaps narrower than the agent, and
cracked or interpenetrating floors are the second case. Only new geometry fixes
those.

Constraints the tool has to respect, and does:

* Recast erodes by `agentRadius` on every side, and a raised strip has a drop on
  **both** sides, so each side also loses a cell to the ledge filter. The width
  default is `agentRadius * 4 + cellSize * 2` for that reason; anything narrower
  is eroded away entirely while still looking correct in the editor.
* It must lie inside the navmesh bounds or no tile covers it.

Note on extents, corrected: `ZoneCollisionMesh::addTriangle(vec3, vec3, vec3)`
does **not** filter by max extents. That filtering is on the actor path only, so
patch geometry is not silently dropped for being outside them. An earlier note
here claimed otherwise and was wrong.

Editing individual mesh vertices
--------------------------------

Long wanted, for the cases where the generator simply produces the wrong thing.
Worth being clear that it sits badly with the current pipeline:

* Tiles are opaque `dtCreateNavMeshData` blobs. Polygon vertices, the detail
  mesh, the BV tree and the links are all index coupled, so moving one vertex
  means recomputing the spatial index and detail heights or Detour will
  mis-query.
* The navmesh file stores tiles as opaque bytes, so an edit survives only until
  that tile is rebuilt - and a tile is rebuilt whenever geometry or build
  settings change, or the user hits Build Tile.

So a literal vertex editor is write-only-until-rebuild, which is a poor trade.
The same goal is better served by inputs the generator respects and that
survive a rebuild: mesh patches to add, convex volumes marked
`Unwalkable` to subtract. Worth revisiting only if there are defects those two
cannot express.


Smaller known items
-------------------

* Exclusions from the collision mesh are held in the ECS and lost on zone
  reload. Persisting them needs a stable key - the `ZoneActor_{:05}` instance
  index is the obvious candidate, and is only stable while the `.zon` is.
* The collision exclusion fade only runs through the static mesh path;
  hierarchical models are excluded correctly but do not visibly fade.
* `MGHierarchicalModel` still builds GPU buffers per instance, the duplication
  already fixed for simple models. Only matters on a zone with many animated
  models.
* Draw call volume is one draw per object per material batch, which can
  approach the bgfx limit of 65536 on a large zone. `RenderManager::EndFrame`
  warns when a frame gets close. Real instancing is the fix, and
  `MeshInstanceData` in `RenderBatch.cpp` looks set up for it but unused.
* The plugin DLL does not build on develop: upstream put `common/ZoneData`
  under `#if 0` without updating `plugin/ModelLoader.cpp`. This blocks running
  a plugin that understands anything added meshgen-side, including the High
  Avoid area type.
