
#include "pch.h"
#include "SceneObjectsPanel.h"

#include "EQComponents.h"
#include "imgui/fonts/IconsFontAwesome.h"
#include "imgui/scoped_helpers.h"
#include "meshgen/Editor.h"
#include "meshgen/Scene.h"
#include "meshgen/ZoneRenderManager.h"
#include "mq/base/String.h"

#include "entt/entity/handle.hpp"

// The name a row shows, and so the name a search matches against - searching should find what
// is on screen, including the placeholder for objects with no name.
static std::string_view GetDisplayName(const entt::handle& entity)
{
	std::string_view name = entity.get<IdentityComponent>().name;
	return name.empty() ? std::string_view{ "Unnamed" } : name;
}

SceneObjectsPanel::SceneObjectsPanel(Editor* app)
	: PanelWindow("Scene Objects", "SceneObjectsPanel")
	, m_editor(app)
{
	if ((m_project = m_editor->GetProject()))
		m_scene = m_project->GetScene();
}

SceneObjectsPanel::~SceneObjectsPanel()
{
}

void SceneObjectsPanel::OnProjectChanged(const std::shared_ptr<ZoneProject>& zoneProject)
{
	if ((m_project = zoneProject))
		m_scene = m_project->GetScene();
	else
		m_scene = nullptr;
}

void SceneObjectsPanel::OnImGuiRender(bool* p_open)
{
	if (ImGui::Begin(panelName.c_str(), p_open))
	{
		if (m_project && !m_project->IsBusy())
		{
			DrawSearchBox();

			if (!m_searchFilter.empty())
			{
				ImGui::TextDisabled("%d of %d objects match", m_searchMatchCount, m_searchTotalCount);
			}

			ImGui::Checkbox("Filter by distance", &m_filterByDistance);

			if (m_filterByDistance)
			{
				ImGui::SliderFloat("Distance Filter", &m_distanceFilter, 0.0f, 500.0f);
			}

			// Rebuilding takes seconds on a large zone, so exclusions are collected up and
			// applied when asked for rather than on every click.
			if (m_project->IsCollisionMeshDirty())
			{
				if (ImGui::Button("Rebuild collision mesh"))
				{
					if (m_project->RebuildCollisionMesh() && g_zoneRenderManager)
					{
						// The collision view draws from buffers built off the old mesh.
						g_zoneRenderManager->Rebuild();
					}
				}

				ImGui::SameLine();
				ImGui::TextUnformatted("collision exclusions have changed");
			}

			// Entity list

			mq::imgui::ScopedStyleStack cellPadding(ImGuiStyleVar_CellPadding, ImVec2(4.0f, 0.0f));

			// Table
			{
				ImGuiTableFlags tableFlags = ImGuiTableFlags_NoPadInnerX
					| ImGuiTableFlags_Resizable
					| ImGuiTableFlags_Reorderable
					| ImGuiTableFlags_ScrollY;

				constexpr int numColumns = 3;
				glm::vec3 cameraPos = m_editor->GetCamera().GetPosition();
				float distanceFilterSq = m_distanceFilter * m_distanceFilter;

				if (ImGui::BeginTable("##SceneObjectsTable", numColumns, tableFlags, ImGui::GetContentRegionAvail()))
				{
					ImGui::TableSetupColumn("Name");
					ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 100.0f);
					ImGui::TableSetupColumn("Collide", ImGuiTableColumnFlags_WidthFixed, 50.0f);

					ImGui::TableSetupScrollFreeze(0, 1);
					ImGui::TableHeadersRow();

					int matchCount = 0;
					int totalCount = 0;

					auto view = m_scene->GetAllEntitiesWith<IdentityComponent>();
					for (auto entity : view)
					{
						entt::handle handle{ m_scene->GetRegistry(), entity };

						if (!handle.any_of<HierarchicalComponent>())
							continue;

						if (m_filterByDistance)
						{
							if (auto xform = handle.try_get<TransformComponent>())
							{
								float dist = glm::distance2(xform->position, cameraPos);

								if (dist > distanceFilterSq)
									continue;
							}
						}

						if (GetParent(handle).valid())
							continue;

						++totalCount;

						if (!MatchesSearch(GetDisplayName(handle)))
							continue;

						++matchCount;
						DrawEntityRow(handle);
					}

					m_searchMatchCount = matchCount;
					m_searchTotalCount = totalCount;

					ImGui::EndTable();
				}
			}
		}
		
	}

	ImGui::End();
}

