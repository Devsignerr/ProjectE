#include "Scene/AnimGraph.h"

#include "Core/FileSystem.h"
#include "Core/Log.h"
#include "Core/StringConv.h"
#include "Scene/Prefab.h"

#include <json.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cwctype>
#include <format>
#include <fstream>
#include <functional>

E_DECLARE_LOG_CATEGORY(LogScene)

namespace
{
	using nlohmann::json;

	constexpr size_t MaxLayers = 8; // 페이드 중 전이가 계속 와도 이 이상 쌓지 않는다 (가장 오래된 것부터 버림)

	bool ParseOp(const std::string& Text, EAnimConditionOp& Out)
	{
		static const std::pair<const char*, EAnimConditionOp> Table[] = {
			{ "<", EAnimConditionOp::Less },     { "<=", EAnimConditionOp::LessEqual }, { ">", EAnimConditionOp::Greater },
			{ ">=", EAnimConditionOp::GreaterEqual }, { "==", EAnimConditionOp::Equal }, { "!=", EAnimConditionOp::NotEqual },
		};
		for (const auto& [Name, Op] : Table)
		{
			if (Text == Name)
			{
				Out = Op;
				return true;
			}
		}
		return false;
	}

	float ReadNumberOrBool(const json& Node, float Fallback)
	{
		if (Node.is_boolean())
		{
			return Node.get<bool>() ? 1.0f : 0.0f;
		}
		return Node.is_number() ? Node.get<float>() : Fallback;
	}

	// 정규화 시간 진행. 루프면 [0, 1)로 감고 bOutWrapped
	float AdvancePhase(float Phase, float Delta, bool bLoop, bool& bOutWrapped)
	{
		bOutWrapped      = false;
		const float Next = Phase + Delta;
		if (!bLoop)
		{
			return FMath::Clamp(Next, 0.0f, 1.0f);
		}
		if (Next >= 0.0f && Next < 1.0f)
		{
			return Next;
		}
		bOutWrapped = true;
		return Next - std::floor(Next);
	}

	const char* OpToString(EAnimConditionOp Op)
	{
		switch (Op)
		{
		case EAnimConditionOp::Less:         return "<";
		case EAnimConditionOp::LessEqual:    return "<=";
		case EAnimConditionOp::Greater:      return ">";
		case EAnimConditionOp::GreaterEqual: return ">=";
		case EAnimConditionOp::Equal:        return "==";
		case EAnimConditionOp::NotEqual:     return "!=";
		}
		return "==";
	}

	std::optional<FVector2> ReadPosition(const json& Node, const char* Key)
	{
		const auto Found = Node.find(Key);
		if (Found == Node.end() || !Found->is_array() || Found->size() != 2 || !(*Found)[0].is_number() || !(*Found)[1].is_number())
		{
			return std::nullopt;
		}
		return FVector2((*Found)[0].get<float>(), (*Found)[1].get<float>());
	}

	json WritePosition(const FVector2& Position) { return json::array({ Position.X, Position.Y }); }
} // namespace

// ---------------------------------------------------------------- 에셋

int32 FAnimStateMachine::FindState(std::string_view Name) const
{
	for (size_t Index = 0; Index < States.size(); ++Index)
	{
		if (States[Index].Name == Name)
		{
			return static_cast<int32>(Index);
		}
	}
	return -1;
}

void FAnimStateMachine::RemoveState(int32 Index)
{
	if (Index < 0 || Index >= static_cast<int32>(States.size()))
	{
		return;
	}
	States.erase(States.begin() + Index);
	std::erase_if(Transitions, [Index](const FAnimGraphTransition& Transition) { return Transition.From == Index || Transition.To == Index; });
	for (FAnimGraphTransition& Transition : Transitions)
	{
		Transition.From = Transition.From > Index ? Transition.From - 1 : Transition.From;
		Transition.To   = Transition.To > Index ? Transition.To - 1 : Transition.To;
	}
	if (EntryState == Index)
	{
		EntryState = 0;
	}
	else if (EntryState > Index)
	{
		--EntryState;
	}
}

const FAnimGraphParameter* FAnimGraphAsset::FindParameter(std::string_view Name) const
{
	for (const FAnimGraphParameter& Parameter : Parameters)
	{
		if (Parameter.Name == Name)
		{
			return &Parameter;
		}
	}
	return nullptr;
}

int32 FAnimGraphAsset::FindSlot(std::string_view Name) const
{
	for (size_t Index = 0; Index < Slots.size(); ++Index)
	{
		if (Slots[Index].Name == Name)
		{
			return static_cast<int32>(Index);
		}
	}
	return -1;
}

int32 FAnimGraphAsset::FindLayer(std::string_view Name) const
{
	for (size_t Index = 0; Index < Layers.size(); ++Index)
	{
		if (Layers[Index].Name == Name)
		{
			return static_cast<int32>(Index);
		}
	}
	return -1;
}

namespace
{
	using FWarnFunction = std::function<void(std::string)>;

