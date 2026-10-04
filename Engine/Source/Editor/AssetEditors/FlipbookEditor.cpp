#include "Editor/AssetEditors/FlipbookEditor.h"

#include "Core/CommandLine.h"
#include "Core/Log.h"
#include "Core/StringConv.h"
#include "Editor/AssetEditors/AssetEditorWidgets.h"
#include "Editor/AssetEditors/DataValueWidgets.h"
#include "Editor/AssetEditors/Sprite2DEditing.h"
#include "Editor/EditorTheme.h"
#include "Scene/Sprite/Sprite2DLibrary.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <format>

E_DECLARE_LOG_CATEGORY(LogEditor)

namespace
{
	constexpr const char* FramePayload = "E_FLIPBOOK_FRAMES";
	constexpr const char* SlicePayload = "E_FLIPBOOK_SLICES";
	constexpr float       EventFlashSeconds = 1.2f;

	constexpr ImU32 FrameBlockColor    = IM_COL32(60, 66, 80, 255);
	constexpr ImU32 FrameBlockSelected = IM_COL32(0, 110, 210, 255);
	constexpr ImU32 FrameBlockCurrent  = IM_COL32(80, 92, 116, 255);
	constexpr ImU32 PlayheadColor      = IM_COL32(255, 80, 70, 255);
	constexpr ImU32 EventColor         = IM_COL32(255, 210, 60, 255);
	constexpr ImU32 PivotColor         = IM_COL32(255, 210, 60, 200);
} // namespace

// ---------------------------------------------------------------- 에셋

bool FFlipbookEditor::LoadAsset(FAssetEditorEnvironment& Env)
{
	std::string Text;
	if (!ReadAssetText(Text))
	{
		E_LOG(LogEditor, Error, "플립북을 읽을 수 없습니다: {}", GetDisplayName());
		return false;
	}
	FFlipbookAsset           Loaded;
	std::vector<std::string> Warnings;
	std::string              Error;
	if (!FFlipbookAsset::FromJsonString(Text, Loaded, &Warnings, &Error))
	{
		E_LOG(LogEditor, Error, "플립북 형식 오류 ({}): {}", GetDisplayName(), Error);
		return false;
	}
	for (const std::string& Warning : Warnings)
	{
		E_LOG(LogEditor, Warning, "[2D 편집기] {}: {}", GetDisplayName(), Warning);
	}
	Asset = std::move(Loaded);
	SyncAtlas(Env, false);
	ClampSelection();
	RememberDiskState();
	return true;
}

std::string FFlipbookEditor::CaptureLibraryState() const
{
	const std::shared_ptr<const FFlipbookAsset> Loaded = FSprite2DLibrary::Get().LoadFlipbook(GetAssetPathString());
	return Loaded != nullptr ? Loaded->ToJsonString() : std::string();
}

void FFlipbookEditor::PushLivePreview()
{
	std::shared_ptr<FFlipbookAsset> Copy = std::make_shared<FFlipbookAsset>(Asset);
	Copy->RebuildTimeline(); // 프레임 길이 표 (직렬화 안 하는 파생 값)
	FSprite2DLibrary::Get().SetFlipbookPreview(GetAssetPathString(), std::move(Copy));
}

bool FFlipbookEditor::SaveAsset(FAssetEditorEnvironment& Env)
{
	(void)Env;
	std::string Error;
	if (!FSprite2DLibrary::Get().SaveFlipbook(GetAssetPathString(), Asset, &Error))
	{
		E_LOG(LogEditor, Error, "플립북 저장 실패: {}", Error);
		return false;
	}
	AfterSaved();
	return true;
}

std::string FFlipbookEditor::CaptureState() const
{
	return Asset.ToJsonString();
}

void FFlipbookEditor::RestoreState(FAssetEditorEnvironment& Env, const std::string& State)
{
	FFlipbookAsset Restored;
	if (FFlipbookAsset::FromJsonString(State, Restored))
	{
		Asset = std::move(Restored);
		SyncAtlas(Env, false);
	}
	ClampSelection();
}

void FFlipbookEditor::OnClose(FAssetEditorEnvironment& Env)
{
	ReleaseTexturePreview(Env, Texture);
	FSprite2DEditorBase::OnClose(Env);
}

void FFlipbookEditor::OnSprite2DLibraryChanged(FAssetEditorEnvironment& Env)
{
	SyncAtlas(Env, true); // 대상 아틀라스가 저장·핫 리로드됐을 수 있다
}

void FFlipbookEditor::Edited(std::string_view Label)
{
	Asset.RebuildTimeline();
	MarkEdited(Label);
}

