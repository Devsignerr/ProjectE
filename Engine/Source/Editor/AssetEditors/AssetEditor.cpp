#include "Editor/AssetEditors/AssetEditor.h"

#include "Core/Log.h"
#include "Core/StringConv.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorTheme.h"
#include "Renderer/ResourceManager.h"

#include <imgui.h>
#include <json.hpp>

E_DECLARE_LOG_CATEGORY(LogEditor)

FAssetEditor::FAssetEditor(std::filesystem::path InPath)
	: Path(std::move(InPath))
{
}

FAssetEditor::~FAssetEditor() = default;

bool FAssetEditor::Open(FAssetEditorEnvironment& Env)
{
	if (!Preview.Init(*Env.Rhi))
	{
		E_LOG(LogEditor, Error, "미리보기 렌더 타깃 생성 실패: {}", GetDisplayName());
		return false;
	}
	Preview.AddDefaultLight();
	if (!LoadAsset(Env))
	{
		Preview.Shutdown(*Env.Rhi);
		return false;
	}
	History.Reset(CaptureState());
	FramePreview(Env);
	return true;
}

void FAssetEditor::Close(FAssetEditorEnvironment& Env)
{
	if (IsDirty())
	{
		RevertToSaved(Env);
	}
	OnClose(Env);
	Preview.Shutdown(*Env.Rhi);
}

void FAssetEditor::OnClose(FAssetEditorEnvironment& Env)
{
	(void)Env;
}

void FAssetEditor::Update(FAssetEditorEnvironment& Env, float DeltaSeconds)
{
	(void)Env;
	(void)DeltaSeconds;
}

std::string FAssetEditor::GetDisplayName() const
{
	return FStringConv::ToUtf8(Path.filename().wstring());
}