	// 상태/전이/시작 상태 (기본 레이어와 추가 레이어 공용). 실패하면 OutError + false. Label = 경고 앞머리 ("" = 기본 레이어)
	bool ParseMachine(const json& Node, const FAnimGraphAsset& Asset, const std::string& Label, FAnimStateMachine& Out, std::string& OutError,
	                  const FWarnFunction& Warn)
	{
		const auto States = Node.find("States");
		if (States == Node.end() || !States->is_array() || States->empty())
		{
			OutError = Label + "상태(States)가 없습니다";
			return false;
		}
		for (const json& StateNode : *States)
		{
			FAnimGraphState State;
			State.Name            = StateNode.value("Name", std::string());
			State.BlendParameter  = StateNode.value("BlendParameter", std::string());
			State.BlendParameterY = State.BlendParameter.empty() ? std::string() : StateNode.value("BlendParameterY", std::string());
			State.Speed           = StateNode.value("Speed", 1.0f);
			State.bLoop           = StateNode.value("Loop", true);
			State.EditorPosition  = ReadPosition(StateNode, "EditorPosition");
			if (const auto Samples = StateNode.find("Samples"); Samples != StateNode.end() && Samples->is_array())
			{
				for (const json& SampleNode : *Samples)
				{
					FAnimBlendSample Sample;
					Sample.Clip = SampleNode.value("Clip", std::string());
					if (const auto Position = SampleNode.find("Position"); Position != SampleNode.end() && Position->is_array())
					{
						Sample.Position  = Position->size() > 0 && (*Position)[0].is_number() ? (*Position)[0].get<float>() : 0.0f;
						Sample.PositionY = Position->size() > 1 && (*Position)[1].is_number() ? (*Position)[1].get<float>() : 0.0f;
					}
					else
					{
						Sample.Position = SampleNode.value("Position", 0.0f);
					}
					Sample.Rate = SampleNode.value("Rate", 1.0f);
					State.Samples.push_back(std::move(Sample));
				}
			}
			else if (StateNode.contains("Clip"))
			{
				FAnimBlendSample Sample;
				Sample.Clip = StateNode.value("Clip", std::string());
				Sample.Rate = StateNode.value("Rate", 1.0f);
				State.Samples.push_back(std::move(Sample));
			}
			if (State.Name.empty() || State.Samples.empty())
			{
				OutError = std::format("{}상태 {}번: 이름과 클립(Clip 또는 Samples)이 필요합니다", Label, Out.States.size());
				return false;
			}
			if (Out.FindState(State.Name) >= 0)
			{
				OutError = std::format("{}상태 이름이 겹칩니다: {}", Label, State.Name);
				return false;
			}
			if (!State.Is2D())
			{
				std::stable_sort(State.Samples.begin(), State.Samples.end(), [](const FAnimBlendSample& A, const FAnimBlendSample& B) { return A.Position < B.Position; });
			}
			Out.States.push_back(std::move(State));
		}

		const std::string Entry = Node.value("EntryState", std::string());
		Out.EntryState          = Entry.empty() ? 0 : Out.FindState(Entry);
		if (Out.EntryState < 0)
		{
			OutError = std::format("{}시작 상태를 찾을 수 없습니다: {}", Label, Entry);
			return false;
		}

		if (const auto Found = Node.find("Transitions"); Found != Node.end() && Found->is_array())
		{
			for (const json& TransitionNode : *Found)
			{
				FAnimGraphTransition Transition;
				const std::string    From = TransitionNode.value("From", std::string("*"));
				const std::string    To   = TransitionNode.value("To", std::string());
				Transition.From           = From == "*" ? -1 : Out.FindState(From);
				Transition.To             = Out.FindState(To);
				Transition.Duration       = std::max(0.0f, TransitionNode.value("Duration", 0.2f));
				Transition.ExitTime       = TransitionNode.value("ExitTime", -1.0f);
				if ((From != "*" && Transition.From < 0) || Transition.To < 0)
				{
					Warn(std::format("{}전이 {} → {}: 상태를 찾을 수 없어 건너뜁니다", Label, From, To));
					continue;
				}
				bool bValid = true;
				if (const auto Conditions = TransitionNode.find("Conditions"); Conditions != TransitionNode.end() && Conditions->is_array())
				{
					for (const json& ConditionNode : *Conditions)
					{
						FAnimTransitionCondition Condition;
						Condition.Parameter = ConditionNode.value("Parameter", std::string());
						Condition.Value     = ConditionNode.contains("Value") ? ReadNumberOrBool(ConditionNode["Value"], 0.0f) : 1.0f;
						if (Condition.Parameter.empty() || !ParseOp(ConditionNode.value("Op", std::string("==")), Condition.Op))
						{
							Warn(std::format("{}전이 {} → {}: 잘못된 조건 (Parameter/Op)", Label, From, To));
							bValid = false;
							break;
						}
						if (Asset.FindParameter(Condition.Parameter) == nullptr)
						{
							Warn(std::format("{}전이 {} → {}: 선언되지 않은 파라미터 '{}' (기본값 0)", Label, From, To, Condition.Parameter));
						}
						Transition.Conditions.push_back(std::move(Condition));
					}
				}
				if (bValid)
				{
					Out.Transitions.push_back(std::move(Transition));
				}
			}
		}
		return true;
	}

	FAnimBoneMask ParseMask(const json& Node)
	{
		FAnimBoneMask Mask;
		if (Node.is_array())
		{
			for (const json& EntryNode : Node)
			{
				FAnimBoneMaskEntry Entry;
				Entry.Bone       = EntryNode.value("Bone", std::string());
				Entry.Weight     = EntryNode.value("Weight", 1.0f);
				Entry.BlendDepth = EntryNode.value("BlendDepth", 0);
				if (!Entry.Bone.empty())
				{
					Mask.Bones.push_back(std::move(Entry));
				}
			}
		}
		return Mask;
	}

	json WriteMask(const FAnimBoneMask& Mask)
	{
		json Array = json::array();
		for (const FAnimBoneMaskEntry& Entry : Mask.Bones)
		{
			json Node    = json::object();
			Node["Bone"] = Entry.Bone;
			if (Entry.Weight != 1.0f)
			{
				Node["Weight"] = Entry.Weight;
			}
			if (Entry.BlendDepth != 0)
			{
				Node["BlendDepth"] = Entry.BlendDepth;
			}
			Array.push_back(std::move(Node));
		}
		return Array;
	}

