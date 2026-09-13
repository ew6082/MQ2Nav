//
// MeshPatchTool.cpp
//

#include "MeshPatchTool.h"

#include "common/MathUtil.h"
#include "common/NavMeshData.h"
#include "meshgen/NavMeshTool.h"
#include "meshgen/ZoneProject.h"
#include "meshgen/ZoneRenderManager.h"

#include <DetourDebugDraw.h>

#include <fmt/format.h>
#include <imgui/imgui.h>
#include <imgui/misc/cpp/imgui_stdlib.h>
#include <glm/gtc/type_ptr.hpp>

MeshPatchTool::MeshPatchTool()
{
}

MeshPatchTool::~MeshPatchTool()
{
}

void MeshPatchTool::init(NavMeshTool* meshTool)
{
	m_meshTool = meshTool;

	m_state = (MeshPatchToolState*)m_meshTool->getToolState(type());
	if (!m_state)
	{
		m_state = new MeshPatchToolState();
		m_meshTool->setToolState(type(), m_state);
	}

	m_state->init(m_meshTool);
}

void MeshPatchTool::reset()
{
	m_state->reset();
}

void MeshPatchTool::handleMenu()
{
	auto navMesh = m_meshTool->GetNavMesh();
	if (!navMesh) return;

	const float agentRadius = navMesh->GetNavMeshConfig().agentRadius;

	m_state->initWidthFromConfig();

	ImGui::TextWrapped("Click a point on each side of the gap to lay a patch of geometry"
		" between them. The navmesh is rebuilt from it, so the two sides join properly"
		" instead of being bridged after the fact.");

	ImGui::Separator();

	ImGui::SliderFloat("Width", &m_state->m_width, 1.0f, 100.0f, "%.1f");

	// Recast erodes the walkable area by the agent radius on each side, so a strip narrower
	// than twice that produces no walkable surface at all - it would look like the patch had
	// simply not worked.
	const float minUsefulWidth = agentRadius * 2.0f;
	if (m_state->m_width <= minUsefulWidth)
	{
		ImGui::TextColored(ImColor(255, 128, 0),
			"Too narrow: erosion takes %.1f off each side, so nothing walkable will remain."
			" Use more than %.1f.", agentRadius, minUsefulWidth);
	}

	if (m_state->m_hitPosSet)
	{
		ImGui::TextColored(ImColor(0, 255, 0), "Click the far side of the gap.");

		if (ImGui::Button("Cancel"))
			m_state->m_hitPosSet = false;
	}

	ImGui::Separator();

	// Block creator. Two clicks cannot describe a solid, and some repairs need one: a landing
	// standing out of nothing, or a plug for a crack the floor falls through.
	if (ImGui::CollapsingHeader("Block"))
	{
		ImGui::TextWrapped("A solid box, entered rather than clicked. Position is the corner"
			" it grows from, in the same coordinates the tester tool uses.");

		ImGui::InputFloat3("Position", glm::value_ptr(m_state->m_blockPos));
		ImGui::InputFloat3("Size (L W H)", glm::value_ptr(m_state->m_blockSize));

		if (ImGui::Button("Create Block"))
		{
			std::vector<dtTileRef> modifiedTiles = m_state->addBlock();

			if (!modifiedTiles.empty())
				m_meshTool->RebuildTiles(modifiedTiles);
		}
	}

	ImGui::Separator();

	// Existing patches
	size_t count = navMesh->GetMeshPatchCount();
	ImGui::Text("%d patch%s", static_cast<int>(count), count == 1 ? "" : "es");

	uint32_t deleteId = 0;

	for (size_t i = 0; i < count; ++i)
	{
		MeshPatch* patch = navMesh->GetMeshPatch(i);

		ImGui::PushID(static_cast<int>(patch->id));

		std::string label = patch->name.empty()
			? fmt::format("{} {}", patch->type == MeshPatchType::Box ? "Block" : "Patch", patch->id)
			: patch->name;

		ImGui::TextUnformatted(label.c_str());
		ImGui::SameLine();

		if (ImGui::SmallButton("Delete"))
			deleteId = patch->id;

		ImGui::PopID();
	}

	if (deleteId != 0)
	{
		// Collect the tiles before deleting: afterwards there is no patch to ask about.
		auto modifiedTiles = navMesh->GetTilesIntersectingMeshPatch(deleteId);

		navMesh->DeleteMeshPatchById(deleteId);

		// Geometry can be appended to the collision mesh but not taken back out of it, so
		// removing a patch means regenerating the whole thing before the tiles rebuild.
		if (auto zoneProj = m_meshTool->GetZoneProj())
			zoneProj->RebuildCollisionMesh();

		if (!modifiedTiles.empty())
			m_meshTool->RebuildTiles(modifiedTiles);
	}
}

void MeshPatchTool::handleClick(const glm::vec3& p, bool shift)
{
	if (!m_meshTool) return;

	std::vector<dtTileRef> modifiedTiles = m_state->handlePatchClick(p, shift);

	if (!modifiedTiles.empty())
		m_meshTool->RebuildTiles(modifiedTiles);
}

//----------------------------------------------------------------------------

void MeshPatchToolState::init(NavMeshTool* meshTool)
{
	m_meshTool = meshTool;
}

void MeshPatchToolState::reset()
{
	m_hitPosSet = false;
	m_currentPatchId = 0;
	m_widthInitialized = false;
}

