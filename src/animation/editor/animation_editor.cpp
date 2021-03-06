#include "animation_editor.h"
#include "animation/animation.h"
#include "animation/animation_system.h"
#include "animation/controller.h"
#include "animation/editor/state_machine_editor.h"
#include "animation/events.h"
#include "animation/state_machine.h"
#include "editor/asset_browser.h"
#include "editor/platform_interface.h"
#include "editor/property_grid.h"
#include "editor/utils.h"
#include "editor/world_editor.h"
#include "engine/blob.h"
#include "engine/crc32.h"
#include "engine/engine.h"
#include "engine/fs/os_file.h"
#include "engine/hash_map.h"
#include "engine/log.h"
#include "engine/path.h"
#include "engine/path_utils.h"
#include "engine/plugin_manager.h"
#include "engine/property_register.h"
#include "engine/resource_manager.h"
#include "engine/universe/universe.h"


static ImVec2 operator+(const ImVec2& lhs, const ImVec2& rhs) { return ImVec2(lhs.x + rhs.x, lhs.y + rhs.y); }

using namespace Lumix;

static const ComponentType ANIMABLE_HASH = PropertyRegister::getComponentType("animable");
static const ComponentType CONTROLLER_TYPE = PropertyRegister::getComponentType("anim_controller");
static const ResourceType ANIMATION_TYPE("animation");
static const ResourceType CONTROLLER_RESOURCE_TYPE("anim_controller");


namespace AnimEditor
{


class AnimationEditor : public IAnimationEditor
{
public:
	AnimationEditor(StudioApp& app);
	~AnimationEditor();