	// 상태/전이/시작 상태를 Root에 쓴다 (기본 레이어와 추가 레이어 공용)
	void WriteMachine(const FAnimStateMachine& Machine, const FAnimGraphAsset& Asset, json& Root)
	{
		const auto IsBoolParameter = [&Asset](const std::string& Name) {
			const FAnimGraphParameter* Parameter = Asset.FindParameter(Name);
			return Parameter != nullptr && Parameter->Type == EAnimParamType::Bool;
		};
		if (Machine.EntryState >= 0 && Machine.EntryState < static_cast<int32>(Machine.States.size()))
		{
			Root["EntryState"] = Machine.States[static_cast<size_t>(Machine.EntryState)].Name;
		}

		json StateArray = json::array();
		for (const FAnimGraphState& State : Machine.States)
		{
			json Node    = json::object();
			Node["Name"] = State.Name;
			if (State.BlendParameter.empty() && State.Samples.size() == 1)
			{
				Node["Clip"] = State.Samples.front().Clip;
				if (State.Samples.front().Rate != 1.0f)
				{
					Node["Rate"] = State.Samples.front().Rate;
				}
			}
			else
			{
				if (!State.BlendParameter.empty())
				{
					Node["BlendParameter"] = State.BlendParameter;
				}
				if (State.Is2D())
				{
					Node["BlendParameterY"] = State.BlendParameterY;
				}
				json Samples = json::array();
				for (const FAnimBlendSample& Sample : State.Samples)
				{
					json SampleNode        = json::object();
					SampleNode["Clip"]     = Sample.Clip;
					SampleNode["Position"] = State.Is2D() ? json::array({ Sample.Position, Sample.PositionY }) : json(Sample.Position);
					if (Sample.Rate != 1.0f)
					{
						SampleNode["Rate"] = Sample.Rate;
					}
					Samples.push_back(std::move(SampleNode));
				}
				Node["Samples"] = std::move(Samples);
			}
			if (State.Speed != 1.0f)
			{
				Node["Speed"] = State.Speed;
			}
			if (!State.bLoop)
			{
				Node["Loop"] = false;
			}
			if (State.EditorPosition)
			{
				Node["EditorPosition"] = WritePosition(*State.EditorPosition);
			}
			StateArray.push_back(std::move(Node));
		}
		Root["States"] = std::move(StateArray);

		json TransitionArray = json::array();
		for (const FAnimGraphTransition& Transition : Machine.Transitions)
		{
			const bool bValidTo   = Transition.To >= 0 && Transition.To < static_cast<int32>(Machine.States.size());
			const bool bValidFrom = Transition.From < static_cast<int32>(Machine.States.size());
			if (!bValidTo || !bValidFrom)
			{
				continue;
			}
			json Node        = json::object();
			Node["From"]     = Transition.From < 0 ? std::string("*") : Machine.States[static_cast<size_t>(Transition.From)].Name;
			Node["To"]       = Machine.States[static_cast<size_t>(Transition.To)].Name;
			Node["Duration"] = Transition.Duration;
			if (Transition.ExitTime >= 0.0f)
			{
				Node["ExitTime"] = Transition.ExitTime;
			}
			json Conditions = json::array();
			for (const FAnimTransitionCondition& Condition : Transition.Conditions)
			{
				json ConditionNode         = json::object();
				ConditionNode["Parameter"] = Condition.Parameter;
				ConditionNode["Op"]        = OpToString(Condition.Op);
				if (IsBoolParameter(Condition.Parameter))
				{
					ConditionNode["Value"] = Condition.Value != 0.0f;
				}
				else
				{
					ConditionNode["Value"] = Condition.Value;
				}
				Conditions.push_back(std::move(ConditionNode));
			}
			Node["Conditions"] = std::move(Conditions);
			TransitionArray.push_back(std::move(Node));
		}
		Root["Transitions"] = std::move(TransitionArray);
	}
} // namespace

bool FAnimGraphAsset::FromJsonString(const std::string& Text, FAnimGraphAsset& Out, std::string* OutError, std::vector<std::string>* OutWarnings)
{
	const auto Fail = [OutError](std::string Message) {
		if (OutError != nullptr)
		{
			*OutError = std::move(Message);
		}
		return false;
	};
	const FWarnFunction Warn = [OutWarnings](std::string Message) {
		if (OutWarnings != nullptr)
		{
			OutWarnings->push_back(std::move(Message));
		}
	};

	const json Root = json::parse(Text, nullptr, false);
	if (!Root.is_object())
	{
		return Fail("JSON 객체가 아닙니다");
	}
	FAnimGraphAsset Asset;
	if (const int32 FileVersion = Root.value("Version", 1); FileVersion > Version)
	{
		Warn(std::format("이 엔진보다 새 형식 버전입니다 ({} > {}) — 아는 필드만 읽습니다", FileVersion, Version));
	}
	if (const auto Editor = Root.find("Editor"); Editor != Root.end() && Editor->is_object())
	{
		Asset.PreviewModel           = Editor->value("PreviewModel", std::string());
		Asset.AnyStateEditorPosition = ReadPosition(*Editor, "AnyStatePosition");
	}

	if (const auto Found = Root.find("Parameters"); Found != Root.end() && Found->is_array())
	{
		for (const json& Node : *Found)
		{
			FAnimGraphParameter Parameter;
			Parameter.Name    = Node.value("Name", std::string());
			Parameter.Type    = Node.value("Type", std::string("Float")) == "Bool" ? EAnimParamType::Bool : EAnimParamType::Float;
			Parameter.Default = Node.contains("Default") ? ReadNumberOrBool(Node["Default"], 0.0f) : 0.0f;
			if (Parameter.Name.empty())
			{
				Warn("이름 없는 파라미터를 건너뜁니다");
				continue;
			}
			Asset.Parameters.push_back(std::move(Parameter));
		}
	}

	std::string Error;
	if (!ParseMachine(Root, Asset, std::string(), Asset, Error, Warn))
	{
		return Fail(std::move(Error));
	}

	if (const auto Found = Root.find("Layers"); Found != Root.end() && Found->is_array())
	{
		for (const json& Node : *Found)
		{
			FAnimGraphLayer Layer;
			Layer.Name            = Node.value("Name", std::string());
			Layer.Weight          = Node.value("Weight", 1.0f);
			Layer.WeightParameter = Node.value("WeightParameter", std::string());
			if (const auto Mask = Node.find("Mask"); Mask != Node.end())
			{
				Layer.Mask = ParseMask(*Mask);
			}
			if (const auto Editor = Node.find("Editor"); Editor != Node.end() && Editor->is_object())
			{
				Layer.AnyStateEditorPosition = ReadPosition(*Editor, "AnyStatePosition");
			}
			if (Layer.Name.empty() || Asset.FindLayer(Layer.Name) >= 0)
			{
				return Fail(std::format("레이어 {}번: 이름이 비었거나 겹칩니다 ('{}')", Asset.Layers.size(), Layer.Name));
			}
			if (!ParseMachine(Node, Asset, std::format("레이어 {}: ", Layer.Name), Layer, Error, Warn))
			{
				return Fail(std::move(Error));
			}
			if (!Layer.WeightParameter.empty() && Asset.FindParameter(Layer.WeightParameter) == nullptr)
			{
				Warn(std::format("레이어 {}: 선언되지 않은 가중치 파라미터 '{}' (기본값 0)", Layer.Name, Layer.WeightParameter));
			}
			Asset.Layers.push_back(std::move(Layer));
		}
	}
	if (const auto Found = Root.find("Slots"); Found != Root.end() && Found->is_array())
	{
		for (const json& Node : *Found)
		{
			FAnimGraphSlot Slot;
			Slot.Name = Node.value("Name", std::string());
			if (const auto Mask = Node.find("Mask"); Mask != Node.end())
			{
				Slot.Mask = ParseMask(*Mask);
			}
			if (Slot.Name.empty() || Asset.FindSlot(Slot.Name) >= 0)
			{
				Warn(std::format("몽타주 슬롯 {}번: 이름이 비었거나 겹쳐 건너뜁니다 ('{}')", Asset.Slots.size(), Slot.Name));
				continue;
			}
			Asset.Slots.push_back(std::move(Slot));
		}
	}
	Out = std::move(Asset);
	return true;
}

