#include "Editor/AssetEditors/ParticleEditor.h"

#include "Editor/AssetEditors/AssetEditorWidgets.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorTheme.h"
#include "Renderer/ResourceManager.h"
#include "Scene/Particles.h"

#include <imgui.h>

#include <algorithm>
#include <cstring>
#include <initializer_list>

namespace
{
	constexpr uint32 GMaxCpuParticles = 20000;  // 프레임 업로드 버퍼(4MB) 여유를 남기는 상한
	constexpr uint32 GMaxGpuParticles = 500000; // GPU 풀 48MB

	constexpr FVector4 V(float X, float Y = 0.0f, float Z = 0.0f, float W = 0.0f) { return FVector4(X, Y, Z, W); }

	// ---- 템플릿 도우미 -------------------------------------------------------------------------------------
	FParticleModule& AddModule(FParticleEmitter& Emitter, EParticleModuleType Type)
	{
		const FParticleModuleInfo& Info  = GetParticleModuleInfos()[static_cast<size_t>(Type)];
		auto&                      Stage = Emitter.GetStage(Info.Stage);
		Stage.push_back(FParticleModule::Make(Type));
		return Stage.back();
	}

	void SetInput(FParticleModule& Module, const char* Id, FParticleValue Value)
	{
		const auto Inputs = Module.GetInfo().Inputs;
		for (size_t Index = 0; Index < Inputs.size(); ++Index)
		{
			if (std::strcmp(Inputs[Index].Id, Id) == 0)
			{
				Module.Inputs[Index] = std::move(Value);
				return;
			}
		}
	}

	FParticleValue C(const FVector4& Value) { return FParticleValue::Constant(Value); }
	FParticleValue R(const FVector4& Min, const FVector4& Max) { return FParticleValue::Range(Min, Max); }
	FParticleValue K(std::vector<FParticleCurveKey> Keys) { return FParticleValue::MakeCurve(std::move(Keys)); }

	FParticleEmitter MakeBaseEmitter(const char* Name)
	{
		FParticleEmitter Emitter;
		Emitter.Name = Name;
		Emitter.Renderers.push_back(FParticleRendererSettings{});
		return Emitter;
	}

	// ---- 단계 색 (나이아가라처럼 단계마다 색 띠) ---------------------------------------------------------------
	ImVec4 StageColor(EParticleStage Stage)
	{
		switch (Stage)
		{
		case EParticleStage::EmitterUpdate: return ImVec4(0.55f, 0.32f, 0.12f, 1.0f);
		case EParticleStage::ParticleSpawn: return ImVec4(0.16f, 0.45f, 0.22f, 1.0f);
		default:                            return ImVec4(0.45f, 0.38f, 0.10f, 1.0f);
		}
	}

	const char* StageLabel(EParticleStage Stage)
	{
		switch (Stage)
		{
		case EParticleStage::EmitterUpdate: return ICON_FA_CLOCK "  이미터 갱신 (언제 몇 개)";
		case EParticleStage::ParticleSpawn: return ICON_FA_SEEDLING "  입자 생성 (태어날 때 한 번)";
		default:                            return ICON_FA_WIND "  입자 갱신 (매 프레임)";
		}
	}

	int32 ChannelCount(EParticleInputKind Kind)
	{
		switch (Kind)
		{
		case EParticleInputKind::Vector: return 3;
		case EParticleInputKind::Color:  return 4;
		default:                         return 1;
		}
	}

	// ---- 값 위젯 ------------------------------------------------------------------------------------------
	bool DrawValueWidget(const FParticleInputInfo& Info, FVector4& Value, const char* Id)
	{
		ImGui::SetNextItemWidth(-FLT_MIN);
		switch (Info.Kind)
		{
		case EParticleInputKind::Float:  return ImGui::DragFloat(Id, &Value.X, Info.Speed, 0.0f, 0.0f, "%g");
		case EParticleInputKind::Vector: return ImGui::DragFloat3(Id, &Value.X, Info.Speed, 0.0f, 0.0f, "%g");
		case EParticleInputKind::Color:
			return ImGui::ColorEdit4(Id, &Value.X, ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_AlphaBar);
		case EParticleInputKind::Bool:
		{
			bool bValue = Value.X != 0.0f;
			if (ImGui::Checkbox(Id, &bValue))
			{
				Value.X = bValue ? 1.0f : 0.0f;
				return true;
			}
			return false;
		}
		case EParticleInputKind::Enum:
		{
			int32 Selected = static_cast<int32>(Value.X);
			if (ImGui::Combo(Id, &Selected, Info.EnumNames != nullptr ? Info.EnumNames : "\0"))
			{
				Value.X = static_cast<float>(Selected);
				return true;
			}
			return false;
		}
		}
		return false;
	}

	ImU32 ChannelColor(int32 Channel, float Alpha)
	{
		static constexpr ImVec4 Colors[] = { ImVec4(0.95f, 0.35f, 0.35f, 1), ImVec4(0.40f, 0.85f, 0.40f, 1), ImVec4(0.40f, 0.60f, 1.00f, 1),
			                                 ImVec4(0.85f, 0.85f, 0.85f, 1) };
		ImVec4 Color = Colors[std::clamp(Channel, 0, 3)];
		Color.w      = Alpha;
		return ImGui::ColorConvertFloat4ToU32(Color);
	}

	// 선택 키가 시간순 정렬 뒤에도 같은 키를 가리키도록 옆으로 옮긴다
	int32 ResortKey(std::vector<FParticleCurveKey>& Keys, int32 Index)
	{
		while (Index > 0 && Keys[Index].Time < Keys[Index - 1].Time)
		{
			std::swap(Keys[Index], Keys[Index - 1]);
			--Index;
		}
		while (Index + 1 < static_cast<int32>(Keys.size()) && Keys[Index].Time > Keys[Index + 1].Time)
		{
			std::swap(Keys[Index], Keys[Index + 1]);
			++Index;
		}
		return Index;
	}

