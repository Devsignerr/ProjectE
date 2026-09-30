#include "Core/Testing/TestFramework.h"

#pragma warning(push, 0)
#include <imgui.h>
#include <imgui_node_editor.h>
#pragma warning(pop)

namespace NodeEditor = ax::NodeEditor;

// imgui-node-editor가 엔진의 ImGui(docking 1.93 WIP)와 함께 동작하는지: 화면 없이 몇 프레임 동안 노드/핀/링크를 그린다.
// ImGui 버전이나 노드 편집기 커밋을 바꾸면 이 테스트가 먼저 깨진다 (내부 어설트)
E_TEST(NodeEditor_DrawsNodesWithEngineImGui)
{
	ImGuiContext* ImGuiContextPtr = ImGui::CreateContext();
	ImGuiIO&      IO              = ImGui::GetIO();
	IO.DisplaySize                = ImVec2(1280.0f, 720.0f);
	IO.DeltaTime                  = 1.0f / 60.0f;
	IO.IniFilename                = nullptr;
	unsigned char* Pixels         = nullptr;
	int            Width          = 0;
	int            Height         = 0;
	IO.Fonts->GetTexDataAsRGBA32(&Pixels, &Width, &Height);

	NodeEditor::Config Config;
	Config.SettingsFile                      = nullptr; // 설정 파일을 쓰지 않는다
	NodeEditor::EditorContext* EditorContext = NodeEditor::CreateEditor(&Config);

	ImVec2 NodeSize;
	for (int Frame = 0; Frame < 3; ++Frame)
	{
		ImGui::NewFrame();
		ImGui::Begin("Graph");
		NodeEditor::SetCurrentEditor(EditorContext);
		NodeEditor::Begin("BehaviorTree");

		NodeEditor::BeginNode(1);
		ImGui::TextUnformatted("Selector");
		NodeEditor::BeginPin(2, NodeEditor::PinKind::Output);
		ImGui::TextUnformatted("->");
		NodeEditor::EndPin();
		NodeEditor::EndNode();

		NodeEditor::BeginNode(3);
		NodeEditor::BeginPin(4, NodeEditor::PinKind::Input);
		ImGui::TextUnformatted("->");
		NodeEditor::EndPin();
		ImGui::TextUnformatted("Wait");
		NodeEditor::EndNode();
		if (Frame == 0)
		{
			NodeEditor::SetNodePosition(3, ImVec2(200.0f, 100.0f));
		}

		NodeEditor::Link(5, 2, 4);
		NodeEditor::End();
		NodeSize = NodeEditor::GetNodeSize(1);
		NodeEditor::SetCurrentEditor(nullptr);
		ImGui::End();
		ImGui::Render();
	}

	E_EXPECT_TRUE(NodeSize.x > 0.0f && NodeSize.y > 0.0f);

	NodeEditor::DestroyEditor(EditorContext);
	ImGui::DestroyContext(ImGuiContextPtr);
}