std::string FAnimGraphAsset::ToJsonString() const
{
	json Root       = json::object();
	Root["Version"] = Version;

	json ParameterArray = json::array();
	for (const FAnimGraphParameter& Parameter : Parameters)
	{
		json Node    = json::object();
		Node["Name"] = Parameter.Name;
		Node["Type"] = Parameter.Type == EAnimParamType::Bool ? "Bool" : "Float";
		if (Parameter.Type == EAnimParamType::Bool)
		{
			Node["Default"] = Parameter.Default != 0.0f;
		}
		else
		{
			Node["Default"] = Parameter.Default;
		}
		ParameterArray.push_back(std::move(Node));
	}
	Root["Parameters"] = std::move(ParameterArray);
	WriteMachine(*this, *this, Root);

	if (!Layers.empty())
	{
		json LayerArray = json::array();
		for (const FAnimGraphLayer& Layer : Layers)
		{
			json Node    = json::object();
			Node["Name"] = Layer.Name;
			if (Layer.Weight != 1.0f)
			{
				Node["Weight"] = Layer.Weight;
			}
			if (!Layer.WeightParameter.empty())
			{
				Node["WeightParameter"] = Layer.WeightParameter;
			}
			Node["Mask"] = WriteMask(Layer.Mask);
			WriteMachine(Layer, *this, Node);
			if (Layer.AnyStateEditorPosition)
			{
				Node["Editor"] = json{ { "AnyStatePosition", WritePosition(*Layer.AnyStateEditorPosition) } };
			}
			LayerArray.push_back(std::move(Node));
		}
		Root["Layers"] = std::move(LayerArray);
	}
	if (!Slots.empty())
	{
		json SlotArray = json::array();
		for (const FAnimGraphSlot& Slot : Slots)
		{
			SlotArray.push_back(json{ { "Name", Slot.Name }, { "Mask", WriteMask(Slot.Mask) } });
		}
		Root["Slots"] = std::move(SlotArray);
	}

	json Editor = json::object();
	if (!PreviewModel.empty())
	{
		Editor["PreviewModel"] = PreviewModel;
	}
	if (AnyStateEditorPosition)
	{
		Editor["AnyStatePosition"] = WritePosition(*AnyStateEditorPosition);
	}
	if (!Editor.empty())
	{
		Root["Editor"] = std::move(Editor);
	}
	return Root.dump(2) + "\n";
}

bool FAnimGraphAsset::SaveToFile(const std::filesystem::path& Path) const
{
	std::ofstream File(Path, std::ios::binary | std::ios::trunc);
	if (!File)
	{
		return false;
	}
	const std::string Text = ToJsonString();
	File.write(Text.data(), static_cast<std::streamsize>(Text.size()));
	return File.good();
}

FAnimGraphAsset FAnimGraphAsset::MakeDefault()
{
	FAnimGraphAsset Asset;
	Asset.Parameters.push_back({ "Speed", EAnimParamType::Float, 0.0f });
	FAnimGraphState Idle;
	Idle.Name           = "Idle";
	Idle.Samples        = { FAnimBlendSample{} };
	Idle.EditorPosition = FVector2(260.0f, 0.0f);
	Asset.States.push_back(std::move(Idle));
	Asset.AnyStateEditorPosition = FVector2(0.0f, 0.0f);
	return Asset;
}

// ---------------------------------------------------------------- 파라미터 / 바인딩

void FAnimParameterSet::Set(std::string_view Name, float Value)
{
	for (auto& [Key, Stored] : Values)
	{
		if (Key == Name)
		{
			Stored = Value;
			return;
		}
	}
	Values.emplace_back(std::string(Name), Value);
}

bool FAnimParameterSet::TryGet(std::string_view Name, float& OutValue) const
{
	for (const auto& [Key, Stored] : Values)
	{
		if (Key == Name)
		{
			OutValue = Stored;
			return true;
		}
	}
	return false;
}

float FAnimParameterSet::Get(std::string_view Name, const FAnimGraphAsset& Asset) const
{
	float Value = 0.0f;
	if (TryGet(Name, Value))
	{
		return Value;
	}
	const FAnimGraphParameter* Parameter = Asset.FindParameter(Name);
	return Parameter != nullptr ? Parameter->Default : 0.0f;
}

FAnimGraphBinding FAnimGraphBinding::BindMachine(const FAnimStateMachine& Machine, const FAnimationSet& Set, std::vector<std::string>* OutMissing)
{
	FAnimGraphBinding Binding;
	Binding.ClipDurations.reserve(Set.Clips.size());
	for (const FAnimationClip& Clip : Set.Clips)
	{
		Binding.ClipDurations.push_back(Clip.Duration);
	}
	Binding.SampleClips.resize(Machine.States.size());
	for (size_t State = 0; State < Machine.States.size(); ++State)
	{
		for (const FAnimBlendSample& Sample : Machine.States[State].Samples)
		{
			const int32 Clip = Set.FindClip(Sample.Clip);
			Binding.SampleClips[State].push_back(Clip);
			if (Clip < 0 && OutMissing != nullptr)
			{
				OutMissing->push_back(Sample.Clip);
			}
		}
	}
	return Binding;
}

FAnimGraphBinding FAnimGraphBinding::Bind(const FAnimGraphAsset& Asset, const FAnimationSet& Set, std::vector<std::string>* OutMissing)
{
	FAnimGraphBinding Binding = BindMachine(Asset, Set, OutMissing);
	for (const FAnimGraphLayer& Layer : Asset.Layers)
	{
		Binding.Layers.push_back(BindMachine(Layer, Set, OutMissing));
	}
	return Binding;
}