void FFlipbookEditor::SyncAtlas(FAssetEditorEnvironment& Env, bool bForce)
{
	const std::string NewPath = ResolveReference(Asset.Sprite);
	if (bForce || NewPath != AtlasPath || (Atlas == nullptr && !NewPath.empty()))
	{
		AtlasPath = NewPath;
		Atlas     = NewPath.empty() ? nullptr : FSprite2DLibrary::Get().LoadSprite(NewPath);
		Canvas.RequestFrame();
	}
	const std::string TexturePath = Atlas != nullptr ? FSprite2DLibrary::ResolveReference(AtlasPath, Atlas->Texture) : std::string();
	UpdateTexturePreview(Env, Texture, TexturePath);
}

void FFlipbookEditor::ClampSelection()
{
	FrameSelection = Sprite2DEditing::NormalizeSelection(FrameSelection, static_cast<int32>(Asset.Frames.size()));
}

bool FFlipbookEditor::IsFrameSelected(int32 Frame) const
{
	return std::find(FrameSelection.begin(), FrameSelection.end(), Frame) != FrameSelection.end();
}

// ---------------------------------------------------------------- 재생

void FFlipbookEditor::Update(FAssetEditorEnvironment& Env, float DeltaSeconds)
{
	FSprite2DEditorBase::Update(Env, DeltaSeconds);
	for (FRecentEvent& Event : RecentEvents)
	{
		Event.Age += DeltaSeconds;
	}
	std::erase_if(RecentEvents, [](const FRecentEvent& Event) { return Event.Age > EventFlashSeconds; });
	if (!bPlaying || Asset.Frames.empty())
	{
		return;
	}
	const float        Previous = Time;
	Time += DeltaSeconds * Speed;
	std::vector<int32> Fired;
	FlipbookMath::CollectEvents(Previous, Time, Asset.FrameDurations, Asset.Loop, Asset.Events, false, Fired);
	for (const int32 Index : Fired)
	{
		const FFlipbookEvent& Event = Asset.Events[static_cast<size_t>(Index)];
		RecentEvents.push_back(FRecentEvent{ Event.Name, Event.Frame, 0.0f });
	}
	Time = FlipbookMath::WrapTime(Time, Asset.FrameDurations, Asset.Loop);
	if (Asset.Loop == EFlipbookLoopMode::Once && (Time >= Asset.TotalDuration || Time < 0.0f))
	{
		Time     = FMath::Clamp(Time, 0.0f, Asset.TotalDuration);
		bPlaying = false;
	}
}

int32 FFlipbookEditor::GetCurrentFrame() const
{
	return FlipbookMath::Evaluate(Time, Asset.FrameDurations, Asset.Loop).Frame;
}

float FFlipbookEditor::GetFrameStart(int32 Frame) const
{
	float Start = 0.0f;
	for (int32 Index = 0; Index < Frame && Index < static_cast<int32>(Asset.FrameDurations.size()); ++Index)
	{
		Start += Asset.FrameDurations[static_cast<size_t>(Index)];
	}
	return Start;
}

void FFlipbookEditor::SeekFrame(int32 Frame)
{
	const int32 Count = static_cast<int32>(Asset.Frames.size());
	if (Count == 0)
	{
		return;
	}
	Frame    = ((Frame % Count) + Count) % Count;
	Time     = GetFrameStart(Frame);
	bPlaying = false;
}

// ---------------------------------------------------------------- 왼쪽

void FFlipbookEditor::DrawPreviewArea(FAssetEditorEnvironment& Env)
{
	DrawExternalChangeBanner(Env);
	SyncAtlas(Env, false);
	if (!bCheckedArgs)
	{
		bCheckedArgs = true;
		if (const std::wstring TimeArg = FCommandLine::FromProcess().GetValue(L"--flipbook-time"); !TimeArg.empty())
		{
			Time     = std::stof(TimeArg);
			bPlaying = false;
		}
	}
	DrawToolbar();
	const float TimelineHeight = 58.0f;
	const ImVec2 Avail         = ImGui::GetContentRegionAvail();
	ImGui::PushID("FlipbookCanvas");
	ImGui::BeginChild("##CanvasArea", ImVec2(0.0f, FMath::Max(Avail.y - TimelineHeight - ImGui::GetStyle().ItemSpacing.y, 64.0f)));
	DrawCanvas(Env);
	ImGui::EndChild();
	ImGui::PopID();
	DrawTimeline();
}

