#include "Editor/AssetEditors/SequenceEditor.h"

#include "Core/CommandLine.h"
#include "Core/FileSystem.h"
#include "Core/Log.h"
#include "Core/Reflection/TypeInfo.h"
#include "Core/StringConv.h"
#include "Editor/AssetEditors/AssetEditorWidgets.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorTheme.h"
#include "Renderer/ModelLoader.h"
#include "Renderer/SceneCamera.h"
#include "Scene/AnimationSystem.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <format>
#include <iterator>

E_DECLARE_LOG_CATEGORY(LogEditor)

namespace
{
	constexpr float HeaderWidth = 230.0f;
	constexpr float RulerHeight = 26.0f;
	constexpr float KeyRadius   = 6.0f;

	ImU32 ToU32(const ImVec4& Color) { return ImGui::ColorConvertFloat4ToU32(Color); }

	bool InputString(const char* Label, std::string& Value)
	{
		char Buffer[256];
		strncpy_s(Buffer, Value.c_str(), _TRUNCATE);
		if (ImGui::InputText(Label, Buffer, sizeof(Buffer)))
		{
			Value = Buffer;
			return true;
		}
		return false;
	}

	const char* TrackIcon(ESequenceTrackType Type)
	{
		switch (Type)
		{
		case ESequenceTrackType::Transform: return ICON_FA_UP_DOWN_LEFT_RIGHT;
		case ESequenceTrackType::Property:  return ICON_FA_SLIDERS;
		case ESequenceTrackType::CameraCut: return ICON_FA_VIDEO;
		case ESequenceTrackType::Animation: return ICON_FA_PERSON_RUNNING;
		case ESequenceTrackType::Event:     return ICON_FA_BOLT;
		}
		return ICON_FA_CIRCLE;
	}

	ImU32 InterpColor(ESequenceInterp Interp)
	{
		switch (Interp)
		{
		case ESequenceInterp::Constant: return IM_COL32(240, 170, 70, 255);
		case ESequenceInterp::Linear:   return IM_COL32(120, 210, 120, 255);
		case ESequenceInterp::Smooth:   return IM_COL32(230, 230, 230, 255);
		}
		return IM_COL32_WHITE;
	}

	constexpr const char* InterpLabels[] = { "계단 (Constant)", "직선 (Linear)", "곡선 (Smooth)" };

	bool IsAnimatable(const FPropertyInfo& Property)
	{
		if (Property.HasFlag(PF_Hidden) || Property.HasFlag(PF_ReadOnly))
		{
			return false;
		}
		switch (Property.Type)
		{
		case EPropertyType::Bool:
		case EPropertyType::Int32:
		case EPropertyType::UInt32:
		case EPropertyType::Float:
		case EPropertyType::Vector2:
		case EPropertyType::Vector3:
		case EPropertyType::Vector4: return true;
		default:                     return false;
		}
	}

	int32 ComponentCount(EPropertyType Type)
	{
		switch (Type)
		{
		case EPropertyType::Vector2: return 2;
		case EPropertyType::Vector3: return 3;
		case EPropertyType::Vector4: return 4;
		default:                     return 1;
		}
	}

	const FPropertyInfo* FindTrackProperty(const FSequenceTrack& Track, const FTypeInfo** OutType)
	{
		const FTypeInfo* Type = FTypeRegistry::Get().Find(Track.Component);
		if (OutType != nullptr)
		{
			*OutType = Type;
		}
		return Type != nullptr ? Type->FindProperty(Track.Property) : nullptr;
	}

	// 정렬 유지: Index 키를 시각에 맞는 자리로 옮기고 새 번호 반환
	template <typename TKey, typename TGetTime>
	int32 Resort(std::vector<TKey>& Keys, int32 Index, TGetTime&& GetTime)
	{
		while (Index > 0 && GetTime(Keys[static_cast<size_t>(Index)]) < GetTime(Keys[static_cast<size_t>(Index - 1)]))
		{
			std::swap(Keys[static_cast<size_t>(Index)], Keys[static_cast<size_t>(Index - 1)]);
			--Index;
		}
		while (Index + 1 < static_cast<int32>(Keys.size()) && GetTime(Keys[static_cast<size_t>(Index)]) > GetTime(Keys[static_cast<size_t>(Index + 1)]))
		{
			std::swap(Keys[static_cast<size_t>(Index)], Keys[static_cast<size_t>(Index + 1)]);
			++Index;
		}
		return Index;
	}

	template <typename TKey>
	int32 InsertSorted(std::vector<TKey>& Keys, TKey Key)
	{
		Keys.push_back(std::move(Key));
		return Resort(Keys, static_cast<int32>(Keys.size()) - 1, [](const TKey& Item) { return Item.Time; });
	}
} // namespace

FSequenceEditor::FSequenceEditor(std::filesystem::path InPath)
	: FAssetEditor(std::move(InPath))
{
}

FSequenceEditor::~FSequenceEditor() = default;

// ---------------------------------------------------------------- 에셋 상태

bool FSequenceEditor::LoadAsset(FAssetEditorEnvironment& Env)
{
	std::string    Text;
	std::string    Error;
	FSequenceAsset Loaded;
	if (!FFileSystem::ReadTextFile(Path, Text) || !FSequenceAsset::FromJsonString(Text, Loaded, &Error))
	{
		E_LOG(LogEditor, Error, "시퀀스를 읽지 못했습니다: {} — {}", GetDisplayName(), Error);
		if (Env.Editor && Env.Editor->Notify)
		{
			Env.Editor->Notify("시퀀스를 읽지 못했습니다: " + Error, true);
		}
		return false;
	}
	Asset = std::move(Loaded);
	if (SelectedTrack >= static_cast<int32>(Asset.Tracks.size()))
	{
		SelectedTrack = -1;
		SelectedKey   = -1;
	}
	// 자동 검증: --sequence-time <초> (재생 헤드), --sequence-camera (카메라 미리보기)
	if (const std::wstring TimeArg = FCommandLine::FromProcess().GetValue(L"--sequence-time"); !TimeArg.empty())
	{
		Time = FMath::Clamp(std::wcstof(TimeArg.c_str(), nullptr), 0.0f, Asset.Duration);
	}
	if (FCommandLine::FromProcess().HasFlag(L"--sequence-camera"))
	{
		bCameraPreview = true;
	}
	// 자동 검증: --sequence-select <트랙 번호>[:<키 번호>] (속성 패널 확인)
	if (const std::wstring SelectArg = FCommandLine::FromProcess().GetValue(L"--sequence-select"); !SelectArg.empty() && SelectedTrack < 0)
	{
		int32 TrackArg = -1;
		int32 KeyArg   = -1;
		if (swscanf_s(SelectArg.c_str(), L"%d:%d", &TrackArg, &KeyArg) >= 1 && TrackArg >= 0 && TrackArg < static_cast<int32>(Asset.Tracks.size()))
		{
			SelectedTrack = TrackArg;
			SelectedKey   = KeyArg < static_cast<int32>(Asset.Tracks[static_cast<size_t>(TrackArg)].GetKeyCount()) ? KeyArg : -1;
		}
	}
	PublishIfChanged();
	return true;
}

bool FSequenceEditor::SaveAsset(FAssetEditorEnvironment& Env)
{
	(void)Env;
	Asset.SortKeys();
	if (!Asset.SaveToFile(Path))
	{
		return false;
	}
	FSequenceLibrary::Get().Invalidate(FModelLoader::MakeAssetPath(Path)); // 재생 중 컴포넌트가 다음 갱신에서 다시 읽는다
	return true;
}

std::string FSequenceEditor::CaptureState() const
{
	return Asset.ToJsonString();
}

void FSequenceEditor::RestoreState(FAssetEditorEnvironment& Env, const std::string& State)
{
	(void)Env;
	FSequenceAsset Restored;
	if (FSequenceAsset::FromJsonString(State, Restored))
	{
		Asset = std::move(Restored);
		if (SelectedTrack >= static_cast<int32>(Asset.Tracks.size()))
		{
			SelectedTrack = -1;
		}
		if (SelectedTrack < 0 || SelectedKey >= static_cast<int32>(Asset.Tracks[static_cast<size_t>(SelectedTrack)].GetKeyCount()))
		{
			SelectedKey = -1;
		}
	}
}

void FSequenceEditor::OnClose(FAssetEditorEnvironment& Env)
{
	(void)Env;
	EndPreview();
}

// ---------------------------------------------------------------- 미리보기

FScene* FSequenceEditor::GetEditScene(FAssetEditorEnvironment& Env) const
{
	return Env.Editor != nullptr && !Env.Editor->bPlaying ? Env.Editor->Scene : nullptr;
}

FEntity FSequenceEditor::FindPlayerEntity(const FScene& Scene) const
{
	const std::string AssetPath = FModelLoader::MakeAssetPath(Path);
	FEntity           Found;
	const_cast<FScene&>(Scene).GetRegistry().View<FSequencePlayerComponent>().Each([&](FEntity Entity, FSequencePlayerComponent& Component) {
		if (!Found.IsValid() && Component.Sequence == AssetPath)
		{
			Found = Entity;
		}
	});
	return Found;
}

FEntity FSequenceEditor::ResolveTarget(FAssetEditorEnvironment& Env, const std::string& TargetPath) const
{
	const FScene* Scene = GetEditScene(Env);
	return Scene != nullptr ? FSequenceSystem::ResolveBinding(*Scene, PreviewPlayer, TargetPath) : NullEntity;
}

