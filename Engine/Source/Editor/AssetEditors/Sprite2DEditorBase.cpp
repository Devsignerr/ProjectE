#include "Editor/AssetEditors/Sprite2DEditorBase.h"

#include "Core/CommandLine.h"
#include "Core/FileSystem.h"
#include "Core/Log.h"
#include "Core/StringConv.h"
#include "Editor/ContentBrowser/ContentDragDrop.h"
#include "Editor/EditorContext.h"
#include "RHI/D3D12/D3D12Common.h"
#include "Renderer/ResourceManager.h"
#include "Scene/Sprite/Sprite2DLibrary.h"

#include <algorithm>
#include <cwctype>

E_DECLARE_LOG_CATEGORY(LogEditor)

namespace
{
	std::wstring LowerExtension(const std::filesystem::path& Path)
	{
		std::wstring Extension = Path.extension().wstring();
		std::transform(Extension.begin(), Extension.end(), Extension.begin(), [](wchar_t Char) { return static_cast<wchar_t>(std::towlower(Char)); });
		return Extension;
	}

	bool ReadFileBytes(const std::filesystem::path& Path, std::vector<uint8>& OutBytes)
	{
		OutBytes.clear();
		return FFileSystem::ReadFile(Path, OutBytes);
	}
} // namespace

void FSprite2DEditorBase::Update(FAssetEditorEnvironment& Env, float DeltaSeconds)
{
	FDataEditorBase::Update(Env, DeltaSeconds);
	if (IsDirty())
	{
		bEditedSinceOpen = true;
	}
	const uint32 Generation = FSprite2DLibrary::Get().GetGeneration();
	if (Generation != KnownSprite2DGeneration)
	{
		const bool bFirst       = KnownSprite2DGeneration == 0;
		KnownSprite2DGeneration = Generation;
		if (!bFirst)
		{
			OnSprite2DLibraryChanged(Env);
		}
	}
	++FrameCounter;
	if (FrameCounter == 20 && FCommandLine::FromProcess().HasFlag(GetVerifyRoundTripFlag()))
	{
		RunVerifyRoundTrip(Env);
	}
}

bool FSprite2DEditorBase::ReadAssetText(std::string& OutText) const
{
	return FFileSystem::ReadTextFile(Path, OutText);
}

std::string FSprite2DEditorBase::ResolveReference(const std::string& Reference) const
{
	return FSprite2DLibrary::ResolveReference(GetAssetPathString(), Reference);
}

bool FSprite2DEditorBase::UpdateTexturePreview(FAssetEditorEnvironment& Env, FTexturePreview& Target, const std::string& ContentPath)
{
	const std::filesystem::path Resolved = ContentPath.empty() ? std::filesystem::path() : FSprite2DLibrary::Get().ResolvePath(ContentPath);
	if (Resolved == Target.Path && (Target.IsValid() || Target.bFailed || Resolved.empty()))
	{
		return false;
	}
	ReleaseTexturePreview(Env, Target);
	Target.Path = Resolved;
	if (Resolved.empty())
	{
		return true;
	}
	std::vector<uint8> Bytes;
	const std::string  Display = FStringConv::ToUtf8(Resolved.filename().wstring());
	if (!ReadFileBytes(Resolved, Bytes) || !FImageLoader::LoadFromMemory(Bytes.data(), Bytes.size(), Target.Image, Display.c_str()))
	{
		Target.bFailed = true;
		E_LOG(LogEditor, Warning, "[2D 편집기] 텍스처를 읽을 수 없습니다: {}", FStringConv::ToUtf8(Resolved.wstring()));
		return true;
	}
	if (Env.Resources != nullptr)
	{
		Target.Handle = Env.Resources->CreateTexture(Target.Image.Width, Target.Image.Height, DXGI_FORMAT_R8G8B8A8_UNORM, Target.Image.Pixels.data(),
		                                              FImage::BytesPerPixel, L"Sprite2DEditorPreview");
	}
	return true;
}

void FSprite2DEditorBase::ReleaseTexturePreview(FAssetEditorEnvironment& Env, FTexturePreview& Target)
{
	if (Target.Handle.IsValid() && Env.Resources != nullptr)
	{
		Env.Resources->DestroyTexture(Target.Handle);
	}
	Target = FTexturePreview{};
}

ImTextureID FSprite2DEditorBase::GetImTexture(FAssetEditorEnvironment& Env, const FTexturePreview& Target) const
{
	if (!Target.Handle.IsValid() || Env.Resources == nullptr || !Env.Resources->IsReady(Target.Handle))
	{
		return ImTextureID{};
	}
	return static_cast<ImTextureID>(Env.Resources->ResolveTexture(Target.Handle).GetSrv().Gpu.ptr);
}

