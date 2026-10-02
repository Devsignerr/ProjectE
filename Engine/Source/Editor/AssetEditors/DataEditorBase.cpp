#include "Editor/AssetEditors/DataEditorBase.h"

#include "Core/Log.h"
#include "Core/StringConv.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorTheme.h"
#include "Scene/DataLibrary.h"

#include <imgui.h>

E_DECLARE_LOG_CATEGORY(LogEditor)

namespace
{
	constexpr float DiskPollInterval = 0.5f; // 초
}

void FDataEditorBase::RememberDiskState()
{
	std::error_code ErrorCode;
	KnownWriteTime  = std::filesystem::last_write_time(Path, ErrorCode);
	KnownGeneration = FDataLibrary::Get().GetGeneration();
	bExternalChange = false;
}

std::string FDataEditorBase::GetAssetPathString() const
{
	return FStringConv::ToUtf8(Path.lexically_normal().generic_wstring());
}

void FDataEditorBase::Notify(FAssetEditorEnvironment& Env, const std::string& Message, bool bError) const
{
	if (Env.Editor != nullptr && Env.Editor->Notify)
	{
		Env.Editor->Notify(Message, bError);
	}
	else
	{
		E_LOG(LogEditor, Display, "{}", Message);
	}
}

void FDataEditorBase::Update(FAssetEditorEnvironment& Env, float DeltaSeconds)
{
	// 캐시 세대: 저장(자기 포함)·핫 리로드마다 오른다 → 구조체/참조 대상을 다시 묶는다
	if (FDataLibrary::Get().GetGeneration() != KnownGeneration)
	{
		KnownGeneration = FDataLibrary::Get().GetGeneration();
		OnDataLibraryChanged(Env);
	}

	PollTimer += DeltaSeconds;
	if (PollTimer < DiskPollInterval)
	{
		return;
	}
	PollTimer = 0.0f;
	std::error_code                       ErrorCode;
	const std::filesystem::file_time_type WriteTime = std::filesystem::last_write_time(Path, ErrorCode);
	if (ErrorCode || WriteTime == KnownWriteTime)
	{
		return;
	}
	KnownWriteTime = WriteTime;
	if (!IsDirty())
	{
		RevertToSaved(Env);
		RememberDiskState();
		E_LOG(LogEditor, Display, "데이터 파일이 디스크에서 바뀌어 다시 읽음: {}", GetDisplayName());
		return;
	}
	bExternalChange = true;
}

void FDataEditorBase::DrawExternalChangeBanner(FAssetEditorEnvironment& Env)
{
	if (!bExternalChange)
	{
		return;
	}
	ImGui::PushStyleColor(ImGuiCol_Text, FEditorTheme::Warning);
	ImGui::TextUnformatted(ICON_FA_TRIANGLE_EXCLAMATION " 디스크의 파일이 바뀌었습니다 (저장하지 않은 변경이 있음)");
	ImGui::PopStyleColor();
	ImGui::SameLine();
	if (ImGui::SmallButton("다시 읽기 (내 변경 버림)"))
	{
		RevertToSaved(Env);
		RememberDiskState();
	}
	ImGui::SameLine();
	if (ImGui::SmallButton("내 변경 유지"))
	{
		bExternalChange = false; // 저장하면 디스크 내용을 덮어쓴다
	}
}