void FFlipbookEditor::DrawToolbar()
{
	if (ImGui::Button(bPlaying ? ICON_FA_PAUSE "##Play" : ICON_FA_PLAY "##Play"))
	{
		if (!bPlaying && Asset.Loop == EFlipbookLoopMode::Once && Time >= Asset.TotalDuration)
		{
			Time = 0.0f; // 끝난 Once는 처음부터
		}
		bPlaying = !bPlaying;
	}
	ImGui::SetItemTooltip(bPlaying ? "정지" : "재생");
	ImGui::SameLine();
	if (ImGui::Button(ICON_FA_BACKWARD_STEP))
	{
		SeekFrame(GetCurrentFrame() - 1);
	}
	ImGui::SetItemTooltip("이전 프레임");
	ImGui::SameLine();
	if (ImGui::Button(ICON_FA_FORWARD_STEP))
	{
		SeekFrame(GetCurrentFrame() + 1);
	}
	ImGui::SetItemTooltip("다음 프레임");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(120.0f);
	ImGui::SliderFloat("속도", &Speed, -4.0f, 4.0f, "%.2fx");
	ImGui::SetItemTooltip("재생 속도 (음수 = 역재생). 미리보기 전용 — 저장하지 않는다");
	ImGui::SameLine();
	ImGui::Checkbox("X 반전", &bFlipX);
	ImGui::SameLine();
	ImGui::Checkbox("Y 반전", &bFlipY);
	ImGui::SameLine();
	DrawLivePreviewToggle();
	ImGui::SameLine();
	const int32 Current = GetCurrentFrame();
	ImGui::TextDisabled("프레임 %d / %zu  %.2f초 / %.2f초", Current, Asset.Frames.size(), Time, Asset.TotalDuration);
}

void FFlipbookEditor::DrawCanvas(FAssetEditorEnvironment& Env)
{
	const int32 TextureWidth  = Atlas != nullptr && Atlas->TextureWidth > 0 ? Atlas->TextureWidth : Texture.GetWidth();
	const int32 TextureHeight = Atlas != nullptr && Atlas->TextureHeight > 0 ? Atlas->TextureHeight : Texture.GetHeight();

	// 모든 프레임을 피벗에 맞춰 겹친 경계 (캔버스 내용 크기)
	FVector2 UnionMin(0.0f, 0.0f);
	FVector2 UnionMax(1.0f, 1.0f);
	bool     bAny = false;
	const auto FrameRect = [this](const FSpriteSlice& Slice, FVector2& OutMin, FVector2& OutMax) {
		const float PivotX = bFlipX ? 1.0f - Slice.Pivot.X : Slice.Pivot.X;
		const float PivotY = bFlipY ? 1.0f - Slice.Pivot.Y : Slice.Pivot.Y;
		OutMin             = FVector2(-PivotX * static_cast<float>(Slice.W), -(1.0f - PivotY) * static_cast<float>(Slice.H));
		OutMax             = OutMin + FVector2(static_cast<float>(Slice.W), static_cast<float>(Slice.H));
	};
	if (Atlas != nullptr)
	{
		for (const FFlipbookFrame& Frame : Asset.Frames)
		{
			const int32 SliceIndex = Atlas->FindSlice(Frame.Slice);
			if (SliceIndex < 0)
			{
				continue;
			}
			FVector2 Min;
			FVector2 Max;
			FrameRect(Atlas->Slices[static_cast<size_t>(SliceIndex)], Min, Max);
			UnionMin = bAny ? FVector2(FMath::Min(UnionMin.X, Min.X), FMath::Min(UnionMin.Y, Min.Y)) : Min;
			UnionMax = bAny ? FVector2(FMath::Max(UnionMax.X, Max.X), FMath::Max(UnionMax.Y, Max.Y)) : Max;
			bAny     = true;
		}
	}
	const FVector2 Origin(std::floor(-UnionMin.X), std::floor(-UnionMin.Y)); // 피벗 = 캔버스 이미지 좌표
	const int32    ContentWidth  = static_cast<int32>(std::ceil(UnionMax.X - UnionMin.X));
	const int32    ContentHeight = static_cast<int32>(std::ceil(UnionMax.Y - UnionMin.Y));

	Canvas.Begin("##Canvas", ImGui::GetContentRegionAvail(), FMath::Max(ContentWidth, 1), FMath::Max(ContentHeight, 1));
	ImDrawList* DrawList = Canvas.GetDrawList();
	const int32 Current  = GetCurrentFrame();
	bool        bDrawn   = false;
	if (bAny)
	{
		Canvas.DrawChecker(FVector2(0.0f, 0.0f), FVector2(static_cast<float>(ContentWidth), static_cast<float>(ContentHeight)));
	}
	if (Atlas != nullptr && Current >= 0 && Current < static_cast<int32>(Asset.Frames.size()))
	{
		const int32 SliceIndex = Atlas->FindSlice(Asset.Frames[static_cast<size_t>(Current)].Slice);
		if (SliceIndex >= 0 && TextureWidth > 0 && TextureHeight > 0)
		{
			const FSpriteSlice& Slice = Atlas->Slices[static_cast<size_t>(SliceIndex)];
			FVector2            Min;
			FVector2            Max;
			FrameRect(Slice, Min, Max);
			const FSpriteUvRect Uv = SpriteMath::ComputeUvRect(Slice, TextureWidth, TextureHeight);
			ImVec2              Uv0(Uv.U0, Uv.V0);
			ImVec2              Uv1(Uv.U1, Uv.V1);
			if (bFlipX)
			{
				std::swap(Uv0.x, Uv1.x);
			}
			if (bFlipY)
			{
				std::swap(Uv0.y, Uv1.y);
			}
			Canvas.DrawImageRect(GetImTexture(Env, Texture), Origin + Min, Origin + Max, Atlas->Filter == ESpriteFilter::Point, Uv0, Uv1);
			Canvas.DrawPixelGrid(ContentWidth, ContentHeight);
			bDrawn = true;
		}
	}
	// 피벗 (원점)
	const ImVec2 Pivot = Canvas.ImageToScreen(Origin);
	DrawList->AddLine(ImVec2(Pivot.x - 12.0f, Pivot.y), ImVec2(Pivot.x + 12.0f, Pivot.y), PivotColor);
	DrawList->AddLine(ImVec2(Pivot.x, Pivot.y - 12.0f), ImVec2(Pivot.x, Pivot.y + 12.0f), PivotColor);

	const ImVec2 TextPos(Canvas.GetMin().x + 8.0f, Canvas.GetMin().y + 6.0f);
	if (!bDrawn)
	{
		const char* Message = Atlas == nullptr ? "대상 스프라이트(.esprite)를 고르세요"
		                      : Asset.Frames.empty() ? "프레임이 없습니다 — 오른쪽 슬라이스 목록에서 추가"
		                                             : "현재 프레임의 슬라이스가 아틀라스에 없습니다";
		DrawList->AddText(TextPos, ImGui::ColorConvertFloat4ToU32(FEditorTheme::Warning), Message);
	}
	// 지나간 이벤트 강조
	float Y = TextPos.y + (bDrawn ? 0.0f : 18.0f);
	for (const FRecentEvent& Event : RecentEvents)
	{
		const float Alpha = FMath::Clamp(1.0f - Event.Age / EventFlashSeconds, 0.0f, 1.0f);
		DrawList->AddText(ImVec2(TextPos.x, Y), IM_COL32(255, 210, 60, static_cast<int>(Alpha * 255.0f)),
		                  std::format(ICON_FA_BOLT " {} (프레임 {})", Event.Name, Event.Frame).c_str());
		Y += 18.0f;
	}
	Canvas.End();
}