void FAssetEditor::Draw(FAssetEditorEnvironment& Env)
{
	Preview.BeginUiFrame();

	// 상단 도구 줄: 저장 / 되돌리기 / 실행 취소 / 다시 실행
	if (HasEditableState())
	{
		ImGui::BeginDisabled(!IsDirty());
		if (ImGui::Button(ICON_FA_FLOPPY_DISK " 저장"))
		{
			Save(Env);
		}
		ImGui::SameLine();
		if (ImGui::Button(ICON_FA_ROTATE_LEFT " 되돌리기"))
		{
			RevertToSaved(Env);
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginDisabled(!CanUndo());
		if (ImGui::ArrowButton("##Undo", ImGuiDir_Left))
		{
			Undo(Env);
		}
		ImGui::EndDisabled();
		ImGui::SetItemTooltip("실행 취소 (Ctrl+Z)");
		ImGui::SameLine();
		ImGui::BeginDisabled(!CanRedo());
		if (ImGui::ArrowButton("##Redo", ImGuiDir_Right))
		{
			Redo(Env);
		}
		ImGui::EndDisabled();
		ImGui::SetItemTooltip("다시 실행 (Ctrl+Y)");
		ImGui::SameLine();
	}
	// 경로는 Content 기준으로 짧게 표시
	std::error_code             ErrorCode;
	const std::filesystem::path Relative = Env.Editor ? std::filesystem::relative(Path, Env.Editor->ContentDirectory, ErrorCode) : Path;
	ImGui::TextDisabled("%s", FStringConv::ToUtf8((ErrorCode || Relative.empty() ? Path : Relative).generic_wstring()).c_str());

	// 왼쪽 미리보기 / 오른쪽 속성 (경계를 끌어 폭 조절)
	if (ImGui::BeginTable("##AssetEditorLayout", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV, ImGui::GetContentRegionAvail()))
	{
		ImGui::TableSetupColumn("미리보기", ImGuiTableColumnFlags_WidthStretch, 2.0f);
		ImGui::TableSetupColumn("속성", ImGuiTableColumnFlags_WidthStretch, GetPropertiesWidthWeight());
		ImGui::TableNextRow();

		ImGui::TableNextColumn();
		DrawPreviewArea(Env);

		ImGui::TableNextColumn();
		if (ImGui::BeginChild("##Properties", ImVec2(0.0f, 0.0f)))
		{
			ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
			DrawProperties(Env);
			ImGui::PopItemWidth();
		}
		ImGui::EndChild();
		ImGui::EndTable();
	}
}

void FAssetEditor::DrawPreviewArea(FAssetEditorEnvironment& Env)
{
	DrawPreviewToolbar(Env);
	const ImVec2 Avail = ImGui::GetContentRegionAvail();
	Preview.DrawViewport(FVector2(Avail.x, FMath::Max(Avail.y, 64.0f)));
	if (Preview.IsHovered() && ImGui::IsKeyPressed(ImGuiKey_F) && !ImGui::GetIO().KeyCtrl)
	{
		FramePreview(Env);
	}
	DrawPreviewOverlay(Env);
}

void FAssetEditor::DrawPreviewToolbar(FAssetEditorEnvironment& Env)
{
	ImGui::Checkbox("그리드", &Preview.bShowGrid);
	ImGui::SameLine();
	if (ImGui::SmallButton("화면 맞춤 (F)"))
	{
		FramePreview(Env);
	}
}

void FAssetEditor::DrawPreviewOverlay(FAssetEditorEnvironment& Env)
{
	(void)Env;
}

void FAssetEditor::FramePreview(FAssetEditorEnvironment& Env)
{
	Preview.FrameMeshes(*Env.Resources, FBox(FVector3(-50.0f), FVector3(50.0f)));
}

void FAssetEditor::RenderPreview(FAssetEditorEnvironment& Env)
{
	if (Preview.WasDrawnThisFrame())
	{
		Preview.Render(*Env.Rhi, *Env.PreviewRenderer, Env.Grid);
	}
}

bool FAssetEditor::Save(FAssetEditorEnvironment& Env)
{
	CommitPendingEdit(false);
	if (!SaveAsset(Env))
	{
		E_LOG(LogEditor, Error, "에셋 저장 실패: {}", FStringConv::ToUtf8(Path.wstring()));
		return false;
	}
	History.MarkSaved();
	E_LOG(LogEditor, Display, "에셋 저장: {}", GetDisplayName());
	return true;
}

void FAssetEditor::RevertToSaved(FAssetEditorEnvironment& Env)
{
	PendingEdit = FPendingEdit{};
	if (!LoadAsset(Env))
	{
		E_LOG(LogEditor, Error, "에셋 다시 읽기 실패: {}", GetDisplayName());
		return;
	}
	History.Reset(CaptureState());
}

void FAssetEditor::Undo(FAssetEditorEnvironment& Env)
{
	CommitPendingEdit(false);
	if (const std::string* State = History.Undo())
	{
		RestoreState(Env, *State);
	}
}

void FAssetEditor::Redo(FAssetEditorEnvironment& Env)
{
	CommitPendingEdit(false);
	if (const std::string* State = History.Redo())
	{
		RestoreState(Env, *State);
	}
}

void FAssetEditor::CommitPendingEdit(bool bInteractionActive)
{
	std::string Label;
	if (PendingEdit.TryTake(bInteractionActive, Label))
	{
		History.Commit(std::move(Label), CaptureState());
	}
}

bool FAssetEditor::ApplyTestEdit(FAssetEditorEnvironment& Env)
{
	nlohmann::json Document = nlohmann::json::parse(CaptureState(), nullptr, false);
	if (Document.is_discarded() || !Document.is_object())
	{
		return false;
	}
	// 첫 실수 값 (중첩된 객체/배열 안까지, 깊이 우선)
	const auto FindFloat = [](auto& Self, nlohmann::json& Node) -> nlohmann::json* {
		if (Node.is_number_float())
		{
			return &Node;
		}
		if (Node.is_object() || Node.is_array())
		{
			for (nlohmann::json& Child : Node)
			{
				if (nlohmann::json* Found = Self(Self, Child))
				{
					return Found;
				}
			}
		}
		return nullptr;
	};
	nlohmann::json* Value = FindFloat(FindFloat, Document);
	if (Value == nullptr)
	{
		return false;
	}
	*Value = Value->get<double>() * 0.5 + 0.25;
	RestoreState(Env, Document.dump(2));
	History.Commit("자동 검증 편집", CaptureState());
	return IsDirty();
}