void MeshPatchToolState::initWidthFromConfig()
{
	if (m_widthInitialized)
		return;

	auto navMesh = m_meshTool->GetNavMesh();
	if (!navMesh)
		return;

	// The narrowest strip that survives erosion, plus a little slack. Patches are mostly used
	// to define a path through somewhere the generator gave up on, not to floor an area, so
	// starting wide just means overwriting geometry that did not need it.
	m_width = navMesh->GetNavMeshConfig().agentRadius * 2.0f + 2.0f;
	m_widthInitialized = true;
}

// Puts a newly added patch's geometry into the collision mesh and reports the tiles that need
// rebuilding. Recording the patch is not enough on its own: tiles are rebuilt from the
// collision mesh, so the geometry has to be in there first or nothing changes.
std::vector<dtTileRef> MeshPatchToolState::commitPatch(MeshPatch* patch)
{
	m_currentPatchId = patch->id;

	if (auto zoneProj = m_meshTool->GetZoneProj())
		zoneProj->AddMeshPatchGeometry(*patch);

	return m_meshTool->GetNavMesh()->GetTilesIntersectingMeshPatch(patch->id);
}

std::vector<dtTileRef> MeshPatchToolState::addBlock()
{
	auto navMesh = m_meshTool->GetNavMesh();
	if (!navMesh)
		return {};

	// A zero extent in any axis gives a box with no volume, which rasterizes to nothing.
	if (m_blockSize.x == 0.0f || m_blockSize.y == 0.0f || m_blockSize.z == 0.0f)
		return {};

	const glm::vec3 corner = from_eq_coord(m_blockPos);
	const glm::vec3 opposite = from_eq_coord(m_blockPos + m_blockSize);

	MeshPatch* patch = navMesh->AddMeshPatch({ corner, opposite }, std::string(),
		MeshPatchType::Box);

	return commitPatch(patch);
}

std::vector<glm::vec3> MeshPatchToolState::BuildPatchRing(
	const glm::vec3& from, const glm::vec3& to, float width)
{
	// Widen across the horizontal direction of travel. A patch bridging a gap wants to be
	// perpendicular to the crossing, and staying horizontal keeps the width honest however
	// steep the span is - widening in the plane of a steep slope would pinch it once
	// projected back down.
	glm::vec3 along = to - from;
	glm::vec3 flat{ along.x, 0.0f, along.z };

	if (glm::dot(flat, flat) < 1e-6f)
	{
		// Purely vertical: no meaningful direction of travel, so pick one arbitrarily
		// rather than produce a degenerate quad.
		flat = glm::vec3{ 1.0f, 0.0f, 0.0f };
	}

	glm::vec3 side = glm::normalize(glm::vec3{ -flat.z, 0.0f, flat.x }) * (width * 0.5f);

	return { from - side, from + side, to + side, to - side };
}

std::vector<dtTileRef> MeshPatchToolState::handlePatchClick(const glm::vec3& p, bool shift)
{
	auto navMesh = m_meshTool->GetNavMesh();
	if (!navMesh)
		return {};

	if (!m_hitPosSet)
	{
		m_hitPos = p;
		m_hitPosSet = true;

		return {};
	}

	m_hitPosSet = false;

	MeshPatch* patch = navMesh->AddMeshPatch(
		BuildPatchRing(m_hitPos, p, m_width), std::string());

	return commitPatch(patch);
}

void MeshPatchToolState::handleRender()
{
	auto navMesh = m_meshTool->GetNavMesh();
	if (!navMesh)
		return;

	ZoneRenderDebugDraw dd(g_zoneRenderManager);
	const float s = navMesh->GetNavMeshConfig().agentRadius;

	// The pending first click, so it is clear the tool is waiting for a second one.
	if (m_hitPosSet)
	{
		duDebugDrawCross(&dd, m_hitPos[0], m_hitPos[1] + 0.1f, m_hitPos[2], s,
			duRGBA(255, 190, 0, 200), 2.0f);
	}

	// Outline every patch. They become ordinary geometry once the tiles rebuild, so without
	// this there is nothing to show which mesh came from a patch and which from the zone.
	const uint32_t color = duRGBA(255, 190, 0, 220);

	dd.begin(DU_DRAW_LINES, 2.0f);

	for (size_t i = 0; i < navMesh->GetMeshPatchCount(); ++i)
	{
		const MeshPatch* patch = navMesh->GetMeshPatch(i);
		const size_t n = patch->verts.size();

		if (patch->type == MeshPatchType::Box)
		{
			if (n < 2)
				continue;

			// Two corners, so the ring walk below does not apply - draw the twelve edges.
			const glm::vec3 lo = glm::min(patch->verts[0], patch->verts[1]);
			const glm::vec3 hi = glm::max(patch->verts[0], patch->verts[1]);

			duAppendBoxWire(&dd, lo.x, lo.y, lo.z, hi.x, hi.y, hi.z, color);
			continue;
		}

		for (size_t j = 0; j < n; ++j)
		{
			const glm::vec3& a = patch->verts[j];
			const glm::vec3& b = patch->verts[(j + 1) % n];

			dd.vertex(a.x, a.y + 0.1f, a.z, color);
			dd.vertex(b.x, b.y + 0.1f, b.z, color);
		}
	}

	dd.end();
}