void FFlipbookEditor::DrawTimeline()
{
	const ImVec2 Min    = ImGui::GetCursorScreenPos();
	const float  Width  = FMath::Max(ImGui::GetContentRegionAvail().x, 50.0f);
	const float  Height = 46.0f;
	ImGui::InvisibleButton("##Timeline", ImVec2(Width, Height));
	const bool  bActive  = ImGui::IsItemActive();
	ImDrawList* DrawList = ImGui::GetWindowDrawList();
	const ImVec2 Max(Min.x + Width, Min.y + Height);
	DrawList->AddRectFilled(Min, Max, IM_COL32(24, 26, 31, 255));
	const float Total = Asset.TotalDuration;
	if (Asset.Frames.empty() || Total <= 0.0f)
	{
		DrawList->AddText(ImVec2(Min.x + 6.0f, Min.y + 14.0f), IM_COL32(150, 150, 150, 255), "타임라인 (프레임 없음)");
		return;
	}
	const float Scale   = Width / Total;
	const int32 Current = GetCurrentFrame();
	float       Start   = 0.0f;
	for (size_t Index = 0; Index < Asset.Frames.size(); ++Index)
	{
		const float  Duration = Asset.FrameDurations[Index];
		const ImVec2 BlockMin(Min.x + Start * Scale + 1.0f, Min.y + 14.0f);
		const ImVec2 BlockMax(Min.x + (Start + Duration) * Scale - 1.0f, Max.y - 2.0f);
		const ImU32  Color = IsFrameSelected(static_cast<int32>(Index)) ? FrameBlockSelected
		                     : static_cast<int32>(Index) == Current     ? FrameBlockCurrent
		                                                                : FrameBlockColor;
		DrawList->AddRectFilled(BlockMin, BlockMax, Color, 2.0f);
		DrawList->PushClipRect(BlockMin, BlockMax, true);
		DrawList->AddText(ImVec2(BlockMin.x + 3.0f, BlockMin.y + 2.0f), IM_COL32(220, 220, 230, 255),
		                  std::format("{} {}", Index, Asset.Frames[Index].Slice).c_str());
		DrawList->PopClipRect();
		Start += Duration;
	}
	// 이벤트 표시 (프레임 시작)
	for (const FFlipbookEvent& Event : Asset.Events)
	{
		if (Event.Frame < 0 || Event.Frame >= static_cast<int32>(Asset.Frames.size()))
		{
			continue;
		}
		const float X       = Min.x + GetFrameStart(Event.Frame) * Scale + 1.0f;
		const bool  bRecent = std::any_of(RecentEvents.begin(), RecentEvents.end(), [&Event](const FRecentEvent& Recent) { return Recent.Name == Event.Name; });
		DrawList->AddTriangleFilled(ImVec2(X, Min.y + 2.0f), ImVec2(X + 8.0f, Min.y + 2.0f), ImVec2(X + 4.0f, Min.y + 11.0f),
		                            bRecent ? IM_COL32(255, 255, 255, 255) : EventColor);
		if (ImGui::IsMouseHoveringRect(ImVec2(X - 2.0f, Min.y), ImVec2(X + 10.0f, Min.y + 13.0f)))
		{
			ImGui::SetTooltip("이벤트 %s (프레임 %d)", Event.Name.c_str(), Event.Frame);
		}
	}
	// 재생 위치 (PingPong 역방향 구간은 거울 위치)
	float Position = FlipbookMath::WrapTime(Time, Asset.FrameDurations, Asset.Loop);
	if (Asset.Loop == EFlipbookLoopMode::PingPong && Position >= Total && Asset.Frames.size() > 1)
	{
		Position = Total - Asset.FrameDurations.back() - (Position - Total);
	}
	Position        = FMath::Clamp(Position, 0.0f, Total);
	const float  PX = Min.x + Position * Scale;
	DrawList->AddLine(ImVec2(PX, Min.y), ImVec2(PX, Max.y), PlayheadColor, 2.0f);
	// 스크러빙
	if (bActive)
	{
		Time     = FMath::Clamp((ImGui::GetIO().MousePos.x - Min.x) / Scale, 0.0f, Total * 0.9999f);
		bPlaying = false;
	}
}