void FSequenceEditor::PublishIfChanged()
{
	FSequenceAsset Copy = Asset;
	Copy.SortKeys();
	std::string Key = Copy.ToJsonString();
	if (Published && Key == PublishedKey)
	{
		return;
	}
	PublishedKey = std::move(Key);
	Published    = std::make_shared<const FSequenceAsset>(std::move(Copy));
}

void FSequenceEditor::EndPreview()
{
	if (bPreviewApplied && PreviewScene != nullptr)
	{
		FSequenceSystem::Restore(*PreviewScene, PreviewState);
		PreviewScene->UpdateTransforms();
	}
	PreviewState.Forget();
	bPreviewApplied = false;
	PreviewScene    = nullptr;
	AppliedTime     = -1.0f;
}

void FSequenceEditor::SwapScenePreview(FAssetEditorEnvironment& Env)
{
	(void)Env;
	if (bPreviewApplied && PreviewScene != nullptr)
	{
		PreviewState.SwapWithScene(*PreviewScene);
	}
}

void FSequenceEditor::EndScenePreview(FAssetEditorEnvironment& Env)
{
	(void)Env;
	EndPreview();
}

void FSequenceEditor::Update(FAssetEditorEnvironment& Env, float DeltaSeconds)
{
	PublishIfChanged();
	const float Duration = Asset.Duration;
	if (bTimelinePlaying)
	{
		Time += DeltaSeconds;
		if (Time > Duration)
		{
			if (bLoopPreview && Duration > FMath::SmallNumber)
			{
				Time = std::fmod(Time, Duration);
			}
			else
			{
				Time             = Duration;
				bTimelinePlaying = false;
			}
		}
	}
	Time = FMath::Clamp(Time, 0.0f, Duration);

	// 플레이 중 디버그: 플레이 씬에서 이 시퀀스를 재생하는 엔티티(선택 엔티티 우선)의 재생 위치
	DebugTime = -1.0f;
	DebugLabel.clear();
	if (Env.Editor != nullptr && Env.Editor->bPlaying && Env.Editor->Scene != nullptr)
	{
		FRegistry&        Registry  = Env.Editor->Scene->GetRegistry();
		const std::string AssetPath = FModelLoader::MakeAssetPath(Path);
		FEntity           Found;
		Registry.View<FSequencePlayerComponent>().Each([&](FEntity Entity, FSequencePlayerComponent& Component) {
			if (Component.Sequence == AssetPath && (!Found.IsValid() || Entity == Env.Editor->SelectedEntity))
			{
				Found = Entity;
			}
		});
		if (Found.IsValid())
		{
			const FSequencePlayerRuntime& Runtime = Registry.Get<FSequencePlayerComponent>(Found).Runtime;
			const FNameComponent*         Name    = Registry.TryGet<FNameComponent>(Found);
			DebugTime                             = Runtime.Time;
			DebugLabel = std::format("{} — {:.2f}초 {}", Name ? Name->Name : std::string("?"), Runtime.Time, Runtime.bPlaying ? "재생 중" : "멈춤");
		}
	}

	FScene* Scene = GetEditScene(Env);
	if (Scene == nullptr || !bViewportPreview)
	{
		EndPreview(); // 플레이 중이면 이미 StartPlay에서 끝났다
		return;
	}
	if (PreviewScene != nullptr && PreviewScene != Scene)
	{
		EndPreview();
	}
	PreviewPlayer = FindPlayerEntity(*Scene);
	if (PreviewState.HasStaleEntities(*Scene))
	{
		// 실행 취소/씬 다시 읽기로 엔티티가 새로 만들어졌다: 새 엔티티 값이 원래 값이므로 기록만 버리고 다시 적용
		PreviewState.Forget();
		bPreviewApplied = false;
	}
	if (!bPreviewApplied || Time != AppliedTime || PublishedKey != AppliedKey || PreviewPlayer != AppliedPlayer)
	{
		if (bPreviewApplied && (PublishedKey != AppliedKey || PreviewPlayer != AppliedPlayer))
		{
			// 내용/기준이 바뀜: 트랙을 지우거나 대상을 바꿨을 수 있으므로 원래 값으로 되돌린 뒤 새로 적용 (원래 값은 다시 기억된다)
			FSequenceSystem::Restore(*Scene, PreviewState);
		}
		FSequenceSystem::Evaluate(*Scene, PreviewPlayer, *Published, Time, PreviewState);
		Scene->UpdateTransforms();
		bPreviewApplied = true;
		PreviewScene    = Scene;
		AppliedTime     = Time;
		AppliedKey      = PublishedKey;
		AppliedPlayer   = PreviewPlayer;
	}
	// 카메라 미리보기: 지금 컷 카메라로 뷰포트를 본다 (매 프레임 — 앱이 Context.Camera를 편집 카메라로 되돌리므로)
	if (bCameraPreview && PreviewState.ActiveCamera.IsValid() && Env.Editor->Camera != nullptr)
	{
		const float Aspect = Env.Editor->Camera->GetAspectRatio();
		if (FSceneCamera::ApplyToCamera(*Scene, PreviewState.ActiveCamera, Aspect, PreviewCamera))
		{
			Env.Editor->Camera = &PreviewCamera;
		}
	}
}

// ---------------------------------------------------------------- 키 조작

float FSequenceEditor::Snap(float Seconds) const
{
	if (ImGui::GetIO().KeyAlt || Asset.FrameRate <= 0.0f)
	{
		return Seconds;
	}
	return std::round(Seconds * Asset.FrameRate) / Asset.FrameRate;
}

bool FSequenceEditor::CaptureTransform(FAssetEditorEnvironment& Env, const FSequenceTrack& Track, float Seconds, FSequenceTransformKey& Key) const
{
	FScene*       Scene  = GetEditScene(Env);
	const FEntity Target = ResolveTarget(Env, Track.Target);
	if (Scene == nullptr || !Target.IsValid())
	{
		return false;
	}
	// 회전은 시각상 이전 키의 각도에 가깝게 펼친다 (보간이 먼 길로 돌지 않게)
	FVector3 Reference;
	for (const FSequenceTransformKey& Other : Track.TransformKeys)
	{
		if (Other.Time <= Seconds)
		{
			Reference = Other.Rotation;
		}
	}
	const FTransformComponent& Transform = Scene->GetTransform(Target);
	Key.Position                         = Transform.Position;
	Key.Rotation                         = SequenceMath::QuatToEulerNear(Transform.Rotation, Reference);
	Key.Scale                            = Transform.Scale;
	return true;
}

bool FSequenceEditor::CaptureValue(FAssetEditorEnvironment& Env, const FSequenceTrack& Track, FVector4& OutValue) const
{
	FScene*              Scene    = GetEditScene(Env);
	const FEntity        Target   = ResolveTarget(Env, Track.Target);
	const FTypeInfo*     Type     = nullptr;
	const FPropertyInfo* Property = FindTrackProperty(Track, &Type);
	if (Scene == nullptr || !Target.IsValid() || Property == nullptr || !IsAnimatable(*Property))
	{
		return false;
	}
	const void* Component = Type->GetComponent(Scene->GetRegistry(), Target);
	if (Component == nullptr)
	{
		return false;
	}
	OutValue = FVector4();
	switch (Property->Type)
	{
	case EPropertyType::Bool:    OutValue.X = Property->GetRef<bool>(Component) ? 1.0f : 0.0f; break;
	case EPropertyType::Int32:   OutValue.X = static_cast<float>(*static_cast<const int32*>(Property->GetPtr(Component))); break;
	case EPropertyType::UInt32:  OutValue.X = static_cast<float>(Property->GetRef<uint32>(Component)); break;
	case EPropertyType::Float:   OutValue.X = Property->GetRef<float>(Component); break;
	case EPropertyType::Vector2: OutValue = FVector4(Property->GetRef<FVector2>(Component).X, Property->GetRef<FVector2>(Component).Y, 0.0f, 0.0f); break;
	case EPropertyType::Vector3:
	{
		const FVector3& Value = Property->GetRef<FVector3>(Component);
		OutValue              = FVector4(Value.X, Value.Y, Value.Z, 0.0f);
		break;
	}
	case EPropertyType::Vector4: OutValue = Property->GetRef<FVector4>(Component); break;
	default:                     return false;
	}
	return true;
}