void FAnimGraphRuntime::SetAsset(std::shared_ptr<const FAnimGraphAsset> NewAsset, bool bKeepState)
{
	if (NewAsset == Asset)
	{
		return;
	}
	ResumeState.clear();
	LayerResumeStates.clear();
	const int32 Current = Instance.GetCurrentState();
	if (bKeepState && Asset && Current >= 0 && Current < static_cast<int32>(Asset->States.size()))
	{
		ResumeState = Asset->States[static_cast<size_t>(Current)].Name;
	}
	if (bKeepState && Asset)
	{
		for (size_t Layer = 0; Layer < Asset->Layers.size() && Layer < LayerInstances.size(); ++Layer)
		{
			const int32 State = LayerInstances[Layer].GetCurrentState();
			if (State >= 0 && State < static_cast<int32>(Asset->Layers[Layer].States.size()))
			{
				LayerResumeStates.emplace_back(Asset->Layers[Layer].Name, Asset->Layers[Layer].States[static_cast<size_t>(State)].Name);
			}
		}
	}
	Asset    = std::move(NewAsset);
	BoundSet = nullptr; // 다음 갱신에서 Rebind
}

void FAnimGraphRuntime::Rebind(const FAnimationSet& Set, const std::vector<std::string>& NodeNames, std::vector<std::string>* OutMissing,
                               std::vector<std::string>* OutMissingBones)
{
	Binding  = FAnimGraphBinding::Bind(*Asset, Set, OutMissing);
	BoundSet = &Set;
	BaseNotify.Reset();
	Instance.ResetToState(ResumeState.empty() ? -1 : Asset->FindState(ResumeState));
	ResumeState.clear();

	const size_t LayerCount = Asset->Layers.size();
	LayerInstances.assign(LayerCount, FAnimGraphInstance());
	LayerNotify.assign(LayerCount, FAnimNotifyTrack());
	LayerMasks.assign(LayerCount, std::vector<float>());
	LayerWeights.assign(LayerCount, 0.0f);
	for (size_t Layer = 0; Layer < LayerCount; ++Layer)
	{
		const FAnimGraphLayer& Data = Asset->Layers[Layer];
		AnimGraphMath::ComputeBoneMaskWeights(Data.Mask, Set.NodeParents, NodeNames, LayerMasks[Layer], OutMissingBones);
		for (const auto& [LayerName, StateName] : LayerResumeStates)
		{
			if (LayerName == Data.Name)
			{
				LayerInstances[Layer].ResetToState(Data.FindState(StateName));
			}
		}
	}
	LayerResumeStates.clear();

	SlotMasks.assign(Asset->Slots.size(), std::vector<float>());
	for (size_t Slot = 0; Slot < Asset->Slots.size(); ++Slot)
	{
		AnimGraphMath::ComputeBoneMaskWeights(Asset->Slots[Slot].Mask, Set.NodeParents, NodeNames, SlotMasks[Slot], OutMissingBones);
	}
}

// ---------------------------------------------------------------- 순수 계산

namespace AnimGraphMath
{
	void ComputeBlendSpace1DWeights(const std::vector<float>& Positions, float Value, std::vector<float>& OutWeights)
	{
		const size_t Count = Positions.size();
		OutWeights.assign(Count, 0.0f);
		if (Count == 0)
		{
			return;
		}
		if (Count == 1 || Value <= Positions.front())
		{
			OutWeights.front() = 1.0f;
			return;
		}
		if (Value >= Positions.back())
		{
			OutWeights.back() = 1.0f;
			return;
		}
		// Positions[Next - 1] <= Value < Positions[Next]
		const size_t Next  = static_cast<size_t>(std::upper_bound(Positions.begin(), Positions.end(), Value) - Positions.begin());
		const size_t Prev  = Next - 1;
		const float  Span  = Positions[Next] - Positions[Prev];
		const float  Alpha = Span > FMath::SmallNumber ? (Value - Positions[Prev]) / Span : 1.0f;
		OutWeights[Prev]   = 1.0f - Alpha;
		OutWeights[Next]   = Alpha;
	}

	void ComputeBlendSpace2DWeights(const std::vector<FVector2>& Positions, const FVector2& Value, std::vector<float>& OutWeights)
	{
		const size_t Count = Positions.size();
		OutWeights.assign(Count, 0.0f);
		if (Count == 0)
		{
			return;
		}
		if (Count == 1)
		{
			OutWeights.front() = 1.0f;
			return;
		}
		// 축 단위가 달라도(속도 cm/s vs 방향 도) 같은 비중이 되게 샘플 범위로 정규화
		FVector2 Min = Positions.front();
		FVector2 Max = Min;
		for (const FVector2& Position : Positions)
		{
			Min = FVector2(FMath::Min(Min.X, Position.X), FMath::Min(Min.Y, Position.Y));
			Max = FVector2(FMath::Max(Max.X, Position.X), FMath::Max(Max.Y, Position.Y));
		}
		const float ScaleX    = Max.X - Min.X > FMath::SmallNumber ? 1.0f / (Max.X - Min.X) : 1.0f;
		const float ScaleY    = Max.Y - Min.Y > FMath::SmallNumber ? 1.0f / (Max.Y - Min.Y) : 1.0f;
		const auto  Normalize = [&](const FVector2& Point) { return FVector2((Point.X - Min.X) * ScaleX, (Point.Y - Min.Y) * ScaleY); };

		const FVector2 Target = Normalize(Value);
		float          Total  = 0.0f;
		for (size_t I = 0; I < Count; ++I)
		{
			const FVector2 Pi       = Normalize(Positions[I]);
			const FVector2 ToTarget = Target - Pi;
			float          Weight   = 1.0f;
			for (size_t J = 0; J < Count && Weight > 0.0f; ++J)
			{
				if (J == I)
				{
					continue;
				}
				const FVector2 Edge    = Normalize(Positions[J]) - Pi;
				const float    Length2 = FVector2::Dot(Edge, Edge);
				if (Length2 < 1.0e-8f)
				{
					continue; // 같은 위치의 샘플: 서로 영향 없음 (둘이 나눠 갖는다)
				}
				Weight = FMath::Min(Weight, 1.0f - FVector2::Dot(ToTarget, Edge) / Length2);
			}
			OutWeights[I] = FMath::Max(Weight, 0.0f);
			Total += OutWeights[I];
		}
		if (Total <= FMath::SmallNumber)
		{
			size_t Nearest  = 0;
			float  Distance = FLT_MAX;
			for (size_t I = 0; I < Count; ++I)
			{
				const FVector2 Delta = Normalize(Positions[I]) - Target;
				if (const float D = FVector2::Dot(Delta, Delta); D < Distance)
				{
					Distance = D;
					Nearest  = I;
				}
			}
			OutWeights.assign(Count, 0.0f);
			OutWeights[Nearest] = 1.0f;
			return;
		}
		for (float& Weight : OutWeights)
		{
			Weight /= Total;
		}
	}