void SceneObjectsPanel::DrawSearchBox()
{
	const ImGuiStyle& style = ImGui::GetStyle();
	const float matchCaseWidth = ImGui::CalcTextSize("Aa").x + style.FramePadding.x * 2.0f;

	ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - matchCaseWidth - style.ItemSpacing.x);

	if (ImGui::InputTextWithHint("##Search", ICON_FA_SEARCH " Search objects...",
		m_searchBuffer, sizeof(m_searchBuffer)))
	{
		m_searchEditedAt = ImGui::GetTime();
		m_searchPending = true;
	}

	// Enter, or clicking away, says the name is finished - no reason to make that wait.
	if (ImGui::IsItemDeactivated() && m_searchPending)
	{
		m_searchFilter = m_searchBuffer;
		m_searchPending = false;
	}

	if (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Escape))
	{
		ClearSearch();
		ImGui::SetWindowFocus(nullptr);
	}

	ImGui::SameLine();
	const float afterInputX = ImGui::GetCursorPosX();

	// Clear button drawn inside the right end of the input, matching the bitmap browser.
	if (m_searchBuffer[0] != '\0')
	{
		ImGui::SetCursorPosX(afterInputX - style.ItemSpacing.x - 24.0f);

		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0, 0, 0, 0));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0, 0, 0, 0));
		if (ImGui::SmallButton(ICON_FA_TIMES "##ClearSearch"))
		{
			ClearSearch();
		}
		ImGui::PopStyleColor(3);

		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Clear search");

		ImGui::SameLine();
		ImGui::SetCursorPosX(afterInputX);
	}

	const bool matchCase = m_searchMatchCase;
	if (matchCase)
		ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));

	if (ImGui::Button("Aa##MatchCase"))
		m_searchMatchCase = !m_searchMatchCase;

	if (matchCase)
		ImGui::PopStyleColor();

	if (ImGui::IsItemHovered())
		ImGui::SetTooltip(m_searchMatchCase ? "Match case: on" : "Match case: off");

	UpdateSearchFilter();
}

void SceneObjectsPanel::UpdateSearchFilter()
{
	if (!m_searchPending)
		return;

	// Emptying the box brings everything back at once. Making that wait reads as the list
	// being slow, not as a deliberate delay.
	if (m_searchBuffer[0] == '\0' || ImGui::GetTime() - m_searchEditedAt >= kSearchDelay)
	{
		m_searchFilter = m_searchBuffer;
		m_searchPending = false;
	}
}

void SceneObjectsPanel::ClearSearch()
{
	m_searchBuffer[0] = '\0';
	m_searchFilter.clear();
	m_searchPending = false;
}

bool SceneObjectsPanel::MatchesSearch(std::string_view name) const
{
	if (m_searchFilter.empty())
		return true;

	if (m_searchMatchCase)
		return name.find(m_searchFilter) != std::string_view::npos;

	return mq::ci_find_substr(name, m_searchFilter) != -1;
}

void SceneObjectsPanel::DrawCollisionCheckbox(const entt::handle& entity)
{
	// Only objects that actually reach the collision mesh can be excluded from it. Point
	// lights and area volumes never do, and neither do actors the loader found no collision
	// geometry for - showing them a checkbox that changes nothing would be a lie.
	if (!entity.all_of<ActorComponent, CollisionComponent>())
		return;

	bool collides = !entity.any_of<CollisionExcludedComponent>();

	if (ImGui::Checkbox("##collide", &collides))
	{
		entt::handle mutableEntity = entity;

		if (collides)
			mutableEntity.remove<CollisionExcludedComponent>();
		else
			mutableEntity.emplace<CollisionExcludedComponent>();

		if (m_project)
			m_project->MarkCollisionMeshDirty();
	}

	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("Include this object in the collision mesh, and so in any navmesh built from it");
	}
}

void SceneObjectsPanel::DrawEntityRow(const entt::handle& entity)
{
	std::string_view name = GetDisplayName(entity);

	ImGui::PushID(static_cast<int>(entity.entity()));

	ImGui::TableNextRow();

	ImGui::TableNextColumn();
	ImGui::Text("%.*s", static_cast<int>(name.size()), name.data());

	ImGui::TableNextColumn();

	if (auto actor = entity.try_get<ActorComponent>())
	{
		if (actor->actor->GetSimpleModel())
			ImGui::TextUnformatted("Simple Actor");
		else if (actor->actor->GetHierarchicalModel())
			ImGui::TextUnformatted("Hierarchical Actor");
		else
			ImGui::TextUnformatted("Actor (Unknown)");
	}
	else if (entity.try_get<PointLightComponent>())
	{
		ImGui::TextUnformatted("Point Light");
	}
	else if (entity.try_get<AreaComponent>())
	{
		ImGui::TextUnformatted("Area Volume");
	}
	else if (entity.try_get<WldAreaComponent>())
	{
		ImGui::TextUnformatted("Area Volume (old)");
	}

	ImGui::TableNextColumn();
	DrawCollisionCheckbox(entity);

	ImGui::PopID();
}
