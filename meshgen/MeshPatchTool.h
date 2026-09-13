//
// MeshPatchTool.h
//

#pragma once

#include "meshgen/NavMeshTool.h"
#include "common/NavMeshData.h"

#include <cstdint>

// Tool to lay patches of geometry into the collision mesh, for the places the generator
// cannot produce connected mesh on its own. Two clicks: one on each side of the gap.

class MeshPatchToolState;
struct MeshPatch;

class MeshPatchTool : public Tool
{
public:
	MeshPatchTool();
	~MeshPatchTool() override;

	ToolType type() const override { return ToolType::MESH_PATCH; }
	void init(NavMeshTool* meshTool) override;
	void reset() override;
	void handleMenu() override;
	void handleClick(const glm::vec3& p, bool shift) override;

private:
	NavMeshTool* m_meshTool = nullptr;
	MeshPatchToolState* m_state = nullptr;
};

class MeshPatchToolState : public ToolState
{
	friend class MeshPatchTool;

public:
	void init(NavMeshTool* meshTool) override;
	void reset() override;
	void handleRender() override;
	void handleUpdate(const float dt) override {}

	std::vector<dtTileRef> handlePatchClick(const glm::vec3& p, bool shift);

	// Creates a solid block from the currently entered position and size. Returns the tiles
	// that need rebuilding, empty if the block would be degenerate.
	std::vector<dtTileRef> addBlock();

	// The default width only makes sense once the agent radius is known, and there is no
	// navmesh yet when the tool is constructed.
	void initWidthFromConfig();

	// Builds the quad for a patch spanning from one point to another. Kept separate so the
	// width can be changed after the fact without re-clicking.
	static std::vector<glm::vec3> BuildPatchRing(const glm::vec3& from, const glm::vec3& to, float width);

private:
	// Puts a new patch's geometry into the collision mesh and returns the tiles to rebuild.
	std::vector<dtTileRef> commitPatch(MeshPatch* patch);

	NavMeshTool* m_meshTool = nullptr;

	bool m_hitPosSet = false;
	glm::vec3 m_hitPos;

	// Width of the strip laid between the two clicked points. Erosion takes agentRadius off
	// each side, so anything at or below 2 * agentRadius leaves no walkable surface at all;
	// the default is the narrowest useful value plus a little, since the tool is mostly used
	// to define a narrow path rather than to floor an area. Set from the config on first use.
	float m_width = 12.0f;
	bool m_widthInitialized = false;

	// Block creator, in EQ coordinates to match what the tester tool shows and what the user
	// reads off in game. The position is the corner, not the center.
	glm::vec3 m_blockPos{ 0.0f, 0.0f, 0.0f };
	glm::vec3 m_blockSize{ 20.0f, 20.0f, 4.0f };

	uint32_t m_currentPatchId = 0;
};