// ---------------------------------------------------------------- 속성

void FFlipbookEditor::DrawProperties(FAssetEditorEnvironment& Env)
{
	ImGui::SeparatorText(ICON_FA_FILM " 플립북");
	std::string SpriteRef = Asset.Sprite;
	bool        bChanged  = FAssetEditorWidgets::TextureCombo("스프라이트", SpriteRef, GetContentFiles(Env, FSpriteAsset::Extension), "(없음)");
	bChanged |= AcceptAssetDrop(SpriteRef, FSpriteAsset::Extension);
	if (bChanged && SpriteRef != Asset.Sprite)
	{
		Asset.Sprite = SpriteRef;
		SliceSelection.clear();
		SyncAtlas(Env, true);
		Edited("스프라이트 변경");
	}
	ImGui::SetItemTooltip("대상 .esprite (이 파일 폴더 기준). 콘텐츠 브라우저에서 끌어 놓아도 된다");
	if (!Asset.Sprite.empty() && Atlas == nullptr)
	{
		ImGui::TextColored(FEditorTheme::Danger, ICON_FA_TRIANGLE_EXCLAMATION " 스프라이트를 읽을 수 없습니다");
	}
	if (ImGui::DragFloat("Fps", &Asset.Fps, 0.1f, 0.1f, 240.0f, "%.1f"))
	{
		Asset.Fps = FMath::Clamp(Asset.Fps, 0.1f, 240.0f);
		Edited("Fps");
	}
	int32 Loop = static_cast<int32>(Asset.Loop);
	if (ImGui::Combo("반복", &Loop, "Loop\0Once\0PingPong\0"))
	{
		Asset.Loop = static_cast<EFlipbookLoopMode>(Loop);
		Edited("반복 방식");
	}
	DrawSliceList();
	DrawFrameTable();
	DrawEventTable();
}

void FFlipbookEditor::AddSelectedSlicesAsFrames(int32 InsertBefore)
{
	if (SliceSelection.empty())
	{
		return;
	}
	FrameSelection = Sprite2DEditing::InsertFrames(Asset, InsertBefore, SliceSelection);
	Edited("프레임 추가");
}