	bool EvaluateCondition(EAnimConditionOp Op, float Parameter, float Value)
	{
		constexpr float EqualTolerance = 1.0e-4f;
		switch (Op)
		{
		case EAnimConditionOp::Less:         return Parameter < Value;
		case EAnimConditionOp::LessEqual:    return Parameter <= Value;
		case EAnimConditionOp::Greater:      return Parameter > Value;
		case EAnimConditionOp::GreaterEqual: return Parameter >= Value;
		case EAnimConditionOp::Equal:        return std::fabs(Parameter - Value) <= EqualTolerance;
		case EAnimConditionOp::NotEqual:     return std::fabs(Parameter - Value) > EqualTolerance;
		}
		return false;
	}

	int32 FindTransition(const FAnimGraphAsset& Asset, int32 CurrentState, float CurrentPhase, const FAnimParameterSet& Parameters)
	{
		return FindTransition(Asset, Asset, CurrentState, CurrentPhase, Parameters);
	}

	int32 FindTransition(const FAnimGraphAsset& Asset, const FAnimStateMachine& Machine, int32 CurrentState, float CurrentPhase,
	                     const FAnimParameterSet& Parameters)
	{
		for (size_t Index = 0; Index < Machine.Transitions.size(); ++Index)
		{
			const FAnimGraphTransition& Transition = Machine.Transitions[Index];
			if (Transition.To == CurrentState || (Transition.From >= 0 && Transition.From != CurrentState))
			{
				continue;
			}
			if (Transition.ExitTime >= 0.0f && CurrentPhase < Transition.ExitTime)
			{
				continue;
			}
			const bool bPass = std::all_of(Transition.Conditions.begin(), Transition.Conditions.end(), [&](const FAnimTransitionCondition& Condition) {
				return EvaluateCondition(Condition.Op, Parameters.Get(Condition.Parameter, Asset), Condition.Value);
			});
			if (bPass)
			{
				return static_cast<int32>(Index);
			}
		}
		return -1;
	}

	void AddWeightedPose(std::vector<FNodePose>& Accumulator, const std::vector<FNodePose>& Pose, float Weight, bool bFirst)
	{
		if (bFirst)
		{
			Accumulator.resize(Pose.size());
		}
		const size_t Count = std::min(Accumulator.size(), Pose.size());
		for (size_t Index = 0; Index < Count; ++Index)
		{
			FNodePose&       Sum    = Accumulator[Index];
			const FNodePose& Source = Pose[Index];
			if (bFirst)
			{
				Sum.Translation = Source.Translation * Weight;
				Sum.Rotation    = FQuat(Source.Rotation.X * Weight, Source.Rotation.Y * Weight, Source.Rotation.Z * Weight, Source.Rotation.W * Weight);
				Sum.Scale       = Source.Scale * Weight;
				continue;
			}
			Sum.Translation = Sum.Translation + Source.Translation * Weight;
			Sum.Scale       = Sum.Scale + Source.Scale * Weight;
			// q와 -q는 같은 회전: 누적값과 같은 반구로 맞춰 더한다 (nlerp)
			const float Signed = FQuat::Dot(Sum.Rotation, Source.Rotation) < 0.0f ? -Weight : Weight;
			Sum.Rotation = FQuat(Sum.Rotation.X + Source.Rotation.X * Signed, Sum.Rotation.Y + Source.Rotation.Y * Signed,
			                     Sum.Rotation.Z + Source.Rotation.Z * Signed, Sum.Rotation.W + Source.Rotation.W * Signed);
		}
	}

	void FinishWeightedPose(std::vector<FNodePose>& Accumulator)
	{
		for (FNodePose& Pose : Accumulator)
		{
			Pose.Rotation = Pose.Rotation.GetNormalized();
		}
	}

	void ComputeBoneMaskWeights(const FAnimBoneMask& Mask, const std::vector<int32>& Parents, const std::vector<std::string>& NodeNames,
	                            std::vector<float>& OutWeights, std::vector<std::string>* OutMissing)
	{
		const size_t Count = Parents.size();
		if (Mask.IsFullBody())
		{
			OutWeights.assign(Count, 1.0f);
			return;
		}
		OutWeights.assign(Count, 0.0f);
		// 노드 → 마스크 항목 (같은 뼈가 여러 번 있으면 마지막 항목)
		std::vector<int32> EntryOfNode(Count, -1);
		for (size_t Entry = 0; Entry < Mask.Bones.size(); ++Entry)
		{
			bool bFound = false;
			for (size_t Node = 0; Node < Count && Node < NodeNames.size(); ++Node)
			{
				if (NodeNames[Node] == Mask.Bones[Entry].Bone)
				{
					EntryOfNode[Node] = static_cast<int32>(Entry);
					bFound            = true;
				}
			}
			if (!bFound && OutMissing != nullptr)
			{
				OutMissing->push_back(Mask.Bones[Entry].Bone);
			}
		}
		for (size_t Node = 0; Node < Count; ++Node)
		{
			// 자신부터 위로 올라가며 처음 만나는 항목 (Depth = 그 뼈에서 몇 세대 아래인지)
			int32 Depth = 0;
			for (int32 Current = static_cast<int32>(Node); Current >= 0 && Current < static_cast<int32>(Count) && Depth <= static_cast<int32>(Count);
			     Current = Parents[static_cast<size_t>(Current)], ++Depth)
			{
				const int32 Entry = EntryOfNode[static_cast<size_t>(Current)];
				if (Entry < 0)
				{
					continue;
				}
				const FAnimBoneMaskEntry& Data = Mask.Bones[static_cast<size_t>(Entry)];
				const float Ramp = Data.BlendDepth > 0 ? FMath::Min(1.0f, static_cast<float>(Depth + 1) / static_cast<float>(Data.BlendDepth)) : 1.0f;
				OutWeights[Node] = FMath::Clamp(Data.Weight * Ramp, 0.0f, 1.0f);
				break;
			}
		}
	}