int32 FSequenceEditor::AddKeyAtTime(FAssetEditorEnvironment& Env, int32 TrackIndex, float Seconds)
{
	if (TrackIndex < 0 || TrackIndex >= static_cast<int32>(Asset.Tracks.size()))
	{
		return -1;
	}
	FSequenceTrack& Track     = Asset.Tracks[static_cast<size_t>(TrackIndex)];
	Seconds                   = FMath::Clamp(Seconds, 0.0f, Asset.Duration);
	const float     HalfFrame = 0.5f / FMath::Max(Asset.FrameRate, 1.0f);
	const auto      FindAt    = [&](const auto& Keys) -> int32 {
        for (size_t Index = 0; Index < Keys.size(); ++Index)
        {
            if (FMath::Abs(Keys[Index].Time - Seconds) < HalfFrame)
            {
                return static_cast<int32>(Index);
            }
        }
        return -1;
	};
	int32 Result = -1;
	switch (Track.Type)
	{
	case ESequenceTrackType::Transform:
	{
		FSequenceTransformKey Key;
		Key.Time = Seconds;
		if (!CaptureTransform(Env, Track, Seconds, Key))
		{
			// 대상이 없으면 앞 키(없으면 원점) 값으로
			FVector3 Position, Scale;
			FQuat    Rotation;
			if (SequenceMath::EvaluateTransformKeys(Track.TransformKeys, Seconds, Position, Rotation, Scale))
			{
				Key.Position = Position;
				Key.Rotation = SequenceMath::QuatToEulerNear(Rotation, FVector3());
				Key.Scale    = Scale;
			}
		}
		if (const int32 Existing = FindAt(Track.TransformKeys); Existing >= 0)
		{
			Key.Time   = Track.TransformKeys[static_cast<size_t>(Existing)].Time;
			Key.Interp = Track.TransformKeys[static_cast<size_t>(Existing)].Interp;
			Track.TransformKeys[static_cast<size_t>(Existing)] = Key;
			Result = Existing;
		}
		else
		{
			Result = InsertSorted(Track.TransformKeys, Key);
		}
		break;
	}
	case ESequenceTrackType::Property:
	{
		FSequenceValueKey Key;
		Key.Time = Seconds;
		if (!CaptureValue(Env, Track, Key.Value))
		{
			Key.Value = SequenceMath::EvaluateValueKeys(Track.ValueKeys, Seconds, FVector4(), false);
		}
		if (const int32 Existing = FindAt(Track.ValueKeys); Existing >= 0)
		{
			Track.ValueKeys[static_cast<size_t>(Existing)].Value = Key.Value;
			Result = Existing;
		}
		else
		{
			Result = InsertSorted(Track.ValueKeys, Key);
		}
		break;
	}
	case ESequenceTrackType::CameraCut:
	{
		// 선택한 엔티티가 카메라면 그것, 아니면 앞 컷의 카메라
		FSequenceCameraCut Cut;
		Cut.Time = Seconds;
		if (const int32 Previous = SequenceMath::FindCameraCut(Track.Cuts, Seconds); Previous >= 0)
		{
			Cut.Camera = Track.Cuts[static_cast<size_t>(Previous)].Camera;
		}
		if (FScene* Scene = GetEditScene(Env); Scene != nullptr && Scene->GetRegistry().IsValid(Env.Editor->SelectedEntity) &&
		                                       Scene->GetRegistry().Has<FCameraComponent>(Env.Editor->SelectedEntity))
		{
			Cut.Camera = FSequenceSystem::MakeBindingPath(*Scene, PreviewPlayer, Env.Editor->SelectedEntity);
		}
		if (const int32 Existing = FindAt(Track.Cuts); Existing >= 0)
		{
			Track.Cuts[static_cast<size_t>(Existing)].Camera = Cut.Camera;
			Result = Existing;
		}
		else
		{
			Result = InsertSorted(Track.Cuts, Cut);
		}
		break;
	}
	case ESequenceTrackType::Animation:
	{
		FSequenceAnimSection Section;
		Section.Start = Seconds;
		Section.End   = FMath::Min(Seconds + 1.0f, FMath::Max(Asset.Duration, Seconds + 0.1f));
		if (!Track.Sections.empty())
		{
			Section.Clip = Track.Sections.back().Clip;
		}
		else if (FScene* Scene = GetEditScene(Env))
		{
			const std::vector<std::string> Clips = FAnimationSystem::GetClipNames(*Scene, ResolveTarget(Env, Track.Target));
			Section.Clip                         = Clips.empty() ? std::string() : Clips.front();
		}
		Track.Sections.push_back(Section);
		Result = Resort(Track.Sections, static_cast<int32>(Track.Sections.size()) - 1, [](const FSequenceAnimSection& Item) { return Item.Start; });
		break;
	}
	case ESequenceTrackType::Event:
	{
		if (const int32 Existing = FindAt(Track.Events); Existing >= 0)
		{
			Result = Existing;
			break;
		}
		Result = InsertSorted(Track.Events, FSequenceEventKey{ Seconds, std::format("Event{}", Track.Events.size() + 1) });
		break;
	}
	}
	SelectedTrack = TrackIndex;
	SelectedKey   = Result;
	MarkEdited("키 추가");
	return Result;
}

void FSequenceEditor::SetKeyTime(FSequenceTrack& Track, int32& InOutKey, float Seconds)
{
	if (InOutKey < 0 || InOutKey >= static_cast<int32>(Track.GetKeyCount()))
	{
		return;
	}
	const size_t Index = static_cast<size_t>(InOutKey);
	Seconds            = FMath::Clamp(Seconds, 0.0f, Asset.Duration);
	const auto ByTime  = [](const auto& Item) { return Item.Time; };
	switch (Track.Type)
	{
	case ESequenceTrackType::Transform: Track.TransformKeys[Index].Time = Seconds; InOutKey = Resort(Track.TransformKeys, InOutKey, ByTime); break;
	case ESequenceTrackType::Property:  Track.ValueKeys[Index].Time = Seconds; InOutKey = Resort(Track.ValueKeys, InOutKey, ByTime); break;
	case ESequenceTrackType::CameraCut: Track.Cuts[Index].Time = Seconds; InOutKey = Resort(Track.Cuts, InOutKey, ByTime); break;
	case ESequenceTrackType::Event:     Track.Events[Index].Time = Seconds; InOutKey = Resort(Track.Events, InOutKey, ByTime); break;
	case ESequenceTrackType::Animation:
	{
		// 구간은 길이를 유지하며 옮긴다
		FSequenceAnimSection& Section = Track.Sections[Index];
		const float           Length  = Section.End - Section.Start;
		Section.Start                 = FMath::Clamp(Seconds, 0.0f, FMath::Max(Asset.Duration - Length, 0.0f));
		Section.End                   = Section.Start + Length;
		InOutKey = Resort(Track.Sections, InOutKey, [](const FSequenceAnimSection& Item) { return Item.Start; });
		break;
	}
	}
}

void FSequenceEditor::DeleteSelectedKey()
{
	if (SelectedTrack < 0 || SelectedTrack >= static_cast<int32>(Asset.Tracks.size()))
	{
		return;
	}
	FSequenceTrack& Track = Asset.Tracks[static_cast<size_t>(SelectedTrack)];
	if (SelectedKey < 0 || SelectedKey >= static_cast<int32>(Track.GetKeyCount()))
	{
		return;
	}
	const auto Erase = [&](auto& Keys) { Keys.erase(Keys.begin() + SelectedKey); };
	switch (Track.Type)
	{
	case ESequenceTrackType::Transform: Erase(Track.TransformKeys); break;
	case ESequenceTrackType::Property:  Erase(Track.ValueKeys); break;
	case ESequenceTrackType::CameraCut: Erase(Track.Cuts); break;
	case ESequenceTrackType::Animation: Erase(Track.Sections); break;
	case ESequenceTrackType::Event:     Erase(Track.Events); break;
	}
	SelectedKey = -1;
	MarkEdited("키 삭제");
}

void FSequenceEditor::CopySelectedKey()
{
	if (SelectedTrack < 0 || SelectedTrack >= static_cast<int32>(Asset.Tracks.size()))
	{
		return;
	}
	const FSequenceTrack& Track = Asset.Tracks[static_cast<size_t>(SelectedTrack)];
	if (SelectedKey < 0 || SelectedKey >= static_cast<int32>(Track.GetKeyCount()))
	{
		return;
	}
	FSequenceTrack Copy;
	Copy.Type          = Track.Type;
	const size_t Index = static_cast<size_t>(SelectedKey);
	switch (Track.Type)
	{
	case ESequenceTrackType::Transform: Copy.TransformKeys = { Track.TransformKeys[Index] }; break;
	case ESequenceTrackType::Property:  Copy.ValueKeys = { Track.ValueKeys[Index] }; break;
	case ESequenceTrackType::CameraCut: Copy.Cuts = { Track.Cuts[Index] }; break;
	case ESequenceTrackType::Animation: Copy.Sections = { Track.Sections[Index] }; break;
	case ESequenceTrackType::Event:     Copy.Events = { Track.Events[Index] }; break;
	}
	KeyClipboard = std::move(Copy);
}

void FSequenceEditor::PasteKey(float Seconds)
{
	if (!KeyClipboard || SelectedTrack < 0 || SelectedTrack >= static_cast<int32>(Asset.Tracks.size()))
	{
		return;
	}
	FSequenceTrack& Track = Asset.Tracks[static_cast<size_t>(SelectedTrack)];
	if (Track.Type != KeyClipboard->Type)
	{
		return;
	}
	Seconds = FMath::Clamp(Seconds, 0.0f, Asset.Duration);
	switch (Track.Type)
	{
	case ESequenceTrackType::Transform:
	{
		FSequenceTransformKey Key = KeyClipboard->TransformKeys.front();
		Key.Time                  = Seconds;
		SelectedKey               = InsertSorted(Track.TransformKeys, Key);
		break;
	}
	case ESequenceTrackType::Property:
	{
		FSequenceValueKey Key = KeyClipboard->ValueKeys.front();
		Key.Time              = Seconds;
		SelectedKey           = InsertSorted(Track.ValueKeys, Key);
		break;
	}
	case ESequenceTrackType::CameraCut:
	{
		FSequenceCameraCut Cut = KeyClipboard->Cuts.front();
		Cut.Time               = Seconds;
		SelectedKey            = InsertSorted(Track.Cuts, Cut);
		break;
	}
	case ESequenceTrackType::Animation:
	{
		FSequenceAnimSection Section = KeyClipboard->Sections.front();
		const float          Length  = Section.End - Section.Start;
		Section.Start                = Seconds;
		Section.End                  = Seconds + Length;
		Track.Sections.push_back(Section);
		SelectedKey = Resort(Track.Sections, static_cast<int32>(Track.Sections.size()) - 1, [](const FSequenceAnimSection& Item) { return Item.Start; });
		break;
	}
	case ESequenceTrackType::Event:
	{
		FSequenceEventKey Event = KeyClipboard->Events.front();
		Event.Time              = Seconds;
		SelectedKey             = InsertSorted(Track.Events, Event);
		break;
	}
	}
	MarkEdited("키 붙여넣기");
}