void FFlipbookEditor::DrawSliceList()
{
	ImGui::SeparatorText("아틀라스 슬라이스");
	if (Atlas == nullptr)
	{
		ImGui::TextDisabled("(스프라이트 없음)");
		return;
	}
	const int32 InsertAt = FrameSelection.empty() ? -1 : FrameSelection.back() + 1;
	ImGui::BeginDisabled(SliceSelection.empty());
	if (ImGui::SmallButton(ICON_FA_PLUS " 프레임으로 추가"))
	{
		AddSelectedSlicesAsFrames(InsertAt);
	}
	ImGui::EndDisabled();
	ImGui::SetItemTooltip("고른 슬라이스(목록 순서)를 선택 프레임 뒤에 (없으면 끝에). 프레임 표로 끌어 놓아도 된다");
	ImGui::SameLine();
	if (ImGui::SmallButton("모두 선택"))
	{
		SliceSelection = Atlas->GetSliceNames();
	}
	const float Height = FMath::Clamp(static_cast<float>(Atlas->Slices.size()) * ImGui::GetTextLineHeightWithSpacing() + 8.0f, 50.0f, 150.0f);
	if (ImGui::BeginChild("##Slices", ImVec2(0.0f, Height), ImGuiChildFlags_Borders))
	{
		for (const FSpriteSlice& Slice : Atlas->Slices)
		{
			const bool bSelected = std::find(SliceSelection.begin(), SliceSelection.end(), Slice.Name) != SliceSelection.end();
			if (ImGui::Selectable(Slice.Name.c_str(), bSelected, ImGuiSelectableFlags_AllowDoubleClick))
			{
				if (ImGui::GetIO().KeyCtrl)
				{
					if (bSelected)
					{
						std::erase(SliceSelection, Slice.Name);
					}
					else
					{
						SliceSelection.push_back(Slice.Name);
					}
				}
				else
				{
					SliceSelection = { Slice.Name };
				}
				if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
				{
					AddSelectedSlicesAsFrames(InsertAt);
				}
			}
			if (ImGui::BeginDragDropSource())
			{
				if (std::find(SliceSelection.begin(), SliceSelection.end(), Slice.Name) == SliceSelection.end())
				{
					SliceSelection = { Slice.Name };
				}
				ImGui::SetDragDropPayload(SlicePayload, nullptr, 0);
				ImGui::Text("슬라이스 %zu개", SliceSelection.size());
				ImGui::EndDragDropSource();
			}
		}
	}
	ImGui::EndChild();
}