	const char* getName() const override { return "animation_editor"; }
	void setContainer(Container* container) override { m_container = container; }
	bool isEditorOpened() override { return m_editor_opened; }
	void toggleEditorOpened() override { m_editor_opened = !m_editor_opened; }
	bool isInputsOpened() override { return m_inputs_opened; }
	void toggleInputsOpened() override { m_inputs_opened = !m_inputs_opened; }
	void onWindowGUI() override;
	StudioApp& getApp() override { return m_app; }
	int getEventTypesCount() const override;
	EventType& createEventType(const char* type) override;
	EventType& getEventTypeByIdx(int idx) override  { return m_event_types[idx]; }
	EventType& getEventType(u32 type) override;

private:
	void newController();
	void save();
	void saveAs();
	void drawGraph();
	void load();
	void loadFromEntity();
	void loadFromFile();
	void editorGUI();
	void inputsGUI();
	void constantsGUI();
	void animationSlotsGUI();
	void menuGUI();
	void onSetInputGUI(u8* data, Component& component);

private:
	StudioApp& m_app;
	bool m_editor_opened;
	bool m_inputs_opened;
	ImVec2 m_offset;
	ControllerResource* m_resource;
	Container* m_container;
	StaticString<MAX_PATH_LENGTH> m_path;
	Array<EventType> m_event_types;
};


AnimationEditor::AnimationEditor(StudioApp& app)
	: m_app(app)
	, m_editor_opened(false)
	, m_inputs_opened(false)
	, m_offset(0, 0)
	, m_event_types(app.getWorldEditor()->getAllocator())
{
	m_path = "";
	IAllocator& allocator = app.getWorldEditor()->getAllocator();

	auto* action = LUMIX_NEW(allocator, Action)("Animation Editor", "animation_editor");
	action->func.bind<AnimationEditor, &AnimationEditor::toggleEditorOpened>(this);
	action->is_selected.bind<AnimationEditor, &AnimationEditor::isEditorOpened>(this);
	app.addWindowAction(action);

	action = LUMIX_NEW(allocator, Action)("Animation Inputs", "animation_inputs");
	action->func.bind<AnimationEditor, &AnimationEditor::toggleInputsOpened>(this);
	action->is_selected.bind<AnimationEditor, &AnimationEditor::isInputsOpened>(this);
	app.addWindowAction(action);

	Engine& engine = m_app.getWorldEditor()->getEngine();
	auto* manager = engine.getResourceManager().get(CONTROLLER_RESOURCE_TYPE);
	auto* anim_sys = (AnimationSystem*)engine.getPluginManager().getPlugin("animation");
	m_resource = LUMIX_NEW(allocator, ControllerResource)(*anim_sys, *this, *manager, allocator);
	m_container = (Container*)m_resource->getRoot();

	EventType& event_type = createEventType("set_input");
	event_type.size = sizeof(Anim::SetInputEvent);
	event_type.label = "Set Input";
	event_type.editor.bind<AnimationEditor, &AnimationEditor::onSetInputGUI>(this);
}


AnimationEditor::~AnimationEditor()
{
	IAllocator& allocator = m_app.getWorldEditor()->getAllocator();
	LUMIX_DELETE(allocator, m_resource);
}


AnimationEditor::EventType& AnimationEditor::getEventType(u32 type)
{
	for (auto& i : m_event_types)
	{
		if (i.type == type) return i;
	}
	return m_event_types[0];
}


void AnimationEditor::onSetInputGUI(u8* data, Component& component)
{
	auto event = (Anim::SetInputEvent*)data;
	auto& input_decl = component.getController().getEngineResource()->m_input_decl;
	auto getter = [](void* data, int idx, const char** out) -> bool {
		auto& input_decl = *(Anim::InputDecl*)data;
		*out = input_decl.inputs[idx].name;
		return true;
	};
	ImGui::Combo("Input", &event->input_idx, getter, &input_decl, input_decl.inputs_count);
	if (event->input_idx >= 0 && event->input_idx < input_decl.inputs_count)
	{
		switch (input_decl.inputs[event->input_idx].type)
		{
			case Anim::InputDecl::BOOL: ImGui::Checkbox("Value", &event->b_value); break;
			case Anim::InputDecl::INT: ImGui::InputInt("Value", &event->i_value); break;
			case Anim::InputDecl::FLOAT: ImGui::InputFloat("Value", &event->f_value); break;
			default: ASSERT(false); break;
		}
	}
}


void AnimationEditor::onWindowGUI()
{
	editorGUI();
	inputsGUI();
}


void AnimationEditor::saveAs()
{
	if (!PlatformInterface::getSaveFilename(m_path.data, lengthOf(m_path.data), "Animation controllers\0*.act\0", "")) return;
	save();
}


void AnimationEditor::save()
{
	if (m_path[0] == 0 &&
		!PlatformInterface::getSaveFilename(m_path.data, lengthOf(m_path.data), "Animation controllers\0*.act\0", ""))
		return;
	IAllocator& allocator = m_app.getWorldEditor()->getAllocator();
	OutputBlob blob(allocator);
	m_resource->serialize(blob);
	FS::OsFile file;
	file.open(m_path, FS::Mode::CREATE_AND_WRITE, allocator);
	file.write(blob.getData(), blob.getPos());
	file.close();
}


void AnimationEditor::drawGraph()
{
	ImGui::BeginChild("canvas", ImVec2(0, 0), true);
	if (ImGui::IsWindowHovered() && !ImGui::IsAnyItemActive() && ImGui::IsMouseDragging(2, 0.0f))
	{
		m_offset = m_offset + ImGui::GetIO().MouseDelta;
	}

	auto* scene = (AnimationScene*)m_app.getWorldEditor()->getUniverse()->getScene(ANIMABLE_HASH);
	auto& entities = m_app.getWorldEditor()->getSelectedEntities();
	Anim::ComponentInstance* runtime = nullptr;
	if (!entities.empty())
	{
		ComponentHandle ctrl = scene->getComponent(entities[0], CONTROLLER_TYPE);
		if (isValid(ctrl))
		{
			runtime = scene->getControllerRoot(ctrl);
		}
	}

	ImDrawList* draw = ImGui::GetWindowDrawList();
	auto canvas_screen_pos = ImGui::GetCursorScreenPos() + m_offset;
	m_container->drawInside(draw, canvas_screen_pos);
	if(runtime) m_resource->getRoot()->debugInside(draw, canvas_screen_pos, runtime, m_container);

	ImGui::EndChild();
}


void AnimationEditor::loadFromEntity()
{
	auto& entities = m_app.getWorldEditor()->getSelectedEntities();
	if (entities.empty()) return;
	auto* scene = (AnimationScene*)m_app.getWorldEditor()->getUniverse()->getScene(ANIMABLE_HASH);
	ComponentHandle ctrl = scene->getComponent(entities[0], CONTROLLER_TYPE);
	if (!isValid(ctrl)) return;
	m_path = scene->getControllerSource(ctrl).c_str();
	load();
}


void AnimationEditor::load()
{
	IAllocator& allocator = m_app.getWorldEditor()->getAllocator();
	FS::OsFile file;
	file.open(m_path, FS::Mode::OPEN_AND_READ, allocator);
	Array<u8> data(allocator);
	data.resize((int)file.size());
	file.read(&data[0], data.size());
	InputBlob blob(&data[0], data.size());
	if (m_resource->deserialize(blob, m_app.getWorldEditor()->getEngine(), allocator))
	{
		m_container = (Container*)m_resource->getRoot();
	}
	else
	{
		LUMIX_DELETE(allocator, m_resource);
		Engine& engine = m_app.getWorldEditor()->getEngine();
		auto* manager = engine.getResourceManager().get(CONTROLLER_RESOURCE_TYPE);
		auto* anim_sys = (AnimationSystem*)engine.getPluginManager().getPlugin("animation");
		m_resource = LUMIX_NEW(allocator, ControllerResource)(*anim_sys, *this, *manager, allocator);
		m_container = (Container*)m_resource->getRoot();
	}
	file.close();
}


void AnimationEditor::loadFromFile()
{
	if (!PlatformInterface::getOpenFilename(m_path.data, lengthOf(m_path.data), "Animation controllers\0*.act\0", "")) return;
	load();
}


void AnimationEditor::newController()
{
	IAllocator& allocator = m_app.getWorldEditor()->getAllocator();
	LUMIX_DELETE(allocator, m_resource);
	Engine& engine = m_app.getWorldEditor()->getEngine();
	auto* manager = engine.getResourceManager().get(CONTROLLER_RESOURCE_TYPE);
	auto* anim_sys = (AnimationSystem*)engine.getPluginManager().getPlugin("animation");
	m_resource = LUMIX_NEW(allocator, ControllerResource)(*anim_sys, *this, *manager, allocator);
	m_container = (Container*)m_resource->getRoot();
	m_path = "";
}


int AnimationEditor::getEventTypesCount() const
{
	return m_event_types.size();
}


AnimationEditor::EventType& AnimationEditor::createEventType(const char* type)
{
	EventType& event_type = m_event_types.emplace();
	event_type.type = crc32(type);
	return event_type;
}


void AnimationEditor::menuGUI()
{
	if (ImGui::BeginMenuBar())
	{
		if (ImGui::BeginMenu("File"))
		{
			if (ImGui::MenuItem("New")) newController();
			if (ImGui::MenuItem("Save")) save();
			if (ImGui::MenuItem("Save As")) saveAs();
			if (ImGui::MenuItem("Open")) loadFromFile();
			if (ImGui::MenuItem("Open from selected entity")) loadFromEntity();
			ImGui::EndMenu();
		}
		if (ImGui::MenuItem("Go up", nullptr, false, m_container->getParent() != nullptr))
		{
			m_container = m_container->getParent();
		}
		ImGui::EndMenuBar();
	}
}


void AnimationEditor::editorGUI()
{
	if (ImGui::BeginDock("Animation Editor", &m_editor_opened, ImGuiWindowFlags_MenuBar))
	{
		menuGUI();
		ImGui::Columns(2);
		drawGraph();
		ImGui::NextColumn();
		ImGui::Text("Properties");
		if(m_container->getSelectedComponent()) m_container->getSelectedComponent()->onGUI();
		ImGui::Columns();
	}
	ImGui::EndDock();
}


void AnimationEditor::inputsGUI()
{
	if (ImGui::BeginDock("Animation inputs", &m_inputs_opened))
	{
		if (ImGui::CollapsingHeader("Inputs"))
		{
			const auto& selected_entities = m_app.getWorldEditor()->getSelectedEntities();
			auto* scene = (AnimationScene*)m_app.getWorldEditor()->getUniverse()->getScene(ANIMABLE_HASH);
			ComponentHandle cmp = selected_entities.empty() ? INVALID_COMPONENT : scene->getComponent(selected_entities[0], CONTROLLER_TYPE);
			u8* input_data = isValid(cmp) ? scene->getControllerInput(cmp) : nullptr;
			Anim::InputDecl& input_decl = m_resource->getEngineResource()->m_input_decl;

			for (int i = 0; i < input_decl.inputs_count; ++i)
			{
				ImGui::PushID(i);
				auto& input = input_decl.inputs[i];
				ImGui::PushItemWidth(100);
				ImGui::InputText("##name", input.name, lengthOf(input.name));
				ImGui::SameLine();
				if (ImGui::Combo("##type", (int*)&input.type, "float\0int\0bool\0"))
				{
					input_decl.recalculateOffsets();
				}
				if (input_data)
				{
					ImGui::SameLine();
					switch (input.type)
					{
						case Anim::InputDecl::FLOAT: ImGui::DragFloat("##value", (float*)(input_data + input.offset)); break;
						case Anim::InputDecl::BOOL: ImGui::Checkbox("##value", (bool*)(input_data + input.offset)); break;
						case Anim::InputDecl::INT: ImGui::InputInt("##value", (int*)(input_data + input.offset)); break;
						default: ASSERT(false); break;
					}
				}
				ImGui::PopItemWidth();
				ImGui::PopID();
			}

			if (ImGui::Button("Add"))
			{
				auto& input = input_decl.inputs[input_decl.inputs_count];
				input.name[0] = 0;
				input.type = Anim::InputDecl::BOOL;
				input.offset = input_decl.getSize();
				++input_decl.inputs_count;
			}
		}

		constantsGUI();
		animationSlotsGUI();
	}
	ImGui::EndDock();
}


void AnimationEditor::constantsGUI()
{
	if (!ImGui::CollapsingHeader("Constants")) return;

	Anim::InputDecl& input_decl = m_resource->getEngineResource()->m_input_decl;
	for (int i = 0; i < input_decl.constants_count; ++i)
	{
		auto& constant = input_decl.constants[i];
		StaticString<20> tmp("###", i);
		ImGui::PushItemWidth(100);
		ImGui::InputText(tmp, constant.name, lengthOf(constant.name));
		ImGui::SameLine();
		tmp << "*";
		if (ImGui::Combo(tmp, (int*)&constant.type, "float\0int\0bool\0"))
		{
			input_decl.recalculateOffsets();
		}
		ImGui::SameLine();
		tmp << "*";
		switch (constant.type)
		{
			case Anim::InputDecl::FLOAT: ImGui::DragFloat(tmp, &constant.f_value); break;
			case Anim::InputDecl::BOOL: ImGui::Checkbox(tmp, &constant.b_value); break;
			case Anim::InputDecl::INT: ImGui::InputInt(tmp, &constant.i_value); break;
			default: ASSERT(false); break;
		}
		ImGui::PopItemWidth();
	}

	if (ImGui::Button("Add##add_const"))
	{
		auto& constant = input_decl.constants[input_decl.constants_count];
		constant.name[0] = 0;
		constant.type = Anim::InputDecl::BOOL;
		constant.b_value = true;
		++input_decl.constants_count;
	}
}


void AnimationEditor::animationSlotsGUI()
{
	if (!ImGui::CollapsingHeader("Animation slots")) return;
	ImGui::PushID("anim_slots");
	auto& engine_anim_set = m_resource->getEngineResource()->m_animation_set;
	auto& slots = m_resource->getAnimationSlots();
	auto& sets = m_resource->getEngineResource()->m_sets_names;
	ImGui::PushItemWidth(-1);
	ImGui::Columns(sets.size() + 1);
	ImGui::NextColumn();
	ImGui::PushID("header");
	for (int j = 0; j < sets.size(); ++j)
	{
		ImGui::PushID(j);
		ImGui::PushItemWidth(-1);
		ImGui::InputText("", sets[j].data, lengthOf(sets[j].data));
		ImGui::PopItemWidth();
		ImGui::PopID();
		ImGui::NextColumn();
	}
	ImGui::PopID();
	ImGui::Separator();
	for (int i = 0; i < slots.size(); ++i)
	{
		const string& slot = slots[i];
		ImGui::PushID(i);
		char slot_cstr[64];
		copyString(slot_cstr, slot.c_str());

		ImGui::PushItemWidth(-20);
		if (ImGui::InputText("##name", slot_cstr, lengthOf(slot_cstr), ImGuiInputTextFlags_EnterReturnsTrue))
		{
			bool exists = slots.find([&slot_cstr](const string& val) { return val == slot_cstr; }) >= 0;

			if (exists)
			{
				g_log_error.log("Animation") << "Slot " << slot_cstr << " already exists.";
			}
			else
			{
				u32 old_hash = crc32(slot.c_str());
				u32 new_hash = crc32(slot_cstr);

				for (auto& entry : engine_anim_set)
				{
					if (entry.hash == old_hash) entry.hash = new_hash;
				}
				slots[i] = slot_cstr;
			}
		}
		ImGui::PopItemWidth();
		ImGui::SameLine();
		u32 slot_hash = crc32(slot.c_str());
		if (ImGui::Button("x"))
		{
			slots.erase(i);
			engine_anim_set.eraseItems([slot_hash](Anim::ControllerResource::AnimSetEntry& val) { return val.hash == slot_hash; });
			--i;
		}
		ImGui::NextColumn();
		for (int j = 0; j < sets.size(); ++j)
		{
			Anim::ControllerResource::AnimSetEntry* entry = nullptr;
			for (auto& e : engine_anim_set)
			{
				if (e.set == j && e.hash == slot_hash) 
				{
					entry = &e;
					break;
				}
			}

			ImGui::PushItemWidth(ImGui::GetColumnWidth());
			char tmp[MAX_PATH_LENGTH];
			copyString(tmp, entry && entry->animation ? entry->animation->getPath().c_str() : "");
			ImGui::PushID(j);
			if (m_app.getAssetBrowser()->resourceInput("", "##res", tmp, lengthOf(tmp), ANIMATION_TYPE))
			{
				if (entry && entry->animation) entry->animation->getResourceManager().unload(*entry->animation);
				auto* manager = m_app.getWorldEditor()->getEngine().getResourceManager().get(ANIMATION_TYPE);
				if (entry)
				{
					entry->animation = (Animation*)manager->load(Path(tmp));
				}
				else
				{
					engine_anim_set.push({j, slot_hash, (Animation*)manager->load(Path(tmp))});
				}
			}
			ImGui::PopID();
			ImGui::PopItemWidth();


			ImGui::NextColumn();
		}
		ImGui::PopID();
	}
	ImGui::Columns();

	if (ImGui::Button("Add slot (row)"))
	{
		bool exists = slots.find([](const string& val) { return val == ""; }) >= 0;

		if (exists)
		{
			g_log_error.log("Animation") << "Slot with empty name already exists. Please rename it and then you can create a new slot.";
		}
		else
		{
			IAllocator& allocator = m_app.getWorldEditor()->getAllocator();
			slots.emplace("", allocator);
		}
	}
	if (ImGui::Button("Add set (column)"))
	{
		IAllocator& allocator = m_app.getWorldEditor()->getAllocator();
		m_resource->getEngineResource()->m_sets_names.emplace("new set");
	}
	ImGui::PopItemWidth();
	ImGui::PopID();


}


IAnimationEditor* IAnimationEditor::create(IAllocator& allocator, StudioApp& app)
{
	return LUMIX_NEW(allocator, AnimationEditor)(app);
}


} // namespace AnimEditor