// ---------------------------------------------------------------- 타임라인

std::string FSequenceEditor::TrackLabel(const FSequenceTrack& Track) const
{
	if (!Track.Name.empty())
	{
		return Track.Name;
	}
	const std::string Target = Track.Target.empty() ? std::string("(재생 엔티티)") : Track.Target;
	switch (Track.Type)
	{
	case ESequenceTrackType::Transform: return Target;
	case ESequenceTrackType::Property:
	{
		std::string Component = Track.Component;
		if (Component.ends_with("Component"))
		{
			Component.resize(Component.size() - 9);
		}
		return std::format("{} · {}.{}", Target, Component, Track.Property);
	}
	case ESequenceTrackType::CameraCut: return "카메라 컷";
	case ESequenceTrackType::Animation: return Target;
	case ESequenceTrackType::Event:     return "이벤트";
	}
	return "?";
}

void FSequenceEditor::DrawToolbar(FAssetEditorEnvironment& Env)
{
	if (ImGui::Button(ICON_FA_BACKWARD_STEP))
	{
		Time = 0.0f;
	}
	ImGui::SetItemTooltip("처음으로 (Home)");
	ImGui::SameLine();
	if (ImGui::Button(bTimelinePlaying ? ICON_FA_PAUSE : ICON_FA_PLAY))
	{
		bTimelinePlaying = !bTimelinePlaying;
		if (bTimelinePlaying && Time >= Asset.Duration)
		{
			Time = 0.0f;
		}
	}
	ImGui::SetItemTooltip("재생 / 일시정지 (Space)");
	ImGui::SameLine();
	if (ImGui::Button(ICON_FA_STOP))
	{
		bTimelinePlaying = false;
		Time             = 0.0f;
	}
	ImGui::SetItemTooltip("정지 (처음으로)");
	ImGui::SameLine();
	ImGui::Checkbox("반복", &bLoopPreview);
	ImGui::SameLine();
	ImGui::SetNextItemWidth(90.0f);
	if (ImGui::DragFloat("##Time", &Time, 0.01f, 0.0f, Asset.Duration, "%.2f초"))
	{
		bTimelinePlaying = false;
	}
	ImGui::SameLine();
	ImGui::TextDisabled("/ %.2f초 · %d프레임", Asset.Duration, static_cast<int32>(std::lround(Time * Asset.FrameRate)));
	ImGui::SameLine();
	ImGui::Checkbox(ICON_FA_EYE " 뷰포트 미리보기", &bViewportPreview);
	ImGui::SetItemTooltip("재생 헤드 시점을 편집 씬에 적용 (끄거나 창을 닫으면 원래 값으로)");
	ImGui::SameLine();
	ImGui::BeginDisabled(!bViewportPreview);
	ImGui::Checkbox(ICON_FA_VIDEO " 카메라", &bCameraPreview);
	ImGui::EndDisabled();
	ImGui::SetItemTooltip("뷰포트를 지금 컷 카메라 시점으로 본다");
	ImGui::SameLine();
	if (ImGui::Button(ICON_FA_PLUS " 트랙"))
	{
		ImGui::OpenPopup("##AddTrack");
	}
	DrawAddTrackMenu(Env);
	ImGui::SameLine();
	if (ImGui::Button(ICON_FA_EXPAND))
	{
		bFitView = true;
	}
	ImGui::SetItemTooltip("전체 길이가 보이게");

	// 상태 줄: 바인딩 기준
	if (Env.Editor != nullptr && Env.Editor->bPlaying)
	{
		if (DebugTime >= 0.0f)
		{
			ImGui::TextColored(FEditorTheme::Success, ICON_FA_PLAY " 디버그: %s (초록 선)", DebugLabel.c_str());
		}
		else
		{
			ImGui::TextColored(FEditorTheme::Warning, "플레이 중 — 이 시퀀스를 재생하는 엔티티가 없습니다 (편집 씬 미리보기는 멈춤)");
		}
	}
	else if (const FScene* Scene = GetEditScene(Env); Scene != nullptr && PreviewPlayer.IsValid())
	{
		const FNameComponent* Name = Scene->GetRegistry().TryGet<FNameComponent>(PreviewPlayer);
		ImGui::TextDisabled(ICON_FA_CLAPPERBOARD " 재생 엔티티: %s (대상 이름은 이 엔티티 하위 → 씬 전체 순서로 찾음)", Name ? Name->Name.c_str() : "?");
	}
	else
	{
		ImGui::TextColored(FEditorTheme::Warning, ICON_FA_CLAPPERBOARD " 이 시퀀스를 쓰는 SequencePlayerComponent가 씬에 없습니다 — 대상은 씬 전체에서 찾습니다");
	}
}

void FSequenceEditor::DrawAddTrackMenu(FAssetEditorEnvironment& Env)
{
	if (!ImGui::BeginPopup("##AddTrack"))
	{
		return;
	}
	FScene*       Scene    = GetEditScene(Env);
	const FEntity Selected = Scene != nullptr && Scene->GetRegistry().IsValid(Env.Editor->SelectedEntity) ? Env.Editor->SelectedEntity : NullEntity;
	const std::string TargetPath = Selected.IsValid() ? FSequenceSystem::MakeBindingPath(*Scene, PreviewPlayer, Selected) : std::string();
	const std::string Shown      = Selected.IsValid() ? (TargetPath.empty() ? std::string("(재생 엔티티)") : TargetPath) : std::string("엔티티를 선택하세요");
	const auto        Add        = [&](FSequenceTrack Track, const char* Label) {
        Asset.Tracks.push_back(std::move(Track));
        SelectedTrack = static_cast<int32>(Asset.Tracks.size()) - 1;
        SelectedKey   = -1;
        MarkEdited(Label);
        ImGui::CloseCurrentPopup();
	};
	ImGui::TextDisabled("대상: %s", Shown.c_str());
	ImGui::Separator();
	if (ImGui::MenuItem(ICON_FA_UP_DOWN_LEFT_RIGHT " 트랜스폼", nullptr, false, Selected.IsValid()))
	{
		FSequenceTrack Track;
		Track.Type   = ESequenceTrackType::Transform;
		Track.Target = TargetPath;
		Add(std::move(Track), "트랜스폼 트랙 추가");
		AddKeyAtTime(Env, SelectedTrack, Snap(Time)); // 현재 값 첫 키
	}
	if (ImGui::BeginMenu(ICON_FA_SLIDERS " 프로퍼티", Selected.IsValid()))
	{
		FTypeRegistry::Get().ForEachComponentType([&](const FTypeInfo& Type) {
			if (!Type.HasComponent || !Type.HasComponent(Scene->GetRegistry(), Selected) || Type.Name == "TransformComponent")
			{
				return;
			}
			const bool bAny = std::any_of(Type.Properties.begin(), Type.Properties.end(), [](const FPropertyInfo& Property) { return IsAnimatable(Property); });
			if (bAny && ImGui::BeginMenu(Type.DisplayName.c_str()))
			{
				for (const FPropertyInfo& Property : Type.Properties)
				{
					if (IsAnimatable(Property) && ImGui::MenuItem(Property.DisplayName.c_str()))
					{
						FSequenceTrack Track;
						Track.Type      = ESequenceTrackType::Property;
						Track.Target    = TargetPath;
						Track.Component = Type.Name;
						Track.Property  = Property.Name;
						Add(std::move(Track), "프로퍼티 트랙 추가");
						AddKeyAtTime(Env, SelectedTrack, Snap(Time));
					}
				}
				ImGui::EndMenu();
			}
		});
		ImGui::EndMenu();
	}
	const bool bAnimated = Selected.IsValid() && Scene->GetRegistry().Has<FAnimationComponent>(Selected);
	if (ImGui::MenuItem(ICON_FA_PERSON_RUNNING " 애니메이션 클립", nullptr, false, bAnimated))
	{
		FSequenceTrack Track;
		Track.Type   = ESequenceTrackType::Animation;
		Track.Target = TargetPath;
		Add(std::move(Track), "애니메이션 트랙 추가");
		AddKeyAtTime(Env, SelectedTrack, Snap(Time));
	}
	if (!bAnimated && Selected.IsValid())
	{
		ImGui::SetItemTooltip("모델 루트(애니메이션 컴포넌트가 있는 엔티티)를 선택하세요");
	}
	const bool bHasCut = std::any_of(Asset.Tracks.begin(), Asset.Tracks.end(), [](const FSequenceTrack& Track) { return Track.Type == ESequenceTrackType::CameraCut; });
	if (ImGui::MenuItem(ICON_FA_VIDEO " 카메라 컷", nullptr, false, !bHasCut))
	{
		FSequenceTrack Track;
		Track.Type = ESequenceTrackType::CameraCut;
		Add(std::move(Track), "카메라 컷 트랙 추가");
		if (Selected.IsValid() && Scene->GetRegistry().Has<FCameraComponent>(Selected))
		{
			AddKeyAtTime(Env, SelectedTrack, Snap(Time));
		}
	}
	const bool bCamera = Selected.IsValid() && Scene->GetRegistry().Has<FCameraComponent>(Selected);
	if (ImGui::MenuItem(ICON_FA_CAMERA " 시야각 (선택 카메라)", nullptr, false, bCamera))
	{
		FSequenceTrack Track;
		Track.Type      = ESequenceTrackType::Property;
		Track.Target    = TargetPath;
		Track.Component = "CameraComponent";
		Track.Property  = "FovYDegrees";
		Add(std::move(Track), "시야각 트랙 추가");
		AddKeyAtTime(Env, SelectedTrack, Snap(Time));
	}
	if (ImGui::MenuItem(ICON_FA_BOLT " 이벤트"))
	{
		FSequenceTrack Track;
		Track.Type = ESequenceTrackType::Event;
		Add(std::move(Track), "이벤트 트랙 추가");
	}
	ImGui::EndPopup();
}