	// 곡선 편집기: 채널별 선 + 키 점. 끌기 = 이동, 더블클릭 = 키 추가, 우클릭 = 키 삭제. 아래에 선택 키 값 편집
	bool DrawCurveEditor(const FParticleInputInfo& Info, FParticleValue& Value)
	{
		std::vector<FParticleCurveKey>& Keys = Value.Curve;
		if (Keys.empty())
		{
			Keys.push_back({ 0.0f, Value.A });
		}
		bool          bChanged  = false;
		ImGuiStorage* Storage   = ImGui::GetStateStorage();
		const ImGuiID SelectId  = ImGui::GetID("##CurveSelected");
		const ImGuiID ChannelId = ImGui::GetID("##CurveChannel");
		const ImGuiID DragId    = ImGui::GetID("##CurveDragging");
		const int32   Channels  = ChannelCount(Info.Kind);
		int32         Selected  = std::clamp(Storage->GetInt(SelectId, 0), 0, static_cast<int32>(Keys.size()) - 1);
		int32         Active    = std::clamp(Storage->GetInt(ChannelId, 0), 0, Channels - 1);

		if (Channels > 1)
		{
			static constexpr const char* Names[] = { "X", "Y", "Z", "A" };
			for (int32 Channel = 0; Channel < Channels; ++Channel)
			{
				const char* Name = Info.Kind == EParticleInputKind::Color ? (Channel == 0 ? "R" : Channel == 1 ? "G" : Channel == 2 ? "B" : "A") : Names[Channel];
				ImGui::PushStyleColor(ImGuiCol_Button, Channel == Active ? ImGui::ColorConvertU32ToFloat4(ChannelColor(Channel, 0.55f)) : ImVec4(0.2f, 0.2f, 0.2f, 1));
				ImGui::PushID(Channel);
				if (ImGui::SmallButton(Name))
				{
					Active = Channel;
				}
				ImGui::PopID();
				ImGui::PopStyleColor();
				ImGui::SameLine();
			}
			ImGui::TextDisabled("끌 채널");
		}

		// 값 범위 (모든 채널, 여유 10%)
		float MinValue = Keys[0].Value.X, MaxValue = Keys[0].Value.X;
		for (const FParticleCurveKey& Key : Keys)
		{
			for (int32 Channel = 0; Channel < Channels; ++Channel)
			{
				MinValue = std::min(MinValue, Key.Value[Channel]);
				MaxValue = std::max(MaxValue, Key.Value[Channel]);
			}
		}
		MinValue = std::min(MinValue, 0.0f);
		if (MaxValue - MinValue < 1e-3f)
		{
			MaxValue = MinValue + 1.0f;
		}
		const float Pad = (MaxValue - MinValue) * 0.1f;
		MinValue -= Pad;
		MaxValue += Pad;

		const float  Width  = std::max(ImGui::GetContentRegionAvail().x, 60.0f);
		const float  Height = ImGui::GetFontSize() * 5.5f;
		const ImVec2 Origin = ImGui::GetCursorScreenPos();
		ImGui::InvisibleButton("##Curve", ImVec2(Width, Height), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
		const bool bHovered = ImGui::IsItemHovered();
		const auto ToScreen = [&](float Time, float Val) {
			return ImVec2(Origin.x + Time * Width, Origin.y + (1.0f - (Val - MinValue) / (MaxValue - MinValue)) * Height);
		};

		ImDrawList* Draw = ImGui::GetWindowDrawList();
		Draw->AddRectFilled(Origin, ImVec2(Origin.x + Width, Origin.y + Height), IM_COL32(24, 24, 24, 255), 3.0f);
		for (float Line : { 0.25f, 0.5f, 0.75f })
		{
			Draw->AddLine(ImVec2(Origin.x + Line * Width, Origin.y), ImVec2(Origin.x + Line * Width, Origin.y + Height), IM_COL32(50, 50, 50, 255));
		}
		if (MinValue < 0.0f && MaxValue > 0.0f)
		{
			const float ZeroY = ToScreen(0.0f, 0.0f).y;
			Draw->AddLine(ImVec2(Origin.x, ZeroY), ImVec2(Origin.x + Width, ZeroY), IM_COL32(80, 80, 80, 255));
		}
		if (Info.Kind == EParticleInputKind::Color) // 아래쪽 색 띠
		{
			const float BarTop = Origin.y + Height - 6.0f;
			const auto  ToU32  = [](const FVector4& C) {
                return ImGui::ColorConvertFloat4ToU32(ImVec4(std::clamp(C.X, 0.0f, 1.0f), std::clamp(C.Y, 0.0f, 1.0f), std::clamp(C.Z, 0.0f, 1.0f), 1.0f));
			};
			float PrevX = Origin.x;
			ImU32 PrevC = ToU32(Keys.front().Value);
			for (const FParticleCurveKey& Key : Keys)
			{
				const float X = Origin.x + Key.Time * Width;
				const ImU32 C = ToU32(Key.Value);
				Draw->AddRectFilledMultiColor(ImVec2(PrevX, BarTop), ImVec2(X, Origin.y + Height), PrevC, C, C, PrevC);
				PrevX = X;
				PrevC = C;
			}
			Draw->AddRectFilled(ImVec2(PrevX, BarTop), ImVec2(Origin.x + Width, Origin.y + Height), PrevC);
		}
		for (int32 Channel = 0; Channel < Channels; ++Channel)
		{
			const ImU32 Color = ChannelColor(Channel, Channel == Active ? 1.0f : 0.35f);
			ImVec2      Prev  = ToScreen(0.0f, Keys.front().Value[Channel]);
			for (const FParticleCurveKey& Key : Keys)
			{
				const ImVec2 Point = ToScreen(Key.Time, Key.Value[Channel]);
				Draw->AddLine(Prev, Point, Color, Channel == Active ? 2.0f : 1.0f);
				Prev = Point;
			}
			Draw->AddLine(Prev, ToScreen(1.0f, Keys.back().Value[Channel]), Color, Channel == Active ? 2.0f : 1.0f);
		}
		for (int32 Index = 0; Index < static_cast<int32>(Keys.size()); ++Index)
		{
			const ImVec2 Point = ToScreen(Keys[Index].Time, Keys[Index].Value[Active]);
			Draw->AddCircleFilled(Point, Index == Selected ? 5.5f : 4.0f, Index == Selected ? IM_COL32(255, 200, 60, 255) : IM_COL32(230, 230, 230, 255));
		}

		// 조작
		const ImVec2 Mouse   = ImGui::GetIO().MousePos;
		const auto   FindKey = [&]() {
            int32 Found = -1;
            float Best  = 8.0f * 8.0f;
            for (int32 Index = 0; Index < static_cast<int32>(Keys.size()); ++Index)
            {
                const ImVec2 Point = ToScreen(Keys[Index].Time, Keys[Index].Value[Active]);
                const float  Dist  = (Point.x - Mouse.x) * (Point.x - Mouse.x) + (Point.y - Mouse.y) * (Point.y - Mouse.y);
                if (Dist < Best)
                {
                    Best  = Dist;
                    Found = Index;
                }
            }
            return Found;
		};
		if (bHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && FindKey() < 0)
		{
			const float Time = std::clamp((Mouse.x - Origin.x) / Width, 0.0f, 1.0f);
			Keys.push_back({ Time, Value.SampleCurve(Time) });
			Selected = ResortKey(Keys, static_cast<int32>(Keys.size()) - 1);
			bChanged = true;
		}
		else if (bHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
		{
			const int32 Found = FindKey();
			if (Found >= 0)
			{
				Selected = Found;
			}
			Storage->SetInt(DragId, Found >= 0 ? 1 : 0);
		}
		if (bHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
		{
			const int32 Found = FindKey();
			if (Found >= 0 && Keys.size() > 1)
			{
				Keys.erase(Keys.begin() + Found);
				Selected = std::min(Selected, static_cast<int32>(Keys.size()) - 1);
				bChanged = true;
			}
		}
		if (ImGui::IsItemActive() && Storage->GetInt(DragId, 0) != 0 && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f))
		{
			const ImVec2 Delta = ImGui::GetIO().MouseDelta;
			if (Delta.x != 0.0f || Delta.y != 0.0f)
			{
				FParticleCurveKey& Key = Keys[Selected];
				Key.Time               = std::clamp(Key.Time + Delta.x / Width, 0.0f, 1.0f);
				Key.Value[Active] -= Delta.y / Height * (MaxValue - MinValue);
				Selected = ResortKey(Keys, Selected);
				bChanged = true;
			}
		}
		if (!ImGui::IsItemActive())
		{
			Storage->SetInt(DragId, 0);
		}
		if (bHovered)
		{
			ImGui::SetItemTooltip("끌기: 키 이동 · 더블클릭: 키 추가 · 우클릭: 키 삭제\n가로 = 수명(또는 주기) 비율 0~1");
		}

		// 선택 키 값
		FParticleCurveKey& Key = Keys[Selected];
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.0f);
		if (ImGui::DragFloat("##KeyTime", &Key.Time, 0.005f, 0.0f, 1.0f, "시간 %.2f"))
		{
			Key.Time = std::clamp(Key.Time, 0.0f, 1.0f);
			Selected = ResortKey(Keys, Selected);
			bChanged = true;
		}
		if (Info.Kind != EParticleInputKind::Color && Info.Kind != EParticleInputKind::Vector)
		{
			ImGui::SameLine();
		}
		bChanged |= DrawValueWidget(Info, Keys[Selected].Value, "##KeyValue");

		Storage->SetInt(SelectId, Selected);
		Storage->SetInt(ChannelId, Active);
		if (bChanged)
		{
			Value.A = Value.B = Keys.front().Value;
		}
		return bChanged;
	}

	// 동적 입력: 방식 버튼(상수/무작위/곡선) + 방식별 위젯
	bool DrawDynamicInput(const FParticleInputInfo& Info, FParticleValue& Value)
	{
		bool bChanged = false;
		ImGui::PushID(Info.Id);
		const bool bModes = Info.bAllowRandom || Info.bAllowCurve;
		if (bModes)
		{
			const char* Icon = Value.Mode == EParticleValueMode::Random ? ICON_FA_DICE : (Value.Mode == EParticleValueMode::Curve ? ICON_FA_CHART_LINE : ICON_FA_HASHTAG);
			if (ImGui::SmallButton(Icon))
			{
				ImGui::OpenPopup("##Mode");
			}
			ImGui::SetItemTooltip("값 방식 바꾸기 (상수 / 무작위 범위 / 곡선)");
			if (ImGui::BeginPopup("##Mode"))
			{
				const auto Option = [&](EParticleValueMode Mode, const char* Label) {
					if (ImGui::Selectable(Label, Value.Mode == Mode) && Value.Mode != Mode)
					{
						const FVector4 Current = Value.Mode == EParticleValueMode::Curve ? Value.SampleCurve(0.0f) : Value.A;
						if (Mode == EParticleValueMode::Constant)
						{
							Value = FParticleValue::Constant(Current);
						}
						else if (Mode == EParticleValueMode::Random)
						{
							Value = FParticleValue::Range(Current, Current);
						}
						else
						{
							Value = FParticleValue::MakeCurve({ { 0.0f, Current }, { 1.0f, Current } });
						}
						bChanged = true;
					}
				};
				Option(EParticleValueMode::Constant, ICON_FA_HASHTAG "  상수");
				if (Info.bAllowRandom)
				{
					Option(EParticleValueMode::Random, ICON_FA_DICE "  무작위 범위 (입자마다)");
				}
				if (Info.bAllowCurve)
				{
					Option(EParticleValueMode::Curve, ICON_FA_CHART_LINE "  곡선 (수명/주기에 따라)");
				}
				ImGui::EndPopup();
			}
			if (Value.Mode != EParticleValueMode::Curve)
			{
				ImGui::SameLine(); // 곡선은 다음 줄에 전체 폭
			}
		}

		switch (Value.Mode)
		{
		case EParticleValueMode::Random:
			ImGui::BeginGroup();
			bChanged |= DrawValueWidget(Info, Value.A, "##Min");
			bChanged |= DrawValueWidget(Info, Value.B, "##Max");
			ImGui::EndGroup();
			ImGui::SetItemTooltip("위: 최소 · 아래: 최대 (입자마다 사이의 값을 무작위로)");
			break;
		case EParticleValueMode::Curve:
			ImGui::BeginGroup();
			bChanged |= DrawCurveEditor(Info, Value);
			ImGui::EndGroup();
			break;
		default:
			if (DrawValueWidget(Info, Value.A, "##Value"))
			{
				Value.B  = Value.A;
				bChanged = true;
			}
			break;
		}
		ImGui::PopID();
		return bChanged;
	}

	// 머리 오른쪽에 붙일 버튼 줄의 폭 (체크박스 + 아이콘 버튼들)
	float RightButtonsWidth(std::initializer_list<const char*> Icons, bool bCheckbox)
	{
		const ImGuiStyle& Style = ImGui::GetStyle();
		float             Width = bCheckbox ? ImGui::GetFrameHeight() + Style.ItemSpacing.x : 0.0f;
		for (const char* Icon : Icons)
		{
			Width += ImGui::CalcTextSize(Icon).x + Style.FramePadding.x * 2.0f + Style.ItemSpacing.x;
		}
		return Width;
	}

	// 접는 머리 오른쪽 끝에 버튼 줄을 붙인다 (창이 좁으면 제목 바로 뒤)
	void SameLineAtRight(float RowStartX, const char* Label, float ButtonsWidth)
	{
		const float LabelEnd = RowStartX + ImGui::GetTreeNodeToLabelSpacing() + ImGui::CalcTextSize(Label).x + ImGui::GetStyle().ItemSpacing.x * 2.0f;
		ImGui::SameLine(std::max(LabelEnd, ImGui::GetWindowContentRegionMax().x - ButtonsWidth));
	}

	// 오른쪽 정렬 아이콘 버튼 줄 (접는 머리 위)
	bool HeaderIconButton(const char* Icon, const char* Tooltip)
	{
		const bool bClicked = ImGui::SmallButton(Icon);
		ImGui::SetItemTooltip("%s", Tooltip);
		return bClicked;
	}

	const char* RendererLabel(EParticleRendererType Type)
	{
		switch (Type)
		{
		case EParticleRendererType::Mesh:   return ICON_FA_CUBE "  메시 렌더러";
		case EParticleRendererType::Ribbon: return ICON_FA_WAVE_SQUARE "  리본 렌더러";
		default:                            return ICON_FA_IMAGE "  스프라이트 렌더러";
		}
	}
} // namespace

// ---- 템플릿 -------------------------------------------------------------------------------------------------

std::vector<const char*> FParticleEditor::GetTemplateNames()
{
	return { "기본", "불꽃", "연기", "불티 (튀는 불똥)", "마법 소용돌이 (GPU)", "비 (GPU)", "리본 꼬리" };
}

FParticleEmitter FParticleEditor::MakeTemplate(size_t Index)
{
	switch (Index)
	{
	case 1: // 불꽃
	{
		FParticleEmitter E = MakeBaseEmitter("Fire");
		SetInput(AddModule(E, EParticleModuleType::SpawnRate), "SpawnRate", C(V(45)));
		FParticleModule& Init = AddModule(E, EParticleModuleType::InitializeParticle);
		SetInput(Init, "Lifetime", R(V(0.6f), V(1.2f)));
		SetInput(Init, "Color", C(V(4.0f, 1.5f, 0.35f, 1.0f)));
		SetInput(Init, "SpriteSize", R(V(30, 30), V(55, 55)));
		SetInput(Init, "SpriteRotation", R(V(0), V(360)));
		FParticleModule& Shape = AddModule(E, EParticleModuleType::ShapeLocation);
		SetInput(Shape, "Shape", C(V(1)));
		SetInput(Shape, "Radius", C(V(15)));
		FParticleModule& Cone = AddModule(E, EParticleModuleType::AddVelocityInCone);
		SetInput(Cone, "ConeAngle", C(V(15)));
		SetInput(Cone, "Speed", R(V(80), V(150)));
		SetInput(AddModule(E, EParticleModuleType::Drag), "Drag", C(V(1.0f)));
		FParticleModule& Curl = AddModule(E, EParticleModuleType::CurlNoiseForce);
		SetInput(Curl, "Strength", C(V(150)));
		SetInput(Curl, "Frequency", C(V(0.02f)));
		SetInput(AddModule(E, EParticleModuleType::ScaleColor), "Scale",
		         K({ { 0.0f, V(1, 1, 1, 0) }, { 0.15f, V(1, 1, 1, 1) }, { 0.6f, V(0.8f, 0.5f, 0.4f, 0.7f) }, { 1.0f, V(0.4f, 0.2f, 0.2f, 0) } }));
		SetInput(AddModule(E, EParticleModuleType::ScaleSpriteSize), "Scale", K({ { 0.0f, V(0.8f, 0.8f) }, { 1.0f, V(0.3f, 0.3f) } }));
		SetInput(AddModule(E, EParticleModuleType::SpriteRotationRate), "RotationRate", R(V(-90), V(90)));
		return E;
	}
	case 2: // 연기
	{
		FParticleEmitter E = MakeBaseEmitter("Smoke");
		E.Renderers[0].BlendMode = EParticleBlendMode::Alpha;
		SetInput(AddModule(E, EParticleModuleType::SpawnRate), "SpawnRate", C(V(12)));
		FParticleModule& Init = AddModule(E, EParticleModuleType::InitializeParticle);
		SetInput(Init, "Lifetime", R(V(2.5f), V(4.0f)));
		SetInput(Init, "Color", C(V(0.35f, 0.35f, 0.37f, 0.6f)));
		SetInput(Init, "SpriteSize", R(V(60, 60), V(90, 90)));
		SetInput(Init, "SpriteRotation", R(V(0), V(360)));
		FParticleModule& Shape = AddModule(E, EParticleModuleType::ShapeLocation);
		SetInput(Shape, "Shape", C(V(1)));
		SetInput(Shape, "Radius", C(V(20)));
		FParticleModule& Cone = AddModule(E, EParticleModuleType::AddVelocityInCone);
		SetInput(Cone, "ConeAngle", C(V(20)));
		SetInput(Cone, "Speed", R(V(50), V(90)));
		SetInput(AddModule(E, EParticleModuleType::Drag), "Drag", C(V(0.6f)));
		SetInput(AddModule(E, EParticleModuleType::CurlNoiseForce), "Strength", C(V(60)));
		SetInput(AddModule(E, EParticleModuleType::ScaleColor), "Scale", K({ { 0.0f, V(1, 1, 1, 0) }, { 0.2f, V(1, 1, 1, 1) }, { 1.0f, V(1, 1, 1, 0) } }));
		SetInput(AddModule(E, EParticleModuleType::ScaleSpriteSize), "Scale", K({ { 0.0f, V(0.6f, 0.6f) }, { 1.0f, V(2.2f, 2.2f) } }));
		SetInput(AddModule(E, EParticleModuleType::SpriteRotationRate), "RotationRate", R(V(-30), V(30)));
		return E;
	}
	case 3: // 불티
	{
		FParticleEmitter E = MakeBaseEmitter("Sparks");
		E.Duration                     = 1.5f;
		E.Renderers[0].Alignment       = EParticleSpriteAlignment::Velocity;
		E.Renderers[0].VelocityStretch = 0.03f;
		SetInput(AddModule(E, EParticleModuleType::SpawnBurst), "SpawnCount", R(V(50), V(80)));
		FParticleModule& Init = AddModule(E, EParticleModuleType::InitializeParticle);
		SetInput(Init, "Lifetime", R(V(0.6f), V(1.4f)));
		SetInput(Init, "Color", C(V(6.0f, 3.0f, 1.0f, 1.0f)));
		SetInput(Init, "SpriteSize", C(V(3, 3)));
		SetInput(Init, "SpriteRotation", C(V(0)));
		FParticleModule& Cone = AddModule(E, EParticleModuleType::AddVelocityInCone);
		SetInput(Cone, "ConeAngle", C(V(60)));
		SetInput(Cone, "Speed", R(V(400), V(900)));
		AddModule(E, EParticleModuleType::GravityForce);
		SetInput(AddModule(E, EParticleModuleType::Drag), "Drag", C(V(0.8f)));
		SetInput(AddModule(E, EParticleModuleType::Collision), "Restitution", C(V(0.45f)));
		SetInput(AddModule(E, EParticleModuleType::ScaleColor), "Scale", K({ { 0.0f, V(1, 1, 1, 1) }, { 1.0f, V(0.6f, 0.2f, 0.1f, 0) } }));
		return E;
	}
	case 4: // 마법 소용돌이 (GPU)
	{
		FParticleEmitter E = MakeBaseEmitter("MagicSwirl");
		E.SimTarget        = EParticleSimTarget::GPU;
		E.MaxParticles     = 20000;
		SetInput(AddModule(E, EParticleModuleType::SpawnRate), "SpawnRate", C(V(6000)));
		FParticleModule& Init = AddModule(E, EParticleModuleType::InitializeParticle);
		SetInput(Init, "Lifetime", R(V(2.0f), V(3.0f)));
		SetInput(Init, "Color", R(V(0.3f, 0.8f, 4.0f, 1.0f), V(1.5f, 0.4f, 4.0f, 1.0f)));
		SetInput(Init, "SpriteSize", C(V(4, 4)));
		FParticleModule& Shape = AddModule(E, EParticleModuleType::ShapeLocation);
		SetInput(Shape, "Shape", C(V(5)));
		SetInput(Shape, "Radius", C(V(100)));
		SetInput(Shape, "MinorRadius", C(V(10)));
		SetInput(Shape, "Offset", C(V(0, 0, 100)));
		FParticleModule& Vortex = AddModule(E, EParticleModuleType::VortexForce);
		SetInput(Vortex, "Center", C(V(0, 0, 100)));
		SetInput(Vortex, "Amount", C(V(500)));
		SetInput(Vortex, "PullIn", C(V(80)));
		FParticleModule& Curl = AddModule(E, EParticleModuleType::CurlNoiseForce);
		SetInput(Curl, "Strength", C(V(250)));
		SetInput(Curl, "Frequency", C(V(0.015f)));
		SetInput(AddModule(E, EParticleModuleType::Drag), "Drag", C(V(0.5f)));
		SetInput(AddModule(E, EParticleModuleType::ScaleColor), "Scale", K({ { 0.0f, V(1, 1, 1, 0) }, { 0.1f, V(1, 1, 1, 1) }, { 1.0f, V(1, 1, 1, 0) } }));
		return E;
	}
	case 5: // 비 (GPU)
	{
		FParticleEmitter E = MakeBaseEmitter("Rain");
		E.SimTarget                    = EParticleSimTarget::GPU;
		E.MaxParticles                 = 12000;
		E.Renderers[0].Alignment       = EParticleSpriteAlignment::Velocity;
		E.Renderers[0].VelocityStretch = 0.04f;
		SetInput(AddModule(E, EParticleModuleType::SpawnRate), "SpawnRate", C(V(5000)));
		FParticleModule& Init = AddModule(E, EParticleModuleType::InitializeParticle);
		SetInput(Init, "Lifetime", C(V(1.5f)));
		SetInput(Init, "Color", C(V(0.5f, 0.6f, 0.8f, 0.6f)));
		SetInput(Init, "SpriteSize", C(V(1.5f, 1.5f)));
		SetInput(Init, "SpriteRotation", C(V(0)));
		FParticleModule& Shape = AddModule(E, EParticleModuleType::ShapeLocation);
		SetInput(Shape, "Shape", C(V(2)));
		SetInput(Shape, "BoxSize", C(V(500, 500, 0)));
		SetInput(Shape, "Offset", C(V(0, 0, 700)));
		SetInput(AddModule(E, EParticleModuleType::AddVelocity), "Velocity", R(V(0, 0, -900), V(0, 0, -1100)));
		FParticleModule& Collision = AddModule(E, EParticleModuleType::Collision);
		SetInput(Collision, "KillOnCollide", C(V(1)));
		return E;
	}
	case 6: // 리본 꼬리
	{
		FParticleEmitter E = MakeBaseEmitter("RibbonTrail");
		E.Renderers[0].Type = EParticleRendererType::Ribbon;
		SetInput(AddModule(E, EParticleModuleType::SpawnRate), "SpawnRate", C(V(40)));
		FParticleModule& Init = AddModule(E, EParticleModuleType::InitializeParticle);
		SetInput(Init, "Lifetime", C(V(1.5f)));
		SetInput(Init, "Color", C(V(0.6f, 2.0f, 3.0f, 1.0f)));
		SetInput(Init, "SpriteSize", C(V(14, 14)));
		FParticleModule& Shape = AddModule(E, EParticleModuleType::ShapeLocation);
		SetInput(Shape, "Shape", C(V(0)));
		SetInput(Shape, "Offset", C(V(80, 0, 60)));
		SetInput(AddModule(E, EParticleModuleType::AddVelocity), "Velocity", C(V(0, 0, 60)));
		FParticleModule& Vortex = AddModule(E, EParticleModuleType::VortexForce);
		SetInput(Vortex, "Amount", C(V(900)));
		SetInput(Vortex, "PullIn", C(V(400)));
		SetInput(AddModule(E, EParticleModuleType::ScaleColor), "Scale", K({ { 0.0f, V(1, 1, 1, 1) }, { 1.0f, V(1, 1, 1, 0) } }));
		SetInput(AddModule(E, EParticleModuleType::ScaleSpriteSize), "Scale", K({ { 0.0f, V(1, 1) }, { 1.0f, V(0.1f, 0.1f) } }));
		return E;
	}
	default:
		return FParticleEmitter::MakeDefault();
	}
}

// ---- 에셋 --------------------------------------------------------------------------------------------------

bool FParticleEditor::LoadAsset(FAssetEditorEnvironment& Env)
{
	if (!System)
	{
		System = Env.Resources->LoadParticleSystem(Path);
		if (!System)
		{
			return false;
		}
	}
	// 열 때/되돌릴 때 파일 내용으로 맞춘다 (캐시가 이전 편집 상태일 수 있음)
	FParticleSystemAsset Loaded;
	if (!Loaded.LoadFromFile(Path))
	{
		return false;
	}
	*System = std::move(Loaded);
	Env.Resources->ResolveParticleResources(*System, Path.parent_path());
	SelectedEmitter = std::clamp(SelectedEmitter, 0, std::max(static_cast<int32>(System->Emitters.size()) - 1, 0));

	FScene& Scene = Preview.GetScene();
	if (!Scene.GetRegistry().IsValid(EmitterEntity))
	{
		EmitterEntity                       = Scene.CreateEntity("PreviewEmitter");
		FParticleSystemComponent& Component = Scene.GetRegistry().Emplace<FParticleSystemComponent>(EmitterEntity);
		Component.Runtime.System            = System;
		Component.Runtime.ResolvedAsset     = "(preview)";
		Component.Asset                     = "(preview)";
		Scene.UpdateTransforms();
	}
	RestartPreview();

	if (TextureFiles.empty() && Env.Editor != nullptr)
	{
		TextureFiles = FAssetEditorWidgets::ScanImageFiles(Env.Editor->ContentDirectory, Path.parent_path());
	}
	return true;
}

bool FParticleEditor::SaveAsset(FAssetEditorEnvironment& Env)
{
	(void)Env;
	return System->SaveToFile(Path);
}

std::string FParticleEditor::CaptureState() const
{
	return System ? System->ToJsonString() : std::string();
}

void FParticleEditor::RestoreState(FAssetEditorEnvironment& Env, const std::string& State)
{
	if (System && System->FromJsonString(State))
	{
		Env.Resources->ResolveParticleResources(*System, Path.parent_path());
		SelectedEmitter = std::clamp(SelectedEmitter, 0, std::max(static_cast<int32>(System->Emitters.size()) - 1, 0));
		RestartPreview();
	}
}

void FParticleEditor::RestartPreview()
{
	FParticleSystem::Restart(Preview.GetScene(), EmitterEntity);
}

void FParticleEditor::Update(FAssetEditorEnvironment& Env, float DeltaSeconds)
{
	(void)Env;
	if (!bPaused)
	{
		FParticleSystem::Update(Preview.GetScene(), DeltaSeconds);
	}
}

void FParticleEditor::FramePreview(FAssetEditorEnvironment& Env)
{
	(void)Env;
	// 입자는 메시 경계가 없으므로 이미터 주변 3m 높이를 기본 구도로
	Preview.GetOrbit().Frame(FBox(FVector3(-150.0f, -150.0f, 0.0f), FVector3(150.0f, 150.0f, 300.0f)), Preview.GetCamera().GetFovYDegrees(),
	                         Preview.GetCamera().GetAspectRatio());
}

void FParticleEditor::DrawPreviewToolbar(FAssetEditorEnvironment& Env)
{
	FAssetEditor::DrawPreviewToolbar(Env);
	ImGui::SameLine();
	ImGui::TextDisabled("|");
	ImGui::SameLine();
	if (FEditorTheme::ToolButton(bPaused ? ICON_FA_PLAY : ICON_FA_PAUSE, bPaused ? "재생" : "일시정지", false))
	{
		bPaused = !bPaused;
	}
	ImGui::SameLine();
	if (FEditorTheme::ToolButton(ICON_FA_ROTATE_LEFT, "처음부터 다시", false))
	{
		RestartPreview();
		bPaused = false;
	}
	ImGui::SameLine();
	ImGui::TextDisabled("입자 %u", FParticleSystem::CountParticles(Preview.GetScene()));
}

// ---- UI ----------------------------------------------------------------------------------------------------

bool FParticleEditor::DrawEmitterList(bool& bOutStructure)
{
	bool   bEdited = false;
	auto&  Emitters = System->Emitters;
	ImGui::SeparatorText(ICON_FA_LAYER_GROUP "  이미터");

	int32 MoveFrom = -1, MoveTo = -1, Remove = -1;
	for (int32 Index = 0; Index < static_cast<int32>(Emitters.size()); ++Index)
	{
		FParticleEmitter& Emitter = Emitters[Index];
		ImGui::PushID(Index);
		if (ImGui::Checkbox("##Enabled", &Emitter.bEnabled))
		{
			bEdited = true;
		}
		ImGui::SetItemTooltip("이미터 켜기/끄기");
		ImGui::SameLine();
		const bool bGpu = Emitter.SimTarget == EParticleSimTarget::GPU;
		ImGui::PushStyleColor(ImGuiCol_Text, bGpu ? FEditorTheme::Warning : FEditorTheme::Success);
		ImGui::TextUnformatted(bGpu ? "GPU" : "CPU");
		ImGui::PopStyleColor();
		ImGui::SameLine();
		const ImGuiStyle& Style        = ImGui::GetStyle();
		const auto        IconWidth    = [&](const char* Icon) { return ImGui::CalcTextSize(Icon).x + Style.FramePadding.x * 2.0f + Style.ItemSpacing.x; };
		const float       ButtonsWidth = IconWidth(ICON_FA_ARROW_UP) + IconWidth(ICON_FA_ARROW_DOWN) + IconWidth(ICON_FA_TRASH) + Style.ItemSpacing.x;
		if (ImGui::Selectable(Emitter.Name.c_str(), SelectedEmitter == Index, ImGuiSelectableFlags_AllowOverlap,
		                      ImVec2(ImGui::GetContentRegionAvail().x - ButtonsWidth, 0)))
		{
			SelectedEmitter = Index;
		}
		ImGui::SameLine();
		ImGui::BeginDisabled(Index == 0);
		if (HeaderIconButton(ICON_FA_ARROW_UP, "위로 (먼저 그림)"))
		{
			MoveFrom = Index;
			MoveTo   = Index - 1;
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginDisabled(Index + 1 >= static_cast<int32>(Emitters.size()));
		if (HeaderIconButton(ICON_FA_ARROW_DOWN, "아래로"))
		{
			MoveFrom = Index;
			MoveTo   = Index + 1;
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (HeaderIconButton(ICON_FA_TRASH, "이미터 삭제"))
		{
			Remove = Index;
		}
		ImGui::PopID();
	}

	if (ImGui::Button(ICON_FA_PLUS "  이미터 추가"))
	{
		ImGui::OpenPopup("##AddEmitter");
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(Emitters.empty());
	if (ImGui::Button(ICON_FA_COPY "  복제"))
	{
		FParticleEmitter Copy = Emitters[SelectedEmitter];
		Copy.Name += "_Copy";
		Emitters.insert(Emitters.begin() + SelectedEmitter + 1, std::move(Copy));
		++SelectedEmitter;
		bEdited = bOutStructure = true;
	}
	ImGui::EndDisabled();
	if (ImGui::BeginPopup("##AddEmitter"))
	{
		ImGui::TextDisabled("템플릿에서 시작");
		const std::vector<const char*> Names = GetTemplateNames();
		for (size_t Index = 0; Index < Names.size(); ++Index)
		{
			if (ImGui::Selectable(Names[Index]))
			{
				Emitters.push_back(MakeTemplate(Index));
				SelectedEmitter = static_cast<int32>(Emitters.size()) - 1;
				bEdited = bOutStructure = true;
			}
		}
		ImGui::EndPopup();
	}

	if (MoveFrom >= 0)
	{
		std::swap(Emitters[MoveFrom], Emitters[MoveTo]);
		if (SelectedEmitter == MoveFrom)
		{
			SelectedEmitter = MoveTo;
		}
		else if (SelectedEmitter == MoveTo)
		{
			SelectedEmitter = MoveFrom;
		}
		bEdited = bOutStructure = true;
	}
	if (Remove >= 0)
	{
		Emitters.erase(Emitters.begin() + Remove);
		SelectedEmitter = std::clamp(SelectedEmitter, 0, std::max(static_cast<int32>(Emitters.size()) - 1, 0));
		bEdited = bOutStructure = true;
	}
	return bEdited;
}

bool FParticleEditor::DrawEmitterSettings(FParticleEmitter& Emitter, bool& bOutRestart)
{
	bool bEdited = false;
	ImGui::SeparatorText(ICON_FA_SLIDERS "  이미터 속성");

	char NameBuffer[128];
	std::snprintf(NameBuffer, sizeof(NameBuffer), "%s", Emitter.Name.c_str());
	if (ImGui::InputText("이름", NameBuffer, sizeof(NameBuffer)))
	{
		Emitter.Name = NameBuffer;
		bEdited      = true;
	}

	int32 Target = static_cast<int32>(Emitter.SimTarget);
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted("계산 방식");
	ImGui::SameLine();
	bool bTargetChanged = ImGui::RadioButton("CPU", &Target, 0);
	ImGui::SameLine();
	bTargetChanged |= ImGui::RadioButton("GPU", &Target, 1);
	if (bTargetChanged && Target != static_cast<int32>(Emitter.SimTarget))
	{
		Emitter.SimTarget = static_cast<EParticleSimTarget>(Target);
		const uint32 Limit = Emitter.SimTarget == EParticleSimTarget::GPU ? GMaxGpuParticles : GMaxCpuParticles;
		Emitter.MaxParticles = std::min(Emitter.MaxParticles, Limit);
		bEdited = bOutRestart = true;
	}
	FAssetEditorWidgets::Hint(Emitter.SimTarget == EParticleSimTarget::GPU
	                              ? "GPU: 수만 개도 가볍게 계산합니다. 반투명 정렬과 리본은 지원하지 않습니다."
	                              : "CPU: 수천 개까지 알맞습니다. 반투명 입자를 뒤에서 앞으로 정렬하고, 리본을 그릴 수 있습니다.");
	if (Emitter.SimTarget == EParticleSimTarget::GPU)
	{
		for (const FParticleRendererSettings& Renderer : Emitter.Renderers)
		{
			if (Renderer.bEnabled && Renderer.Type == EParticleRendererType::Ribbon)
			{
				ImGui::TextColored(FEditorTheme::Warning, ICON_FA_TRIANGLE_EXCLAMATION "  리본 렌더러는 GPU 계산에서 그려지지 않습니다");
				break;
			}
		}
	}

	if (ImGui::Checkbox("로컬 공간 (입자가 이미터를 따라 움직임)", &Emitter.bLocalSpace))
	{
		bEdited = bOutRestart = true;
	}
	bEdited |= ImGui::DragFloat("주기 (초)", &Emitter.Duration, 0.01f, 0.01f, 600.0f, "%.2f");
	if (ImGui::Checkbox("반복", &Emitter.bLoop))
	{
		bEdited = bOutRestart = true;
	}
	const uint32 Limit        = Emitter.SimTarget == EParticleSimTarget::GPU ? GMaxGpuParticles : GMaxCpuParticles;
	int32        MaxParticles = static_cast<int32>(Emitter.MaxParticles);
	if (ImGui::DragInt("최대 개수", &MaxParticles, Emitter.SimTarget == EParticleSimTarget::GPU ? 100.0f : 1.0f, 1, static_cast<int32>(Limit)))
	{
		Emitter.MaxParticles = static_cast<uint32>(std::clamp(MaxParticles, 1, static_cast<int32>(Limit)));
		bEdited              = true;
	}
	int32 Seed = static_cast<int32>(Emitter.Seed);
	if (ImGui::DragInt("무작위 시드", &Seed, 1.0f, 0, 1000000))
	{
		Emitter.Seed = static_cast<uint32>(std::max(Seed, 0));
		bEdited = bOutRestart = true;
	}
	return bEdited;
}

bool FParticleEditor::DrawModuleStacks(FParticleEmitter& Emitter, bool& bOutRestart)
{
	bool bEdited = false;
	for (size_t StageIndex = 0; StageIndex < static_cast<size_t>(EParticleStage::Count); ++StageIndex)
	{
		const EParticleStage Stage   = static_cast<EParticleStage>(StageIndex);
		auto&                Modules = Emitter.GetStage(Stage);
		ImGui::PushID(static_cast<int32>(StageIndex));

		const ImVec4 Color = StageColor(Stage);
		ImGui::PushStyleColor(ImGuiCol_Header, Color);
		ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(Color.x * 1.2f, Color.y * 1.2f, Color.z * 1.2f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(Color.x * 1.3f, Color.y * 1.3f, Color.z * 1.3f, 1.0f));
		const bool bStageOpen = ImGui::CollapsingHeader(StageLabel(Stage), ImGuiTreeNodeFlags_DefaultOpen);
		ImGui::PopStyleColor(3);
		if (!bStageOpen)
		{
			ImGui::PopID();
			continue;
		}

		int32 MoveFrom = -1, MoveTo = -1, Remove = -1;
		for (int32 Index = 0; Index < static_cast<int32>(Modules.size()); ++Index)
		{
			FParticleModule&           Module = Modules[Index];
			const FParticleModuleInfo& Info   = Module.GetInfo();
			ImGui::PushID(Index);

			const float RowStartX = ImGui::GetCursorPosX();
			ImGui::BeginDisabled(!Module.bEnabled);
			const bool bOpen = ImGui::TreeNodeEx("##Module", ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap |
			                                                     ImGuiTreeNodeFlags_SpanAvailWidth,
			                                     "%s", Info.DisplayName);
			ImGui::EndDisabled();
			ImGui::SetItemTooltip("%s\n(%s)\n우클릭: 순서 바꾸기 / 삭제", Info.Description, Info.Id);
			if (ImGui::BeginPopupContextItem("##ModuleMenu"))
			{
				if (ImGui::MenuItem(ICON_FA_ARROW_UP "  위로 (먼저 실행)", nullptr, false, Index > 0))
				{
					MoveFrom = Index;
					MoveTo   = Index - 1;
				}
				if (ImGui::MenuItem(ICON_FA_ARROW_DOWN "  아래로", nullptr, false, Index + 1 < static_cast<int32>(Modules.size())))
				{
					MoveFrom = Index;
					MoveTo   = Index + 1;
				}
				if (ImGui::MenuItem(ICON_FA_XMARK "  삭제"))
				{
					Remove = Index;
				}
				ImGui::EndPopup();
			}

			// 머리 오른쪽: 켜기 / 삭제
			SameLineAtRight(RowStartX, Info.DisplayName, RightButtonsWidth({ ICON_FA_XMARK }, true));
			if (ImGui::Checkbox("##On", &Module.bEnabled))
			{
				bEdited = bOutRestart = true;
			}
			ImGui::SetItemTooltip("모듈 켜기/끄기");
			ImGui::SameLine();
			if (HeaderIconButton(ICON_FA_XMARK, "모듈 삭제"))
			{
				Remove = Index;
			}

			if (bOpen)
			{
				ImGui::BeginDisabled(!Module.bEnabled);
				// 입력: 이름 | 값 두 칸. 곡선은 폭이 필요해 한 줄 전체를 쓴다 (표를 끊었다가 다시 연다)
				bool bTableOpen = false;
				for (size_t Input = 0; Input < Info.Inputs.size() && Input < Module.Inputs.size(); ++Input)
				{
					const FParticleInputInfo& InputInfo = Info.Inputs[Input];
					FParticleValue&           Value     = Module.Inputs[Input];
					const bool                bCurve    = Value.Mode == EParticleValueMode::Curve;
					// 표 열기/닫기는 입력별 ID 범위 밖에서 (ImGui ID 스택 짝 맞춤)
					if (bCurve && bTableOpen)
					{
						ImGui::EndTable();
						bTableOpen = false;
					}
					else if (!bCurve && !bTableOpen)
					{
						char TableId[32];
						std::snprintf(TableId, sizeof(TableId), "##Inputs%zu", Input);
						bTableOpen = ImGui::BeginTable(TableId, 2, ImGuiTableFlags_SizingStretchProp);
						if (bTableOpen)
						{
							ImGui::TableSetupColumn("이름", ImGuiTableColumnFlags_WidthStretch, 0.38f);
							ImGui::TableSetupColumn("값", ImGuiTableColumnFlags_WidthStretch, 0.62f);
						}
					}
					bool bChanged = false;
					ImGui::PushID(static_cast<int32>(Input));
					if (bCurve)
					{
						ImGui::AlignTextToFramePadding();
						ImGui::TextUnformatted(InputInfo.DisplayName);
						ImGui::SameLine();
						bChanged = DrawDynamicInput(InputInfo, Value);
					}
					else if (bTableOpen)
					{
						ImGui::TableNextRow();
						ImGui::TableSetColumnIndex(0);
						ImGui::AlignTextToFramePadding();
						ImGui::TextUnformatted(InputInfo.DisplayName);
						ImGui::SetItemTooltip("%s", InputInfo.DisplayName); // 좁을 때 잘린 이름
						ImGui::TableSetColumnIndex(1);
						bChanged = DrawDynamicInput(InputInfo, Value);
					}
					ImGui::PopID();
					if (bChanged)
					{
						bEdited = true;
						// 이미터 단계 값(버스트 시각 등)은 다시 시작해야 바로 보인다
						bOutRestart |= Stage == EParticleStage::EmitterUpdate;
					}
				}
				if (bTableOpen)
				{
					ImGui::EndTable();
				}
				ImGui::EndDisabled();
				ImGui::TreePop();
			}
			ImGui::PopID();
		}

		if (ImGui::Button(ICON_FA_PLUS "  모듈 추가"))
		{
			ImGui::OpenPopup("##AddModule");
		}
		if (ImGui::BeginPopup("##AddModule"))
		{
			for (const FParticleModuleInfo& Info : GetParticleModuleInfos())
			{
				if (Info.Stage != Stage)
				{
					continue;
				}
				if (ImGui::Selectable(Info.DisplayName))
				{
					Modules.push_back(FParticleModule::Make(Info.Type));
					bEdited = bOutRestart = true;
				}
				ImGui::SetItemTooltip("%s", Info.Description);
			}
			ImGui::EndPopup();
		}
		if (MoveFrom >= 0)
		{
			std::swap(Modules[MoveFrom], Modules[MoveTo]);
			bEdited = bOutRestart = true;
		}
		if (Remove >= 0)
		{
			Modules.erase(Modules.begin() + Remove);
			bEdited = bOutRestart = true;
		}
		ImGui::Spacing();
		ImGui::PopID();
	}
	return bEdited;
}

bool FParticleEditor::DrawRenderers(FParticleEmitter& Emitter, bool& bOutResolve)
{
	bool bEdited = false;
	ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.18f, 0.28f, 0.50f, 1.0f));
	const bool bOpen = ImGui::CollapsingHeader(ICON_FA_PAINTBRUSH "  렌더러 (어떻게 그릴지)", ImGuiTreeNodeFlags_DefaultOpen);
	ImGui::PopStyleColor();
	if (!bOpen)
	{
		return false;
	}

	int32 Remove = -1;
	for (int32 Index = 0; Index < static_cast<int32>(Emitter.Renderers.size()); ++Index)
	{
		FParticleRendererSettings& R = Emitter.Renderers[Index];
		const float RowStartX = ImGui::GetCursorPosX();
		ImGui::PushID(Index);
		const bool bNodeOpen = ImGui::TreeNodeEx("##Renderer", ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap |
		                                                           ImGuiTreeNodeFlags_SpanAvailWidth,
		                                         "%s", RendererLabel(R.Type));
		SameLineAtRight(RowStartX, RendererLabel(R.Type), RightButtonsWidth({ ICON_FA_XMARK }, true));
		bEdited |= ImGui::Checkbox("##On", &R.bEnabled);
		ImGui::SameLine();
		if (HeaderIconButton(ICON_FA_XMARK, "렌더러 삭제"))
		{
			Remove = Index;
		}
		if (bNodeOpen)
		{
			int32 Type = static_cast<int32>(R.Type);
			if (ImGui::Combo("종류", &Type, "스프라이트 (판)\0메시\0리본 (띠)\0"))
			{
				R.Type  = static_cast<EParticleRendererType>(Type);
				bEdited = bOutResolve = true;
			}
			int32 Blend = static_cast<int32>(R.BlendMode);
			if (ImGui::Combo("블렌드", &Blend, "반투명 (연기)\0가산 (불꽃/빛)\0"))
			{
				R.BlendMode = static_cast<EParticleBlendMode>(Blend);
				bEdited     = true;
			}
			if (FAssetEditorWidgets::TextureCombo("텍스처", R.TexturePath, TextureFiles, "(기본: 부드러운 원)"))
			{
				bEdited = bOutResolve = true;
			}
			if (R.Type == EParticleRendererType::Sprite)
			{
				int32 Grid[2] = { R.SubImageColumns, R.SubImageRows };
				if (ImGui::DragInt2("플립북 칸 (가로/세로)", Grid, 0.1f, 1, 64))
				{
					R.SubImageColumns = std::clamp(Grid[0], 1, 64);
					R.SubImageRows    = std::clamp(Grid[1], 1, 64);
					bEdited           = true;
				}
				int32 Alignment = static_cast<int32>(R.Alignment);
				if (ImGui::Combo("방향", &Alignment, "카메라를 향함\0속도 방향으로 늘임\0"))
				{
					R.Alignment = static_cast<EParticleSpriteAlignment>(Alignment);
					bEdited     = true;
				}
				if (R.Alignment == EParticleSpriteAlignment::Velocity)
				{
					bEdited |= ImGui::DragFloat("늘임 (초)", &R.VelocityStretch, 0.001f, 0.0f, 1.0f, "%.3f");
				}
			}
			else if (R.Type == EParticleRendererType::Mesh)
			{
				int32 Mesh = R.MeshAsset == "primitive:cube" ? 1 : 0;
				if (ImGui::Combo("메시", &Mesh, "구\0큐브\0"))
				{
					R.MeshAsset = Mesh == 1 ? "primitive:cube" : "primitive:sphere";
					bEdited = bOutResolve = true;
				}
				FAssetEditorWidgets::Hint("입자 크기 X가 메시 지름(cm)이 됩니다.");
			}
			else
			{
				bEdited |= ImGui::DragFloat("폭 배율", &R.RibbonWidthScale, 0.01f, 0.0f, 20.0f, "%.2f");
				FAssetEditorWidgets::Hint("입자를 태어난 순서대로 이어 띠를 만듭니다. 폭 = 입자 크기 X × 배율.");
			}
			ImGui::TreePop();
		}
		ImGui::PopID();
	}
	if (ImGui::Button(ICON_FA_PLUS "  렌더러 추가"))
	{
		ImGui::OpenPopup("##AddRenderer");
	}
	if (ImGui::BeginPopup("##AddRenderer"))
	{
		const char* Labels[] = { "스프라이트", "메시", "리본" };
		for (int32 Type = 0; Type < 3; ++Type)
		{
			if (ImGui::Selectable(Labels[Type]))
			{
				FParticleRendererSettings Renderer;
				Renderer.Type = static_cast<EParticleRendererType>(Type);
				Emitter.Renderers.push_back(Renderer);
				bEdited = bOutResolve = true;
			}
		}
		ImGui::EndPopup();
	}
	if (Remove >= 0)
	{
		Emitter.Renderers.erase(Emitter.Renderers.begin() + Remove);
		bEdited = true;
	}
	return bEdited;
}

void FParticleEditor::DrawProperties(FAssetEditorEnvironment& Env)
{
	bool bStructure = false; // 이미터 추가/삭제/순서 → 미리보기를 처음부터
	bool bRestart   = false;
	bool bResolve   = false; // 텍스처/메시 다시 해석
	bool bEdited    = DrawEmitterList(bStructure);

	if (!System->Emitters.empty())
	{
		SelectedEmitter           = std::clamp(SelectedEmitter, 0, static_cast<int32>(System->Emitters.size()) - 1);
		FParticleEmitter& Emitter = System->Emitters[SelectedEmitter];
		ImGui::PushID(SelectedEmitter);
		bEdited |= DrawEmitterSettings(Emitter, bRestart);
		ImGui::Spacing();
		bEdited |= DrawModuleStacks(Emitter, bRestart);
		bEdited |= DrawRenderers(Emitter, bResolve);
		ImGui::PopID();
	}
	else
	{
		FAssetEditorWidgets::Hint("이미터가 없습니다. \"이미터 추가\"로 템플릿에서 시작하세요.");
	}

	if (bResolve || bStructure)
	{
		Env.Resources->ResolveParticleResources(*System, Path.parent_path());
	}
	if (bRestart || bStructure)
	{
		RestartPreview();
	}
	if (bEdited)
	{
		MarkEdited("파티클 값 변경");
	}

	ImGui::Spacing();
	FAssetEditorWidgets::Hint("열린 씬에서 이 에셋을 쓰는 파티클에 바로 반영됩니다. 저장하지 않고 닫으면 원래대로 돌아갑니다.");
}