	float ComputeLayerWeight(const FAnimGraphAsset& Asset, const FAnimGraphLayer& Layer, const FAnimParameterSet& Parameters)
	{
		const float Parameter = Layer.WeightParameter.empty() ? 1.0f : Parameters.Get(Layer.WeightParameter, Asset);
		return FMath::Clamp(Layer.Weight * Parameter, 0.0f, 1.0f);
	}

	void BlendMasked(std::vector<FNodePose>& Base, const std::vector<FNodePose>& Overlay, const std::vector<float>& Weights, float Alpha)
	{
		const size_t Count = std::min(Base.size(), Overlay.size());
		for (size_t Node = 0; Node < Count; ++Node)
		{
			const float Weight = (Weights.empty() ? 1.0f : (Node < Weights.size() ? Weights[Node] : 0.0f)) * Alpha;
			if (Weight <= 0.0f)
			{
				continue;
			}
			Base[Node] = Weight >= 1.0f ? Overlay[Node] : AnimationMath::BlendPose(Base[Node], Overlay[Node], Weight);
		}
	}
} // namespace AnimGraphMath

// ---------------------------------------------------------------- 상태 머신 실행

void FAnimGraphInstance::Reset()
{
	Layers.clear();
	BlendElapsed  = 0.0f;
	BlendDuration = 0.0f;
	Contributions.clear();
	NotifySource    = {};
	StartState      = -1;
	LastTransition  = -1;
	TransitionCount = 0;
}

void FAnimGraphInstance::ResetToState(int32 State)
{
	Reset();
	StartState = State;
}

void FAnimGraphInstance::ComputeSampleWeights(const FAnimGraphAsset& Asset, const FAnimStateMachine& Machine, const FAnimGraphBinding& Binding,
                                              const FAnimParameterSet& Parameters, FLayer& Layer)
{
	const FAnimGraphState&    State = Machine.States[Layer.State];
	const std::vector<int32>& Clips = Binding.SampleClips[Layer.State];
	Layer.SampleWeights.assign(State.Samples.size(), 0.0f);

	// 모델에 있는 샘플만으로 블렌드 스페이스 가중치
	const float Value = State.BlendParameter.empty() ? 0.0f : Parameters.Get(State.BlendParameter, Asset);
	if (State.Is2D())
	{
		Position2DScratch.clear();
		for (size_t Index = 0; Index < State.Samples.size(); ++Index)
		{
			if (Clips[Index] >= 0)
			{
				Position2DScratch.emplace_back(State.Samples[Index].Position, State.Samples[Index].PositionY);
			}
		}
		AnimGraphMath::ComputeBlendSpace2DWeights(Position2DScratch, FVector2(Value, Parameters.Get(State.BlendParameterY, Asset)), WeightScratch);
	}
	else
	{
		PositionScratch.clear();
		for (size_t Index = 0; Index < State.Samples.size(); ++Index)
		{
			if (Clips[Index] >= 0)
			{
				PositionScratch.push_back(State.Samples[Index].Position);
			}
		}
		AnimGraphMath::ComputeBlendSpace1DWeights(PositionScratch, Value, WeightScratch);
	}
	size_t Valid = 0;
	for (size_t Index = 0; Index < State.Samples.size(); ++Index)
	{
		if (Clips[Index] >= 0)
		{
			Layer.SampleWeights[Index] = WeightScratch[Valid++];
		}
	}
}

void FAnimGraphInstance::StartTransition(const FAnimGraphTransition& Transition)
{
	// 지금 가중치를 출발점으로 (페이드 중이었으면 섞인 그대로에서 다시 섞는다)
	for (FLayer& Layer : Layers)
	{
		Layer.StartWeight = Layer.Weight;
	}
	const auto Existing = std::find_if(Layers.begin(), Layers.end(), [&](const FLayer& Layer) { return Layer.State == Transition.To; });
	if (Existing != Layers.end())
	{
		// 아직 섞이고 있는 상태로 돌아감: 재생 위치와 가중치를 이어 간다
		FLayer Resumed = std::move(*Existing);
		Layers.erase(Existing);
		Layers.push_back(std::move(Resumed));
	}
	else
	{
		FLayer Layer;
		Layer.State  = Transition.To;
		Layer.Serial = NextSerial++;
		Layers.push_back(std::move(Layer));
	}
	FLayer& Current       = Layers.back();
	Current.Elapsed       = 0.0f;
	Current.PreviousPhase = Current.Phase;
	Current.PhaseDelta    = 0.0f; // 들어간 프레임은 진행하지 않는다
	Current.bWrapped      = false;
	BlendElapsed          = 0.0f;
	BlendDuration         = Transition.Duration;

	while (Layers.size() > MaxLayers)
	{
		Layers.erase(Layers.begin());
	}
	// 버린 레이어가 있어도 합이 1이 되게
	float Total = 0.0f;
	for (const FLayer& Layer : Layers)
	{
		Total += Layer.StartWeight;
	}
	const float OldTotal = Total - Current.StartWeight;
	if (OldTotal > FMath::SmallNumber && Total > 0.0f)
	{
		const float Scale = (1.0f - Current.StartWeight) / OldTotal;
		for (size_t Index = 0; Index + 1 < Layers.size(); ++Index)
		{
			Layers[Index].StartWeight *= Scale;
		}
	}
}

void FAnimGraphInstance::UpdateLayerWeights()
{
	const float Alpha = AnimationMath::ComputeCrossfadeWeight(BlendElapsed, BlendDuration);
	FLayer&     Current = Layers.back();
	Current.Weight      = Current.StartWeight + (1.0f - Current.StartWeight) * Alpha;
	for (size_t Index = 0; Index + 1 < Layers.size(); ++Index)
	{
		Layers[Index].Weight = Layers[Index].StartWeight * (1.0f - Alpha);
	}
	if (Alpha >= 1.0f || Layers.size() == 1)
	{
		Layers.erase(Layers.begin(), Layers.end() - 1);
		Layers.back().Weight      = 1.0f;
		Layers.back().StartWeight = 1.0f;
	}
}