void FSequenceEditor::HandleShortcuts(FAssetEditorEnvironment& Env)
{
	if (!bTimelineFocused || ImGui::GetIO().WantTextInput)
	{
		return;
	}
	const float Frame = 1.0f / FMath::Max(Asset.FrameRate, 1.0f);
	if (ImGui::IsKeyPressed(ImGuiKey_Space, false))
	{
		bTimelinePlaying = !bTimelinePlaying;
	}
	if (ImGui::IsKeyPressed(ImGuiKey_Home, false))
	{
		Time = 0.0f;
	}
	if (ImGui::IsKeyPressed(ImGuiKey_End, false))
	{
		Time = Asset.Duration;
	}
	if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))
	{
		bTimelinePlaying = false;
		Time             = FMath::Max(Snap(Time) - Frame, 0.0f);
	}
	if (ImGui::IsKeyPressed(ImGuiKey_RightArrow))
	{
		bTimelinePlaying = false;
		Time             = FMath::Min(Snap(Time) + Frame, Asset.Duration);
	}
	if (ImGui::IsKeyPressed(ImGuiKey_K, false) && !ImGui::GetIO().KeyCtrl && SelectedTrack >= 0)
	{
		AddKeyAtTime(Env, SelectedTrack, Snap(Time));
	}
	if (ImGui::IsKeyPressed(ImGuiKey_Delete, false))
	{
		DeleteSelectedKey();
	}
	if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C))
	{
		CopySelectedKey();
	}
	if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_V))
	{
		PasteKey(Snap(Time));
	}
}

void FSequenceEditor::DrawPreviewArea(FAssetEditorEnvironment& Env)
{
	DrawToolbar(Env);
	DrawTimeline(Env);
	HandleShortcuts(Env);
}

