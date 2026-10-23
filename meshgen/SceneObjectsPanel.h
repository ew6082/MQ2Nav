#pragma once

#include "meshgen/PanelManager.h"

#include "entt/entity/fwd.hpp"

#include <string>
#include <string_view>

class Editor;
class Scene;

class SceneObjectsPanel : public PanelWindow
{
public:
	explicit SceneObjectsPanel(Editor* app);
	virtual ~SceneObjectsPanel() override;

	virtual void OnImGuiRender(bool* p_open) override;
	virtual void OnProjectChanged(const std::shared_ptr<ZoneProject>& zoneProject) override;

private:
	void DrawSearchBox();
	void UpdateSearchFilter();
	void ClearSearch();
	bool MatchesSearch(std::string_view name) const;

	void DrawEntityRow(const entt::handle& handle);
	void DrawCollisionCheckbox(const entt::handle& handle);

private:
	Editor* m_editor;
	std::shared_ptr<Scene> m_scene;
	std::shared_ptr<ZoneProject> m_project;

	float m_distanceFilter = 100.0f;
	bool m_filterByDistance = false;

	// What is typed and what is applied are kept apart so that the list is not refiltered on
	// every keystroke - on a zone with thousands of objects that makes the list jump around
	// while a name is still being typed. The typed text is applied once it has been left alone
	// for kSearchDelay seconds.
	static constexpr double kSearchDelay = 0.3;

	char m_searchBuffer[256] = {};
	std::string m_searchFilter;
	double m_searchEditedAt = 0.0;
	bool m_searchPending = false;
	bool m_searchMatchCase = false;

	// Counted while drawing the list, which comes after the line that reports them, so the
	// report shows the previous frame's numbers. A one frame lag nobody can see.
	int m_searchMatchCount = 0;
	int m_searchTotalCount = 0;
};