void FFlipbookEditor::DrawFrameTable()
{
	ImGui::SeparatorText(std::format("프레임 ({})", Asset.Frames.size()).c_str());
	const bool bHasSelection = !FrameSelection.empty();
	ImGui::BeginDisabled(!bHasSelection);
	if (ImGui::SmallButton(ICON_FA_ARROW_UP))
	{
		FrameSelection = Sprite2DEditing::MoveFrames(Asset, FrameSelection, FMath::Max(FrameSelection.front() - 1, 0));
		Edited("프레임 순서");
	}
	ImGui::SameLine();
	if (ImGui::SmallButton(ICON_FA_ARROW_DOWN))
	{
		FrameSelection = Sprite2DEditing::MoveFrames(Asset, FrameSelection, FrameSelection.back() + 2);
		Edited("프레임 순서");
	}
	ImGui::SameLine();
	if (ImGui::SmallButton(ICON_FA_CLONE " 복제"))
	{
		FrameSelection = Sprite2DEditing::DuplicateFrames(Asset, FrameSelection);
		Edited("프레임 복제");
	}
	ImGui::SameLine();
	if (ImGui::SmallButton(ICON_FA_TRASH_CAN " 삭제") || (ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_Delete, false) && !ImGui::GetIO().WantTextInput))
	{
		const int32 Next = Sprite2DEditing::RemoveFrames(Asset, FrameSelection);
		FrameSelection.clear();
		if (Next >= 0)
		{
			FrameSelection.push_back(Next);
		}
		Edited("프레임 삭제");
	}
	ImGui::EndDisabled();

	// 끌어 놓기: 프레임(순서 바꾸기) 또는 슬라이스(그 자리에 추가)
	const auto AcceptDrop = [this](int32 InsertBefore) {
		if (!ImGui::BeginDragDropTarget())
		{
			return;
		}
		if (ImGui::AcceptDragDropPayload(FramePayload))
		{
			FrameSelection = Sprite2DEditing::MoveFrames(Asset, FrameSelection, InsertBefore);
			Edited("프레임 순서");
		}
		else if (ImGui::AcceptDragDropPayload(SlicePayload))
		{
			AddSelectedSlicesAsFrames(InsertBefore);
		}
		ImGui::EndDragDropTarget();
	};

	const ImGuiTableFlags Flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY;
	const float           Rows  = static_cast<float>(Asset.Frames.size() + 1);
	if (ImGui::BeginTable("##Frames", 4, Flags, ImVec2(0.0f, FMath::Clamp(Rows * (ImGui::GetFrameHeightWithSpacing() + 2.0f) + 8.0f, 60.0f, 260.0f))))
	{
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 28.0f);
		ImGui::TableSetupColumn("슬라이스", ImGuiTableColumnFlags_WidthStretch, 2.0f);
		ImGui::TableSetupColumn("Duration", ImGuiTableColumnFlags_WidthStretch, 1.0f);
		ImGui::TableSetupColumn("Scale", ImGuiTableColumnFlags_WidthStretch, 1.0f);
		ImGui::TableHeadersRow();
		const int32 Current = GetCurrentFrame();
		for (size_t Index = 0; Index < Asset.Frames.size(); ++Index)
		{
			const int32     FrameIndex = static_cast<int32>(Index);
			FFlipbookFrame& Frame      = Asset.Frames[Index];
			ImGui::PushID(FrameIndex);
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			const std::string Label = std::format("{}{}", FrameIndex, FrameIndex == Current ? " " ICON_FA_PLAY : "");
			if (ImGui::Selectable(Label.c_str(), IsFrameSelected(FrameIndex), ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
			{
				if (ImGui::GetIO().KeyCtrl)
				{
					if (IsFrameSelected(FrameIndex))
					{
						std::erase(FrameSelection, FrameIndex);
					}
					else
					{
						FrameSelection.push_back(FrameIndex);
					}
				}
				else if (ImGui::GetIO().KeyShift && !FrameSelection.empty())
				{
					const int32 From = FrameSelection.back();
					FrameSelection.clear();
					for (int32 Each = FMath::Min(From, FrameIndex); Each <= FMath::Max(From, FrameIndex); ++Each)
					{
						FrameSelection.push_back(Each);
					}
				}
				else
				{
					FrameSelection = { FrameIndex };
					SeekFrame(FrameIndex);
				}
			}
			if (ImGui::BeginDragDropSource())
			{
				if (!IsFrameSelected(FrameIndex))
				{
					FrameSelection = { FrameIndex };
				}
				ImGui::SetDragDropPayload(FramePayload, nullptr, 0);
				ImGui::Text("프레임 %zu개", FrameSelection.size());
				ImGui::EndDragDropSource();
			}
			AcceptDrop(FrameIndex);

			ImGui::TableNextColumn();
			ImGui::SetNextItemWidth(-1.0f);
			const bool bMissing = Atlas != nullptr && Atlas->FindSlice(Frame.Slice) < 0;
			if (bMissing)
			{
				ImGui::PushStyleColor(ImGuiCol_Text, FEditorTheme::Danger);
			}
			if (ImGui::BeginCombo("##Slice", Frame.Slice.empty() ? "(없음)" : Frame.Slice.c_str()))
			{
				if (Atlas != nullptr)
				{
					for (const FSpriteSlice& Slice : Atlas->Slices)
					{
						if (ImGui::Selectable(Slice.Name.c_str(), Slice.Name == Frame.Slice))
						{
							Frame.Slice = Slice.Name;
							Edited("프레임 슬라이스");
						}
					}
				}
				ImGui::EndCombo();
			}
			if (bMissing)
			{
				ImGui::PopStyleColor();
				ImGui::SetItemTooltip("아틀라스에 없는 슬라이스");
			}
			ImGui::TableNextColumn();
			ImGui::SetNextItemWidth(-1.0f);
			if (ImGui::DragFloat("##Duration", &Frame.Duration, 0.005f, 0.0f, 60.0f, Frame.Duration > 0.0f ? "%.3f초" : "Fps 기준"))
			{
				Frame.Duration = FMath::Max(Frame.Duration, 0.0f);
				Edited("프레임 길이");
			}
			ImGui::SetItemTooltip("초 (0 = Scale / Fps)");
			ImGui::TableNextColumn();
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::BeginDisabled(Frame.Duration > 0.0f);
			if (ImGui::DragFloat("##Scale", &Frame.Scale, 0.01f, 0.01f, 100.0f, "x%.2f"))
			{
				Frame.Scale = FMath::Max(Frame.Scale, 0.01f);
				Edited("프레임 배율");
			}
			ImGui::EndDisabled();
			ImGui::PopID();
		}
		// 끝 자리 (맨 뒤로 놓기)
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Selectable("##End", false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_Disabled);
		AcceptDrop(static_cast<int32>(Asset.Frames.size()));
		ImGui::TableNextColumn();
		ImGui::TextDisabled("(여기에 놓으면 맨 뒤)");
		ImGui::EndTable();
	}
	FAssetEditorWidgets::Hint("행을 끌어 순서 바꾸기 (Ctrl/Shift로 여러 개), 슬라이스 목록에서 끌어 놓으면 그 자리에 추가");
}