void FSequenceEditor::DrawTimeline(FAssetEditorEnvironment& Env)
{
	const float RowHeight = ImGui::GetFrameHeight() + 8.0f;
	if (!ImGui::BeginChild("##Timeline", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollWithMouse))
	{
		ImGui::EndChild();
		return;
	}
	bTimelineFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
	ImDrawList*  Draw      = ImGui::GetWindowDrawList();
	const ImVec2 Origin    = ImGui::GetCursorScreenPos();
	const float  Width     = ImGui::GetContentRegionAvail().x;
	const float  LaneX0    = Origin.x + HeaderWidth;
	const float  LaneWidth = FMath::Max(Width - HeaderWidth, 50.0f);
	const float  Duration  = FMath::Max(Asset.Duration, 0.01f);
	if (bFitView)
	{
		PixelsPerSecond = FMath::Max((LaneWidth - 24.0f) / Duration, 4.0f);
		ViewStart       = 0.0f;
		bFitView        = false;
	}
	const auto ToX    = [&](float Seconds) { return LaneX0 + 8.0f + (Seconds - ViewStart) * PixelsPerSecond; };
	const auto ToTime = [&](float X) { return ViewStart + (X - LaneX0 - 8.0f) / PixelsPerSecond; };
	const ImVec2 Mouse = ImGui::GetIO().MousePos;
	const float  RowsBottom = Origin.y + RulerHeight + RowHeight * static_cast<float>(Asset.Tracks.size());
	const ImVec2 LaneClipMin(LaneX0, Origin.y);
	const ImVec2 LaneClipMax(LaneX0 + LaneWidth, FMath::Max(RowsBottom, Origin.y + ImGui::GetContentRegionAvail().y));

	// 휠: 좌우 이동, Ctrl+휠: 마우스 위치 기준 확대
	if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && Mouse.x >= LaneX0)
	{
		const float Wheel = ImGui::GetIO().MouseWheel;
		if (Wheel != 0.0f)
		{
			if (ImGui::GetIO().KeyCtrl)
			{
				const float Anchor = ToTime(Mouse.x);
				PixelsPerSecond    = FMath::Clamp(PixelsPerSecond * (1.0f + 0.15f * Wheel), 4.0f, 4000.0f);
				ViewStart          = Anchor - (Mouse.x - LaneX0 - 8.0f) / PixelsPerSecond;
			}
			else
			{
				ViewStart -= Wheel * 60.0f / PixelsPerSecond;
			}
		}
		if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
		{
			Drag = EDrag::Pan;
		}
	}
	if (Drag == EDrag::Pan)
	{
		ViewStart -= ImGui::GetIO().MouseDelta.x / PixelsPerSecond;
		if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle))
		{
			Drag = EDrag::None;
		}
	}
	ViewStart = FMath::Clamp(ViewStart, -1.0f, Duration);

	// ---- 눈금
	ImGui::SetCursorScreenPos(ImVec2(LaneX0, Origin.y));
	ImGui::InvisibleButton("##Ruler", ImVec2(LaneWidth, RulerHeight));
	if (ImGui::IsItemActivated())
	{
		Drag = EDrag::Playhead;
	}
	Draw->AddRectFilled(ImVec2(Origin.x, Origin.y), ImVec2(Origin.x + Width, Origin.y + RulerHeight), IM_COL32(32, 34, 38, 255));
	Draw->PushClipRect(LaneClipMin, LaneClipMax, true);
	{
		// 눈금 간격: 글자가 겹치지 않는 가장 작은 단위
		const float Steps[] = { 1.0f / FMath::Max(Asset.FrameRate, 1.0f), 0.05f, 0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 5.0f, 10.0f, 30.0f, 60.0f };
		float       Major   = Steps[std::size(Steps) - 1];
		for (float Step : Steps)
		{
			if (Step * PixelsPerSecond >= 70.0f)
			{
				Major = Step;
				break;
			}
		}
		const float Minor = Major / 5.0f;
		const float First = std::floor(FMath::Max(ViewStart, 0.0f) / Minor) * Minor;
		for (float Tick = First; Tick <= Duration + 1.0e-4f && ToX(Tick) <= LaneX0 + LaneWidth; Tick += Minor)
		{
			const float X       = ToX(Tick);
			const bool  bMajor  = std::fabs(std::remainder(Tick, Major)) < Minor * 0.25f;
			Draw->AddLine(ImVec2(X, Origin.y + (bMajor ? 10.0f : 18.0f)), ImVec2(X, Origin.y + RulerHeight), IM_COL32(150, 150, 150, bMajor ? 255 : 120));
			if (bMajor)
			{
				const std::string Label = std::format("{:g}", std::round(Tick * 100.0f) / 100.0f);
				Draw->AddText(ImVec2(X + 3.0f, Origin.y + 1.0f), IM_COL32(200, 200, 200, 255), Label.c_str());
			}
		}
	}
	Draw->PopClipRect();

	// ---- 트랙 행
	int32 HoverTrack = -1;
	int32 HoverKey   = -1;
	bool  bHoverEnd  = false;
	for (size_t TrackIndex = 0; TrackIndex < Asset.Tracks.size(); ++TrackIndex)
	{
		FSequenceTrack& Track  = Asset.Tracks[TrackIndex];
		const int32     Index  = static_cast<int32>(TrackIndex);
		const float     RowY   = Origin.y + RulerHeight + RowHeight * static_cast<float>(TrackIndex);
		const bool      bSelectedTrack = SelectedTrack == Index;
		ImGui::PushID(Index);
		Draw->AddRectFilled(ImVec2(Origin.x, RowY), ImVec2(Origin.x + Width, RowY + RowHeight),
		                    bSelectedTrack ? IM_COL32(40, 52, 70, 255) : (TrackIndex % 2 == 0 ? IM_COL32(26, 27, 30, 255) : IM_COL32(30, 31, 35, 255)));

		// 머리: 아이콘 + 이름 (클릭 선택, 우클릭 메뉴)
		ImGui::SetCursorScreenPos(ImVec2(Origin.x + 4.0f, RowY + 4.0f));
		ImGui::PushStyleColor(ImGuiCol_Text, Track.bMuted ? FEditorTheme::TextDim : ImVec4(0.92f, 0.92f, 0.92f, 1.0f));
		const std::string Header = std::format("{} {}", TrackIcon(Track.Type), TrackLabel(Track));
		if (ImGui::Selectable(Header.c_str(), bSelectedTrack, 0, ImVec2(HeaderWidth - 10.0f, RowHeight - 8.0f)))
		{
			SelectedTrack = Index;
			SelectedKey   = -1;
		}
		ImGui::PopStyleColor();
		if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
		{
			ContextTrack = Index;
			ImGui::OpenPopup("##TrackMenu");
		}
		if (Track.Type != ESequenceTrackType::CameraCut && Track.Type != ESequenceTrackType::Event && !ResolveTarget(Env, Track.Target).IsValid() &&
		    GetEditScene(Env) != nullptr)
		{
			ImGui::SetItemTooltip("대상 '%s'을(를) 편집 씬에서 찾을 수 없습니다", Track.Target.c_str());
			Draw->AddCircleFilled(ImVec2(Origin.x + HeaderWidth - 10.0f, RowY + RowHeight * 0.5f), 4.0f, ToU32(FEditorTheme::Danger));
		}

		// 레인: 키/구간
		ImGui::SetCursorScreenPos(ImVec2(LaneX0, RowY));
		ImGui::InvisibleButton("##Lane", ImVec2(LaneWidth, RowHeight), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
		const bool  bLaneHovered = ImGui::IsItemHovered();
		const bool  bLaneActivated = ImGui::IsItemActivated();
		const float MidY = RowY + RowHeight * 0.5f;
		Draw->PushClipRect(ImVec2(LaneX0, RowY), ImVec2(LaneX0 + LaneWidth, RowY + RowHeight), true);
		const size_t KeyCount = Track.GetKeyCount();
		for (size_t Key = 0; Key < KeyCount; ++Key)
		{
			const bool bSelected = bSelectedTrack && SelectedKey == static_cast<int32>(Key);
			const ImU32 Accent   = ToU32(FEditorTheme::Accent);
			if (Track.Type == ESequenceTrackType::Animation || Track.Type == ESequenceTrackType::CameraCut)
			{
				// 구간 막대 (카메라 컷은 다음 컷까지)
				float       Start = 0.0f, End = 0.0f;
				std::string Label;
				if (Track.Type == ESequenceTrackType::Animation)
				{
					Start = Track.Sections[Key].Start;
					End   = Track.Sections[Key].End;
					Label = Track.Sections[Key].Clip + (Track.Sections[Key].bLoop ? " " ICON_FA_REPEAT : "");
				}
				else
				{
					Start = Track.Cuts[Key].Time;
					End   = Key + 1 < Track.Cuts.size() ? Track.Cuts[Key + 1].Time : Asset.Duration;
					Label = ICON_FA_CAMERA " " + (Track.Cuts[Key].Camera.empty() ? std::string("(없음)") : Track.Cuts[Key].Camera);
				}
				const ImVec2 Min(ToX(Start), RowY + 4.0f);
				const ImVec2 Max(FMath::Max(ToX(End), Min.x + 4.0f), RowY + RowHeight - 4.0f);
				const ImU32  Fill = Track.Type == ESequenceTrackType::Animation ? IM_COL32(70, 110, 160, 220)
				                                                                : (Key % 2 == 0 ? IM_COL32(110, 80, 140, 220) : IM_COL32(90, 70, 120, 220));
				Draw->AddRectFilled(Min, Max, Fill, 3.0f);
				Draw->AddRect(Min, Max, bSelected ? IM_COL32_WHITE : IM_COL32(255, 255, 255, 60), 3.0f, 0, bSelected ? 2.0f : 1.0f);
				Draw->PushClipRect(Min, Max, true);
				Draw->AddText(ImVec2(Min.x + 5.0f, MidY - ImGui::GetTextLineHeight() * 0.5f), IM_COL32_WHITE, Label.c_str());
				Draw->PopClipRect();
				if (bLaneHovered && Mouse.x >= Min.x - 2.0f && Mouse.x <= Max.x + 2.0f)
				{
					HoverTrack = Index;
					HoverKey   = static_cast<int32>(Key);
					bHoverEnd  = Track.Type == ESequenceTrackType::Animation && std::fabs(Mouse.x - Max.x) < 6.0f;
				}
				continue;
			}
			// 키 마름모 (색 = 보간: 흰 곡선 / 초록 직선 / 주황 계단), 이벤트는 이름 표시
			const float Seconds = Track.GetKeyTime(Key);
			const float X       = ToX(Seconds);
			ImU32       Color   = IM_COL32(240, 200, 90, 255);
			if (Track.Type == ESequenceTrackType::Transform)
			{
				Color = InterpColor(Track.TransformKeys[Key].Interp);
			}
			else if (Track.Type == ESequenceTrackType::Property)
			{
				Color = InterpColor(Track.ValueKeys[Key].Interp);
			}
			const float Radius = bSelected ? KeyRadius + 2.0f : KeyRadius;
			Draw->AddQuadFilled(ImVec2(X, MidY - Radius), ImVec2(X + Radius, MidY), ImVec2(X, MidY + Radius), ImVec2(X - Radius, MidY), bSelected ? Accent : Color);
			Draw->AddQuad(ImVec2(X, MidY - Radius), ImVec2(X + Radius, MidY), ImVec2(X, MidY + Radius), ImVec2(X - Radius, MidY), IM_COL32(0, 0, 0, 200));
			if (Track.Type == ESequenceTrackType::Event)
			{
				Draw->AddText(ImVec2(X + KeyRadius + 3.0f, MidY - ImGui::GetTextLineHeight() * 0.5f), IM_COL32(230, 230, 230, 255), Track.Events[Key].Name.c_str());
			}
			if (bLaneHovered && std::fabs(Mouse.x - X) <= KeyRadius + 2.0f)
			{
				HoverTrack = Index;
				HoverKey   = static_cast<int32>(Key);
				bHoverEnd  = false;
			}
		}
		Draw->PopClipRect();

		// 레인 조작: 키 잡기 / 빈 곳 = 재생 헤드 / 더블클릭 = 키 추가 / 우클릭 = 메뉴
		if (bLaneActivated && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
		{
			SelectedTrack = Index;
			if (HoverTrack == Index && HoverKey >= 0)
			{
				SelectedKey    = HoverKey;
				Drag           = bHoverEnd ? EDrag::SectionEnd : EDrag::Key;
				DragTrack      = Index;
				DragGrabOffset = Track.GetKeyTime(static_cast<size_t>(HoverKey)) - ToTime(Mouse.x);
			}
			else
			{
				SelectedKey = -1;
				Drag        = EDrag::Playhead;
			}
		}
		if (bLaneHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && HoverKey < 0)
		{
			Drag = EDrag::None;
			AddKeyAtTime(Env, Index, Snap(ToTime(Mouse.x)));
		}
		if (bLaneHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
		{
			ContextTrack = Index;
			ContextKey   = HoverTrack == Index ? HoverKey : -1;
			ContextTime  = Snap(ToTime(Mouse.x));
			if (ContextKey >= 0)
			{
				SelectedTrack = Index;
				SelectedKey   = ContextKey;
			}
			ImGui::OpenPopup("##LaneMenu");
		}
		ImGui::PopID();
		if (bLaneHovered && HoverTrack == Index && HoverKey >= 0 && Drag == EDrag::None)
		{
			ImGui::SetMouseCursor(bHoverEnd ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_Hand);
		}
	}

	// ---- 끌기
	if (Drag == EDrag::Playhead)
	{
		bTimelinePlaying = false;
		Time             = FMath::Clamp(Snap(ToTime(Mouse.x)), 0.0f, Asset.Duration);
		if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
		{
			Drag = EDrag::None;
		}
	}
	else if ((Drag == EDrag::Key || Drag == EDrag::SectionEnd) && DragTrack >= 0 && DragTrack < static_cast<int32>(Asset.Tracks.size()))
	{
		FSequenceTrack& Track = Asset.Tracks[static_cast<size_t>(DragTrack)];
		if (SelectedTrack == DragTrack && SelectedKey >= 0 && SelectedKey < static_cast<int32>(Track.GetKeyCount()) && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f))
		{
			if (Drag == EDrag::SectionEnd)
			{
				FSequenceAnimSection& Section = Track.Sections[static_cast<size_t>(SelectedKey)];
				const float           End     = FMath::Clamp(Snap(ToTime(Mouse.x)), Section.Start + 1.0f / FMath::Max(Asset.FrameRate, 1.0f), Asset.Duration);
				if (End != Section.End)
				{
					Section.End = End;
					MarkEdited("구간 길이");
				}
			}
			else
			{
				const float Target = Snap(ToTime(Mouse.x) + DragGrabOffset);
				if (std::fabs(Target - Track.GetKeyTime(static_cast<size_t>(SelectedKey))) > 1.0e-5f)
				{
					SetKeyTime(Track, SelectedKey, Target);
					MarkEdited("키 이동");
				}
			}
		}
		if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
		{
			Drag = EDrag::None;
		}
	}

	// ---- 길이 끝 / 재생 헤드
	const float BottomY = FMath::Max(RowsBottom, Origin.y + RulerHeight + RowHeight);
	Draw->PushClipRect(LaneClipMin, ImVec2(LaneClipMax.x, FMath::Max(LaneClipMax.y, BottomY)), true);
	const float EndX = ToX(Asset.Duration);
	Draw->AddRectFilled(ImVec2(EndX, Origin.y + RulerHeight), ImVec2(LaneX0 + LaneWidth, BottomY), IM_COL32(0, 0, 0, 90));
	Draw->AddLine(ImVec2(EndX, Origin.y), ImVec2(EndX, BottomY), IM_COL32(200, 200, 200, 120), 1.0f);
	if (DebugTime >= 0.0f)
	{
		const float DebugX = ToX(DebugTime);
		Draw->AddLine(ImVec2(DebugX, Origin.y), ImVec2(DebugX, BottomY), ToU32(FEditorTheme::Success), 2.0f);
	}
	const float HeadX = ToX(Time);
	Draw->AddLine(ImVec2(HeadX, Origin.y), ImVec2(HeadX, BottomY), ToU32(FEditorTheme::Danger), 2.0f);
	Draw->AddTriangleFilled(ImVec2(HeadX - 6.0f, Origin.y), ImVec2(HeadX + 6.0f, Origin.y), ImVec2(HeadX, Origin.y + 9.0f), ToU32(FEditorTheme::Danger));
	Draw->PopClipRect();

	// 머리/레인 경계
	Draw->AddLine(ImVec2(LaneX0, Origin.y), ImVec2(LaneX0, BottomY), IM_COL32(255, 255, 255, 40));
	ImGui::SetCursorScreenPos(ImVec2(Origin.x, BottomY + 6.0f));
	if (Asset.Tracks.empty())
	{
		ImGui::TextDisabled("트랙이 없습니다 — 엔티티를 선택하고 위의 '+ 트랙'으로 추가하세요");
	}
	else
	{
		ImGui::TextDisabled("빈 곳 더블클릭: 현재 값으로 키 · K: 재생 헤드에 키 · 끌기: 이동 (Alt: 스냅 끔) · Ctrl+휠: 확대 · 휠/가운데 끌기: 이동");
	}

	// ---- 메뉴
	if (ImGui::BeginPopup("##TrackMenu"))
	{
		if (ContextTrack >= 0 && ContextTrack < static_cast<int32>(Asset.Tracks.size()))
		{
			FSequenceTrack& Track = Asset.Tracks[static_cast<size_t>(ContextTrack)];
			if (ImGui::MenuItem(ICON_FA_KEY " 재생 헤드에 키 (현재 값)", "K"))
			{
				AddKeyAtTime(Env, ContextTrack, Snap(Time));
			}
			if (ImGui::MenuItem(Track.bMuted ? ICON_FA_EYE " 다시 켜기" : ICON_FA_EYE " 끄기 (음소거)"))
			{
				Track.bMuted = !Track.bMuted;
				MarkEdited("트랙 끄기");
			}
			if (ImGui::MenuItem(ICON_FA_COPY " 트랙 복제"))
			{
				FSequenceTrack Copy = Asset.Tracks[static_cast<size_t>(ContextTrack)];
				Asset.Tracks.insert(Asset.Tracks.begin() + ContextTrack + 1, std::move(Copy));
				SelectedTrack = ContextTrack + 1;
				MarkEdited("트랙 복제");
			}
			if (ImGui::MenuItem(ICON_FA_ARROW_UP " 위로", nullptr, false, ContextTrack > 0))
			{
				std::swap(Asset.Tracks[static_cast<size_t>(ContextTrack)], Asset.Tracks[static_cast<size_t>(ContextTrack - 1)]);
				SelectedTrack = ContextTrack - 1;
				MarkEdited("트랙 순서");
			}
			if (ImGui::MenuItem(ICON_FA_ARROW_DOWN " 아래로", nullptr, false, ContextTrack + 1 < static_cast<int32>(Asset.Tracks.size())))
			{
				std::swap(Asset.Tracks[static_cast<size_t>(ContextTrack)], Asset.Tracks[static_cast<size_t>(ContextTrack + 1)]);
				SelectedTrack = ContextTrack + 1;
				MarkEdited("트랙 순서");
			}
			ImGui::Separator();
			if (ImGui::MenuItem(ICON_FA_TRASH " 트랙 삭제"))
			{
				Asset.Tracks.erase(Asset.Tracks.begin() + ContextTrack);
				SelectedTrack = -1;
				SelectedKey   = -1;
				MarkEdited("트랙 삭제");
			}
		}
		ImGui::EndPopup();
	}
	if (ImGui::BeginPopup("##LaneMenu"))
	{
		if (ContextTrack >= 0 && ContextTrack < static_cast<int32>(Asset.Tracks.size()))
		{
			FSequenceTrack& Track = Asset.Tracks[static_cast<size_t>(ContextTrack)];
			if (ContextKey >= 0 && ContextKey < static_cast<int32>(Track.GetKeyCount()))
			{
				ESequenceInterp* Interp = Track.Type == ESequenceTrackType::Transform ? &Track.TransformKeys[static_cast<size_t>(ContextKey)].Interp
				                          : Track.Type == ESequenceTrackType::Property ? &Track.ValueKeys[static_cast<size_t>(ContextKey)].Interp
				                                                                       : nullptr;
				if (Interp != nullptr)
				{
					for (int32 Mode = 0; Mode < 3; ++Mode)
					{
						if (ImGui::MenuItem(InterpLabels[Mode], nullptr, static_cast<int32>(*Interp) == Mode))
						{
							*Interp = static_cast<ESequenceInterp>(Mode);
							MarkEdited("보간 방식");
						}
					}
					ImGui::Separator();
				}
				if (ImGui::MenuItem(ICON_FA_COPY " 복사", "Ctrl+C"))
				{
					CopySelectedKey();
				}
				if (ImGui::MenuItem(ICON_FA_TRASH " 삭제", "Del"))
				{
					DeleteSelectedKey();
				}
			}
			else
			{
				if (ImGui::MenuItem(ICON_FA_KEY " 여기에 키 (현재 값)"))
				{
					AddKeyAtTime(Env, ContextTrack, ContextTime);
				}
				const bool bCanPaste = KeyClipboard && KeyClipboard->Type == Track.Type;
				if (ImGui::MenuItem(ICON_FA_PASTE " 여기에 붙여넣기", "Ctrl+V", false, bCanPaste))
				{
					SelectedTrack = ContextTrack;
					PasteKey(ContextTime);
				}
			}
		}
		ImGui::EndPopup();
	}
	ImGui::EndChild();
}

// ---------------------------------------------------------------- 속성

void FSequenceEditor::DrawProperties(FAssetEditorEnvironment& Env)
{
	if (ImGui::CollapsingHeader(ICON_FA_CLAPPERBOARD " 시퀀스", ImGuiTreeNodeFlags_DefaultOpen))
	{
		if (ImGui::DragFloat("길이 (초)", &Asset.Duration, 0.05f, 0.1f, 3600.0f, "%.2f"))
		{
			Asset.Duration = FMath::Max(Asset.Duration, 0.1f);
			MarkEdited("시퀀스 길이");
		}
		if (ImGui::DragFloat("프레임 (fps)", &Asset.FrameRate, 0.5f, 1.0f, 240.0f, "%.0f"))
		{
			MarkEdited("프레임");
		}
		ImGui::SetItemTooltip("눈금과 키 스냅 간격 (재생은 연속 시간)");
		ImGui::TextDisabled(ICON_FA_CIRCLE_QUESTION " 게임에서 재생하려면");
		ImGui::SetItemTooltip("씬 엔티티에 SequencePlayerComponent(시퀀스 = 이 파일)를 단다 (자동 재생/반복/속도).\n"
		                      "Lua: entity:PlaySequence(), 이벤트는 그 엔티티 스크립트의 OnSequenceEvent_<이름>(), 끝나면 OnSequenceFinished().");
	}
	if (SelectedTrack < 0 || SelectedTrack >= static_cast<int32>(Asset.Tracks.size()))
	{
		FAssetEditorWidgets::Hint("타임라인에서 트랙이나 키를 선택하세요.");
		return;
	}
	FSequenceTrack& Track = Asset.Tracks[static_cast<size_t>(SelectedTrack)];
	if (ImGui::CollapsingHeader((std::string(TrackIcon(Track.Type)) + " 트랙##TrackHeader").c_str(), ImGuiTreeNodeFlags_DefaultOpen))
	{
		DrawTrackProperties(Env, Track);
	}
	FSequenceTrack& Edited = Asset.Tracks[static_cast<size_t>(SelectedTrack)];
	if (SelectedKey >= 0 && SelectedKey < static_cast<int32>(Edited.GetKeyCount()) &&
	    ImGui::CollapsingHeader(ICON_FA_KEY " 선택 키", ImGuiTreeNodeFlags_DefaultOpen))
	{
		DrawKeyProperties(Env, Edited, SelectedKey);
	}
}

void FSequenceEditor::DrawTrackProperties(FAssetEditorEnvironment& Env, FSequenceTrack& Track)
{
	ImGui::TextDisabled("%s 트랙 · 키 %zu개", ToString(Track.Type), Track.GetKeyCount());
	if (InputString("표시 이름", Track.Name))
	{
		MarkEdited("트랙 이름");
	}
	if (Track.Type != ESequenceTrackType::CameraCut && Track.Type != ESequenceTrackType::Event)
	{
		if (InputString("대상", Track.Target))
		{
			MarkEdited("트랙 대상");
		}
		ImGui::SetItemTooltip("엔티티 이름 경로: 비면 재생 엔티티 자신, \"부모/자식\"으로 같은 이름을 구분");
		FScene*       Scene    = GetEditScene(Env);
		const FEntity Resolved = ResolveTarget(Env, Track.Target);
		if (Scene != nullptr)
		{
			if (Resolved.IsValid())
			{
				ImGui::TextColored(FEditorTheme::Success, ICON_FA_LINK " 찾음");
			}
			else
			{
				ImGui::TextColored(FEditorTheme::Danger, ICON_FA_LINK " 편집 씬에서 찾을 수 없음");
			}
			ImGui::SameLine();
			const bool bHasSelection = Scene->GetRegistry().IsValid(Env.Editor->SelectedEntity);
			ImGui::BeginDisabled(!bHasSelection);
			if (ImGui::SmallButton("선택한 엔티티로"))
			{
				Track.Target = FSequenceSystem::MakeBindingPath(*Scene, PreviewPlayer, Env.Editor->SelectedEntity);
				MarkEdited("트랙 대상");
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::BeginDisabled(!Resolved.IsValid());
			if (ImGui::SmallButton("대상 선택"))
			{
				Env.Editor->Select(Resolved);
			}
			ImGui::EndDisabled();
		}
		if (Track.Type == ESequenceTrackType::Property)
		{
			// 컴포넌트/프로퍼티: 대상에 있는 컴포넌트의 키를 넣을 수 있는 프로퍼티
			const FTypeInfo*     Type     = nullptr;
			const FPropertyInfo* Property = FindTrackProperty(Track, &Type);
			const std::string    Current  = Type != nullptr && Property != nullptr ? Type->DisplayName + " / " + Property->DisplayName : Track.Component + "." + Track.Property;
			if (ImGui::BeginCombo("프로퍼티", Current.c_str()))
			{
				FTypeRegistry::Get().ForEachComponentType([&](const FTypeInfo& Candidate) {
					if (Scene != nullptr && Resolved.IsValid() && Candidate.HasComponent && !Candidate.HasComponent(Scene->GetRegistry(), Resolved))
					{
						return;
					}
					for (const FPropertyInfo& Item : Candidate.Properties)
					{
						if (!IsAnimatable(Item))
						{
							continue;
						}
						const std::string Label    = Candidate.DisplayName + " / " + Item.DisplayName + "##" + Candidate.Name + "." + Item.Name;
						const bool        bCurrent = Candidate.Name == Track.Component && Item.Name == Track.Property;
						if (ImGui::Selectable(Label.c_str(), bCurrent))
						{
							Track.Component = Candidate.Name;
							Track.Property  = Item.Name;
							MarkEdited("트랙 프로퍼티");
						}
					}
				});
				ImGui::EndCombo();
			}
			if (Property == nullptr)
			{
				ImGui::TextColored(FEditorTheme::Danger, "알 수 없는 프로퍼티: %s.%s", Track.Component.c_str(), Track.Property.c_str());
			}
		}
	}
	if (ImGui::Checkbox("끄기 (음소거)", &Track.bMuted))
	{
		MarkEdited("트랙 끄기");
	}
	if (ImGui::Button(ICON_FA_KEY " 재생 헤드에 키 (K)"))
	{
		AddKeyAtTime(Env, SelectedTrack, Snap(Time));
	}
	ImGui::SameLine();
	if (ImGui::Button(ICON_FA_TRASH " 트랙 삭제"))
	{
		Asset.Tracks.erase(Asset.Tracks.begin() + SelectedTrack);
		SelectedTrack = -1;
		SelectedKey   = -1;
		MarkEdited("트랙 삭제");
	}
}

void FSequenceEditor::DrawKeyProperties(FAssetEditorEnvironment& Env, FSequenceTrack& Track, int32 Key)
{
	const size_t Index = static_cast<size_t>(Key);
	float        Seconds = Track.GetKeyTime(Index);
	if (ImGui::DragFloat(Track.Type == ESequenceTrackType::Animation ? "시작 (초)" : "시각 (초)", &Seconds, 0.01f, 0.0f, Asset.Duration, "%.3f"))
	{
		SetKeyTime(Track, SelectedKey, Seconds);
		MarkEdited("키 시각");
		return; // 번호가 바뀌었을 수 있다
	}
	const auto InterpCombo = [&](ESequenceInterp& Interp) {
		int32 Mode = static_cast<int32>(Interp);
		if (ImGui::Combo("보간 (다음 키까지)", &Mode, InterpLabels, IM_ARRAYSIZE(InterpLabels)))
		{
			Interp = static_cast<ESequenceInterp>(Mode);
			MarkEdited("보간 방식");
		}
	};
	switch (Track.Type)
	{
	case ESequenceTrackType::Transform:
	{
		FSequenceTransformKey& Item = Track.TransformKeys[Index];
		if (ImGui::DragFloat3("위치", &Item.Position.X, 1.0f))
		{
			MarkEdited("키 위치");
		}
		if (ImGui::DragFloat3("회전 (P/Y/R)", &Item.Rotation.X, 0.5f))
		{
			MarkEdited("키 회전");
		}
		if (ImGui::DragFloat3("스케일", &Item.Scale.X, 0.01f))
		{
			MarkEdited("키 스케일");
		}
		InterpCombo(Item.Interp);
		if (ImGui::Button("현재 값으로"))
		{
			if (CaptureTransform(Env, Track, Item.Time, Item))
			{
				MarkEdited("키 현재 값");
			}
		}
		ImGui::SetItemTooltip("편집 씬의 대상 트랜스폼(로컬)을 이 키에 넣는다");
		break;
	}
	case ESequenceTrackType::Property:
	{
		FSequenceValueKey&   Item     = Track.ValueKeys[Index];
		const FPropertyInfo* Property = FindTrackProperty(Track, nullptr);
		const EPropertyType  Type     = Property != nullptr ? Property->Type : EPropertyType::Float;
		bool                 bChanged = false;
		if (Type == EPropertyType::Bool)
		{
			bool bValue = Item.Value.X >= 0.5f;
			if (ImGui::Checkbox("값", &bValue))
			{
				Item.Value.X = bValue ? 1.0f : 0.0f;
				bChanged     = true;
			}
		}
		else if (Property != nullptr && Property->HasFlag(PF_Color) && (Type == EPropertyType::Vector3 || Type == EPropertyType::Vector4))
		{
			bChanged = Type == EPropertyType::Vector3 ? ImGui::ColorEdit3("값", &Item.Value.X) : ImGui::ColorEdit4("값", &Item.Value.X);
		}
		else
		{
			bChanged = ImGui::DragScalarN("값", ImGuiDataType_Float, &Item.Value.X, ComponentCount(Type), Property != nullptr && Property->Step > 0.0f ? Property->Step : 0.05f);
		}
		if (bChanged)
		{
			MarkEdited("키 값");
		}
		if (Type == EPropertyType::Bool || Type == EPropertyType::Int32 || Type == EPropertyType::UInt32)
		{
			ImGui::TextDisabled("정수/bool 프로퍼티는 항상 계단으로 바뀝니다");
		}
		else
		{
			InterpCombo(Item.Interp);
		}
		if (ImGui::Button("현재 값으로"))
		{
			if (CaptureValue(Env, Track, Item.Value))
			{
				MarkEdited("키 현재 값");
			}
		}
		break;
	}
	case ESequenceTrackType::CameraCut:
	{
		FSequenceCameraCut& Cut = Track.Cuts[Index];
		if (InputString("카메라", Cut.Camera))
		{
			MarkEdited("컷 카메라");
		}
		if (FScene* Scene = GetEditScene(Env); Scene != nullptr)
		{
			// 씬의 카메라 엔티티 목록에서 고르기
			if (ImGui::BeginCombo("##CameraList", "씬의 카메라에서 고르기"))
			{
				Scene->GetRegistry().View<FCameraComponent>().Each([&](FEntity Entity, FCameraComponent&) {
					const std::string Binding = FSequenceSystem::MakeBindingPath(*Scene, PreviewPlayer, Entity);
					if (ImGui::Selectable(Binding.empty() ? "(재생 엔티티)" : Binding.c_str(), Binding == Cut.Camera))
					{
						Cut.Camera = Binding;
						MarkEdited("컷 카메라");
					}
				});
				ImGui::EndCombo();
			}
			if (!ResolveTarget(Env, Cut.Camera).IsValid())
			{
				ImGui::TextColored(FEditorTheme::Danger, "카메라를 찾을 수 없습니다");
			}
		}
		FAssetEditorWidgets::Hint("다음 컷까지 이 카메라가 주 카메라가 된다. 시야각은 '+ 트랙 → 시야각' (CameraComponent.FovYDegrees 키).");
		break;
	}
	case ESequenceTrackType::Animation:
	{
		FSequenceAnimSection& Section = Track.Sections[Index];
		if (ImGui::DragFloat("끝 (초)", &Section.End, 0.01f, Section.Start, Asset.Duration, "%.3f"))
		{
			Section.End = FMath::Max(Section.End, Section.Start);
			MarkEdited("구간 끝");
		}
		std::vector<std::string> Clips;
		if (FScene* Scene = GetEditScene(Env))
		{
			Clips = FAnimationSystem::GetClipNames(*Scene, ResolveTarget(Env, Track.Target));
		}
		if (ImGui::BeginCombo("클립", Section.Clip.empty() ? "(없음)" : Section.Clip.c_str()))
		{
			for (const std::string& Clip : Clips)
			{
				if (ImGui::Selectable(Clip.c_str(), Clip == Section.Clip))
				{
					Section.Clip = Clip;
					MarkEdited("구간 클립");
				}
			}
			ImGui::EndCombo();
		}
		if (!Clips.empty() && std::find(Clips.begin(), Clips.end(), Section.Clip) == Clips.end())
		{
			ImGui::TextColored(FEditorTheme::Warning, "대상 모델에 이 클립이 없습니다");
		}
		if (ImGui::DragFloat("클립 시작 위치 (초)", &Section.Offset, 0.01f, 0.0f, 600.0f))
		{
			MarkEdited("클립 시작 위치");
		}
		if (ImGui::DragFloat("배속", &Section.Rate, 0.01f, 0.0f, 10.0f))
		{
			MarkEdited("구간 배속");
		}
		if (ImGui::Checkbox("반복", &Section.bLoop))
		{
			MarkEdited("구간 반복");
		}
		FAssetEditorWidgets::Hint("구간 동안 모델 루트의 클립 재생 위치를 시퀀스가 정한다 (애니메이션 그래프가 붙은 모델은 제외). 마지막 구간이 끝나면 끝 포즈 유지.");
		break;
	}
	case ESequenceTrackType::Event:
	{
		FSequenceEventKey& Event = Track.Events[Index];
		if (InputString("이름", Event.Name))
		{
			MarkEdited("이벤트 이름");
		}
		ImGui::TextDisabled("Lua: function T:OnSequenceEvent_%s() ... end", Event.Name.c_str());
		break;
	}
	}
	ImGui::Spacing();
	if (ImGui::SmallButton(ICON_FA_COPY " 재생 헤드에 복제"))
	{
		CopySelectedKey();
		PasteKey(Snap(Time));
	}
	ImGui::SameLine();
	if (ImGui::SmallButton(ICON_FA_TRASH " 키 삭제"))
	{
		DeleteSelectedKey();
	}
}