bool FSprite2DEditorBase::AcceptAssetDrop(std::string& InOutReference, const wchar_t* Extension) const
{
	bool bChanged = false;
	if (ImGui::BeginDragDropTarget())
	{
		if (const std::vector<std::filesystem::path>* Paths = FContentDragDrop::AcceptPayload();
		    Paths != nullptr && !Paths->empty() && LowerExtension(Paths->front()) == Extension)
		{
			std::error_code             ErrorCode;
			const std::filesystem::path Relative = std::filesystem::relative(Paths->front(), GetAssetDirectory(), ErrorCode);
			if (!ErrorCode && !Relative.empty())
			{
				InOutReference = FStringConv::ToUtf8(Relative.generic_wstring());
				bChanged       = true;
			}
		}
		ImGui::EndDragDropTarget();
	}
	return bChanged;
}

const std::vector<std::string>& FSprite2DEditorBase::GetContentFiles(FAssetEditorEnvironment& Env, const wchar_t* Extension)
{
	const double Now = ImGui::GetTime();
	if (ScannedExtension == Extension && Now - ScanTime < 2.0)
	{
		return ScannedFiles;
	}
	ScannedExtension = Extension;
	ScanTime         = Now;
	ScannedFiles.clear();
	if (Env.Editor == nullptr)
	{
		return ScannedFiles;
	}
	std::error_code ErrorCode;
	for (auto It = std::filesystem::recursive_directory_iterator(Env.Editor->ContentDirectory, ErrorCode);
	     !ErrorCode && It != std::filesystem::recursive_directory_iterator(); It.increment(ErrorCode))
	{
		if (It->is_regular_file(ErrorCode) && LowerExtension(It->path()) == Extension)
		{
			const std::filesystem::path Relative = std::filesystem::relative(It->path(), GetAssetDirectory(), ErrorCode);
			if (!ErrorCode)
			{
				ScannedFiles.push_back(FStringConv::ToUtf8(Relative.generic_wstring()));
			}
		}
	}
	std::sort(ScannedFiles.begin(), ScannedFiles.end());
	return ScannedFiles;
}

void FSprite2DEditorBase::AfterSaved()
{
	RememberDiskState();
	KnownSprite2DGeneration = FSprite2DLibrary::Get().GetGeneration(); // 자기 저장은 변경 알림으로 보지 않는다
}

void FSprite2DEditorBase::OnClose(FAssetEditorEnvironment& Env)
{
	(void)Env;
	if (bEditedSinceOpen)
	{
		// 저장 안 함으로 닫아도 라이브러리는 파일 내용만 갖지만, 혹시 다른 경로로 캐시가 앞섰다면 파일 상태로 다시 읽게 한다
		FSprite2DLibrary::Get().Invalidate(GetAssetPathString());
	}
}

void FSprite2DEditorBase::RunVerifyRoundTrip(FAssetEditorEnvironment& Env)
{
	std::vector<std::string> Failures;
	const auto               Check = [&Failures](bool bOk, const char* What) {
		if (!bOk)
		{
			Failures.emplace_back(What);
		}
	};
	std::vector<uint8> FileBefore;
	Check(ReadFileBytes(Path, FileBefore), "파일 읽기");
	const std::string Original = CaptureState();

	Check(ApplyVerifyEdits(Env), "프로그램 편집");
	CommitPendingEdit(false);
	const std::string Edited = CaptureState();
	Check(Edited != Original, "편집으로 상태가 바뀜");
	Check(IsDirty(), "편집 후 변경 표시");

	uint32 UndoCount = 0;
	while (CanUndo() && UndoCount < 1000)
	{
		Undo(Env);
		++UndoCount;
	}
	Check(UndoCount > 0, "실행 취소 단계 있음");
	Check(CaptureState() == Original, "실행 취소 끝 = 처음 상태");
	while (CanRedo())
	{
		Redo(Env);
	}
	Check(CaptureState() == Edited, "다시 실행 끝 = 편집 상태");

	// 저장 안 함 닫기와 같은 경로 (FAssetEditor::Close → RevertToSaved)
	RevertToSaved(Env);
	Check(CaptureState() == Original, "저장 안 함 닫기 = 파일 상태");
	std::vector<uint8> FileAfter;
	Check(ReadFileBytes(Path, FileAfter) && FileAfter == FileBefore, "파일 바이트 불변");

	if (Failures.empty())
	{
		E_LOG(LogEditor, Display, "[2D 편집기 왕복] 성공: {} (실행 취소 {}단계)", GetDisplayName(), UndoCount);
		return;
	}
	std::string Joined;
	for (const std::string& Failure : Failures)
	{
		Joined += (Joined.empty() ? "" : ", ") + Failure;
	}
	E_LOG(LogEditor, Error, "[2D 편집기 왕복] 실패: {} — {}", GetDisplayName(), Joined);
}