void FAnimGraphInstance::Update(const FAnimGraphAsset& Asset, const FAnimStateMachine& Machine, const FAnimGraphBinding& Binding,
                                const FAnimParameterSet& Parameters, float DeltaSeconds)
{
	Contributions.clear();
	NotifySource = {};
	if (Machine.States.empty() || Binding.SampleClips.size() != Machine.States.size())
	{
		return;
	}
	if (Layers.empty())
	{
		FLayer Entry;
		const int32 First = StartState >= 0 && StartState < static_cast<int32>(Machine.States.size()) ? StartState : Machine.EntryState;
		Entry.State       = FMath::Clamp(First, 0, static_cast<int32>(Machine.States.size()) - 1);
		StartState        = -1;
		Entry.Serial      = NextSerial++;
		Entry.Weight      = 1.0f;
		Entry.StartWeight = 1.0f;
		Layers.push_back(std::move(Entry));
		DeltaSeconds = 0.0f; // 시작 프레임은 처음 포즈
	}

	// 1) 모든 레이어 진행 (페이드아웃 중인 상태도 계속 재생)
	for (FLayer& Layer : Layers)
	{
		ComputeSampleWeights(Asset, Machine, Binding, Parameters, Layer);
		const FAnimGraphState&    State = Machine.States[Layer.State];
		const std::vector<int32>& Clips = Binding.SampleClips[Layer.State];
		float                     Cycle = 0.0f;
		for (size_t Index = 0; Index < State.Samples.size(); ++Index)
		{
			if (Clips[Index] >= 0 && Layer.SampleWeights[Index] > 0.0f)
			{
				const float Rate = std::fabs(State.Samples[Index].Rate) > FMath::SmallNumber ? std::fabs(State.Samples[Index].Rate) : 1.0f;
				Cycle += Layer.SampleWeights[Index] * Binding.ClipDurations[Clips[Index]] / Rate;
			}
		}
		Layer.PreviousPhase = Layer.Phase;
		Layer.PhaseDelta    = Cycle > FMath::SmallNumber ? DeltaSeconds * State.Speed / Cycle : 0.0f;
		const float Before  = Layer.Phase;
		Layer.Phase         = AdvancePhase(Layer.Phase, Layer.PhaseDelta, State.bLoop, Layer.bWrapped);
		if (!State.bLoop)
		{
			Layer.PhaseDelta = Layer.Phase - Before; // 끝에서 멈춘 만큼만
		}
		Layer.Elapsed += DeltaSeconds;
	}
	BlendElapsed += DeltaSeconds;
	UpdateLayerWeights();

	// 2) 전이 (프레임당 하나)
	if (const int32 Transition = AnimGraphMath::FindTransition(Asset, Machine, Layers.back().State, Layers.back().Phase, Parameters); Transition >= 0)
	{
		StartTransition(Machine.Transitions[Transition]);
		LastTransition = Transition;
		++TransitionCount;
		ComputeSampleWeights(Asset, Machine, Binding, Parameters, Layers.back());
		UpdateLayerWeights();
	}

	// 3) 기여 = 레이어 가중치 × 샘플 가중치
	float  Total     = 0.0f;
	float  BestWeight = -1.0f;
	for (const FLayer& Layer : Layers)
	{
		const FAnimGraphState&    State = Machine.States[Layer.State];
		const std::vector<int32>& Clips = Binding.SampleClips[Layer.State];
		for (size_t Index = 0; Index < State.Samples.size(); ++Index)
		{
			const float Weight = Layer.Weight * Layer.SampleWeights[Index];
			if (Clips[Index] < 0 || Weight <= 0.0f)
			{
				continue;
			}
			const float Duration = Binding.ClipDurations[Clips[Index]];
			Contributions.push_back({ Clips[Index], Layer.Phase * Duration, Weight, Layer.PreviousPhase * Duration, Layer.PhaseDelta * Duration, Layer.bWrapped });
			Total += Weight;
			if (Weight > BestWeight)
			{
				BestWeight                = Weight;
				NotifySource.Key          = Layer.Serial * 64u + static_cast<uint32>(Index % 64);
				NotifySource.Clip         = Clips[Index];
				NotifySource.PreviousTime = Layer.PreviousPhase * Duration;
				NotifySource.NewTime      = Layer.Phase * Duration;
				NotifySource.Delta        = Layer.PhaseDelta * Duration;
				NotifySource.Duration     = Duration;
				NotifySource.bLoop        = State.bLoop;
				NotifySource.bWrapped     = Layer.bWrapped;
			}
		}
	}
	if (Total > FMath::SmallNumber)
	{
		for (FAnimClipContribution& Contribution : Contributions)
		{
			Contribution.Weight /= Total;
		}
	}
}

// ---------------------------------------------------------------- 라이브러리

FAnimGraphLibrary& FAnimGraphLibrary::Get()
{
	static FAnimGraphLibrary Instance;
	return Instance;
}

std::wstring FAnimGraphLibrary::MakeKey(const std::string& AssetPath)
{
	std::wstring Key = FPrefabLibrary::Get().ResolveAssetPath(AssetPath).lexically_normal().generic_wstring();
	std::transform(Key.begin(), Key.end(), Key.begin(), [](wchar_t Char) { return static_cast<wchar_t>(std::towlower(Char)); });
	return Key;
}

void FAnimGraphLibrary::Invalidate()
{
	Cache.clear();
	++Generation;
}

void FAnimGraphLibrary::Invalidate(const std::string& AssetPath)
{
	Cache.erase(MakeKey(AssetPath));
	++Generation;
}

std::shared_ptr<const FAnimGraphAsset> FAnimGraphLibrary::Load(const std::string& AssetPath)
{
	const std::filesystem::path Path = FPrefabLibrary::Get().ResolveAssetPath(AssetPath);
	const std::wstring          Key  = MakeKey(AssetPath);
	if (const auto Found = Cache.find(Key); Found != Cache.end())
	{
		return Found->second;
	}

	std::shared_ptr<const FAnimGraphAsset> Result;
	std::string                            Text;
	if (!FFileSystem::ReadTextFile(Path, Text))
	{
		E_LOG(LogScene, Warning, "애니메이션 그래프를 읽을 수 없습니다: {}", AssetPath);
	}
	else
	{
		auto                     Asset = std::make_shared<FAnimGraphAsset>();
		std::string              Error;
		std::vector<std::string> Warnings;
		if (FAnimGraphAsset::FromJsonString(Text, *Asset, &Error, &Warnings))
		{
			Result = std::move(Asset);
		}
		else
		{
			E_LOG(LogScene, Warning, "애니메이션 그래프 형식 오류 ({}): {}", AssetPath, Error);
		}
		for (const std::string& Warning : Warnings)
		{
			E_LOG(LogScene, Warning, "애니메이션 그래프 {}: {}", AssetPath, Warning);
		}
	}
	Cache[Key] = Result;
	return Result;
}