void FFlipbookEditor::DrawEventTable()
{
	ImGui::SeparatorText(std::format(ICON_FA_BOLT " 이벤트 ({})", Asset.Events.size()).c_str());
	if (ImGui::SmallButton(ICON_FA_PLUS " 현재 프레임에 이벤트"))
	{
		FFlipbookEvent Event;
		Event.Frame = FMath::Max(GetCurrentFrame(), 0);
		Event.Name  = "Event";
		Asset.Events.push_back(std::move(Event));
		Edited("이벤트 추가");
	}
	int32 RemoveIndex = -1;
	if (!Asset.Events.empty() && ImGui::BeginTable("##Events", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
	{
		ImGui::TableSetupColumn("프레임", ImGuiTableColumnFlags_WidthStretch, 1.0f);
		ImGui::TableSetupColumn("이름", ImGuiTableColumnFlags_WidthStretch, 2.0f);
		ImGui::TableSetupColumn("##Remove", ImGuiTableColumnFlags_WidthFixed, 24.0f);
		ImGui::TableHeadersRow();
		for (size_t Index = 0; Index < Asset.Events.size(); ++Index)
		{
			FFlipbookEvent& Event = Asset.Events[Index];
			ImGui::PushID(static_cast<int>(Index));
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::SetNextItemWidth(-1.0f);
			if (ImGui::InputInt("##Frame", &Event.Frame))
			{
				Event.Frame = FMath::Clamp(Event.Frame, 0, FMath::Max(static_cast<int32>(Asset.Frames.size()) - 1, 0));
				Edited("이벤트 프레임");
			}
			ImGui::TableNextColumn();
			ImGui::SetNextItemWidth(-1.0f);
			if (std::string NewName; DataValueWidgets::InputTextCommit("##Name", Event.Name, NewName) && NewName != Event.Name)
			{
				Event.Name = NewName;
				Edited("이벤트 이름");
			}
			ImGui::TableNextColumn();
			if (ImGui::SmallButton(ICON_FA_TRASH_CAN))
			{
				RemoveIndex = static_cast<int32>(Index);
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	if (RemoveIndex >= 0)
	{
		Asset.Events.erase(Asset.Events.begin() + RemoveIndex);
		Edited("이벤트 삭제");
	}
	FAssetEditorWidgets::Hint("이벤트는 그 프레임에 들어갈 때 한 번 — Lua OnFlipbookEvent_<이름>(frame)");
}

// ---------------------------------------------------------------- 자동 검증

bool FFlipbookEditor::ApplyVerifyEdits(FAssetEditorEnvironment& Env)
{
	(void)Env;
	const auto Step = [this](std::string_view Label) {
		Edited(Label);
		CommitPendingEdit(false);
	};
	if (Atlas == nullptr || Atlas->Slices.empty())
	{
		return false;
	}
	// 1) 슬라이스 두 개를 끝에 추가
	const std::vector<std::string> Names = { Atlas->Slices.front().Name, Atlas->Slices.back().Name };
	FrameSelection                       = Sprite2DEditing::InsertFrames(Asset, -1, Names);
	Step("프레임 추가");
	// 2) 맨 앞으로 옮기기 (이벤트가 같은 프레임을 따라가는지 확인)
	const std::vector<FFlipbookEvent> EventsBefore = Asset.Events;
	const std::vector<FFlipbookFrame> FramesBefore = Asset.Frames;
	FrameSelection                                 = Sprite2DEditing::MoveFrames(Asset, FrameSelection, 0);
	Step("프레임 순서");
	bool bEventsFollow = true;
	for (size_t Index = 0; Index < EventsBefore.size() && Index < Asset.Events.size(); ++Index)
	{
		const int32 OldFrame = EventsBefore[Index].Frame;
		const int32 NewFrame = Asset.Events[Index].Frame;
		if (OldFrame >= 0 && OldFrame < static_cast<int32>(FramesBefore.size()) &&
		    !(FramesBefore[static_cast<size_t>(OldFrame)] == Asset.Frames[static_cast<size_t>(NewFrame)]))
		{
			bEventsFollow = false;
		}
	}
	// 3) 복제, 길이·배율, Fps, 반복, 이벤트 추가, 삭제
	FrameSelection = Sprite2DEditing::DuplicateFrames(Asset, FrameSelection);
	Step("프레임 복제");
	Asset.Frames.front().Duration = 0.125f;
	Step("프레임 길이");
	Asset.Fps = Asset.Fps * 0.5f + 1.0f;
	Step("Fps");
	Asset.Loop = Asset.Loop == EFlipbookLoopMode::Loop ? EFlipbookLoopMode::Once : EFlipbookLoopMode::Loop;
	Step("반복 방식");
	Asset.Events.push_back(FFlipbookEvent{ 0, "VerifyEvent" });
	Step("이벤트 추가");
	Sprite2DEditing::RemoveFrames(Asset, FrameSelection);
	FrameSelection.clear();
	Step("프레임 삭제");
	return bEventsFollow;
}
