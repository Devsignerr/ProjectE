#include "Scene/AnimGraph.h"

#include "Core/FileSystem.h"
#include "Core/Log.h"
#include "Core/StringConv.h"
#include "Scene/Prefab.h"

#include <json.hpp>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <format>

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
} // namespace

// ---------------------------------------------------------------- 에셋

int32 FAnimGraphAsset::FindState(std::string_view Name) const
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

bool FAnimGraphAsset::FromJsonString(const std::string& Text, FAnimGraphAsset& Out, std::string* OutError, std::vector<std::string>* OutWarnings)
{
	const auto Fail = [OutError](std::string Message) {
		if (OutError != nullptr)
		{
			*OutError = std::move(Message);
		}
		return false;
	};
	const auto Warn = [OutWarnings](std::string Message) {
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

	const auto States = Root.find("States");
	if (States == Root.end() || !States->is_array() || States->empty())
	{
		return Fail("상태(States)가 없습니다");
	}
	for (const json& Node : *States)
	{
		FAnimGraphState State;
		State.Name           = Node.value("Name", std::string());
		State.BlendParameter = Node.value("BlendParameter", std::string());
		State.Speed          = Node.value("Speed", 1.0f);
		State.bLoop          = Node.value("Loop", true);
		if (const auto Samples = Node.find("Samples"); Samples != Node.end() && Samples->is_array())
		{
			for (const json& SampleNode : *Samples)
			{
				FAnimBlendSample Sample;
				Sample.Clip     = SampleNode.value("Clip", std::string());
				Sample.Position = SampleNode.value("Position", 0.0f);
				Sample.Rate     = SampleNode.value("Rate", 1.0f);
				State.Samples.push_back(std::move(Sample));
			}
		}
		else if (Node.contains("Clip"))
		{
			FAnimBlendSample Sample;
			Sample.Clip = Node.value("Clip", std::string());
			Sample.Rate = Node.value("Rate", 1.0f);
			State.Samples.push_back(std::move(Sample));
		}
		if (State.Name.empty() || State.Samples.empty())
		{
			return Fail(std::format("상태 {}번: 이름과 클립(Clip 또는 Samples)이 필요합니다", Asset.States.size()));
		}
		if (Asset.FindState(State.Name) >= 0)
		{
			return Fail(std::format("상태 이름이 겹칩니다: {}", State.Name));
		}
		std::stable_sort(State.Samples.begin(), State.Samples.end(), [](const FAnimBlendSample& A, const FAnimBlendSample& B) { return A.Position < B.Position; });
		Asset.States.push_back(std::move(State));
	}

	const std::string Entry = Root.value("EntryState", std::string());
	Asset.EntryState        = Entry.empty() ? 0 : Asset.FindState(Entry);
	if (Asset.EntryState < 0)
	{
		return Fail(std::format("시작 상태를 찾을 수 없습니다: {}", Entry));
	}

	if (const auto Found = Root.find("Transitions"); Found != Root.end() && Found->is_array())
	{
		for (const json& Node : *Found)
		{
			FAnimGraphTransition Transition;
			const std::string    From = Node.value("From", std::string("*"));
			const std::string    To   = Node.value("To", std::string());
			Transition.From           = From == "*" ? -1 : Asset.FindState(From);
			Transition.To             = Asset.FindState(To);
			Transition.Duration       = std::max(0.0f, Node.value("Duration", 0.2f));
			Transition.ExitTime       = Node.value("ExitTime", -1.0f);
			if ((From != "*" && Transition.From < 0) || Transition.To < 0)
			{
				Warn(std::format("전이 {} → {}: 상태를 찾을 수 없어 건너뜁니다", From, To));
				continue;
			}
			bool bValid = true;
			if (const auto Conditions = Node.find("Conditions"); Conditions != Node.end() && Conditions->is_array())
			{
				for (const json& ConditionNode : *Conditions)
				{
					FAnimTransitionCondition Condition;
					Condition.Parameter = ConditionNode.value("Parameter", std::string());
					Condition.Value     = ConditionNode.contains("Value") ? ReadNumberOrBool(ConditionNode["Value"], 0.0f) : 1.0f;
					if (Condition.Parameter.empty() || !ParseOp(ConditionNode.value("Op", std::string("==")), Condition.Op))
					{
						Warn(std::format("전이 {} → {}: 잘못된 조건 (Parameter/Op)", From, To));
						bValid = false;
						break;
					}
					if (Asset.FindParameter(Condition.Parameter) == nullptr)
					{
						Warn(std::format("전이 {} → {}: 선언되지 않은 파라미터 '{}' (기본값 0)", From, To, Condition.Parameter));
					}
					Transition.Conditions.push_back(std::move(Condition));
				}
			}
			if (bValid)
			{
				Asset.Transitions.push_back(std::move(Transition));
			}
		}
	}
	Out = std::move(Asset);
	return true;
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

FAnimGraphBinding FAnimGraphBinding::Bind(const FAnimGraphAsset& Asset, const FAnimationSet& Set, std::vector<std::string>* OutMissing)
{
	FAnimGraphBinding Binding;
	Binding.ClipDurations.reserve(Set.Clips.size());
	for (const FAnimationClip& Clip : Set.Clips)
	{
		Binding.ClipDurations.push_back(Clip.Duration);
	}
	Binding.SampleClips.resize(Asset.States.size());
	for (size_t State = 0; State < Asset.States.size(); ++State)
	{
		for (const FAnimBlendSample& Sample : Asset.States[State].Samples)
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
		for (size_t Index = 0; Index < Asset.Transitions.size(); ++Index)
		{
			const FAnimGraphTransition& Transition = Asset.Transitions[Index];
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
} // namespace AnimGraphMath

// ---------------------------------------------------------------- 상태 머신 실행

void FAnimGraphInstance::Reset()
{
	Layers.clear();
	BlendElapsed  = 0.0f;
	BlendDuration = 0.0f;
	Contributions.clear();
	NotifySource = {};
}

void FAnimGraphInstance::ComputeSampleWeights(const FAnimGraphAsset& Asset, const FAnimGraphBinding& Binding, const FAnimParameterSet& Parameters,
                                              FLayer& Layer)
{
	const FAnimGraphState&    State = Asset.States[Layer.State];
	const std::vector<int32>& Clips = Binding.SampleClips[Layer.State];
	Layer.SampleWeights.assign(State.Samples.size(), 0.0f);

	// 모델에 있는 샘플만으로 블렌드 스페이스 가중치
	PositionScratch.clear();
	for (size_t Index = 0; Index < State.Samples.size(); ++Index)
	{
		if (Clips[Index] >= 0)
		{
			PositionScratch.push_back(State.Samples[Index].Position);
		}
	}
	const float Value = State.BlendParameter.empty() ? 0.0f : Parameters.Get(State.BlendParameter, Asset);
	AnimGraphMath::ComputeBlendSpace1DWeights(PositionScratch, Value, WeightScratch);
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

void FAnimGraphInstance::Update(const FAnimGraphAsset& Asset, const FAnimGraphBinding& Binding, const FAnimParameterSet& Parameters, float DeltaSeconds)
{
	Contributions.clear();
	NotifySource = {};
	if (Asset.States.empty() || Binding.SampleClips.size() != Asset.States.size())
	{
		return;
	}
	if (Layers.empty())
	{
		FLayer Entry;
		Entry.State       = FMath::Clamp(Asset.EntryState, 0, static_cast<int32>(Asset.States.size()) - 1);
		Entry.Serial      = NextSerial++;
		Entry.Weight      = 1.0f;
		Entry.StartWeight = 1.0f;
		Layers.push_back(std::move(Entry));
		DeltaSeconds = 0.0f; // 시작 프레임은 처음 포즈
	}

	// 1) 모든 레이어 진행 (페이드아웃 중인 상태도 계속 재생)
	for (FLayer& Layer : Layers)
	{
		ComputeSampleWeights(Asset, Binding, Parameters, Layer);
		const FAnimGraphState&    State = Asset.States[Layer.State];
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
	if (const int32 Transition = AnimGraphMath::FindTransition(Asset, Layers.back().State, Layers.back().Phase, Parameters); Transition >= 0)
	{
		StartTransition(Asset.Transitions[Transition]);
		ComputeSampleWeights(Asset, Binding, Parameters, Layers.back());
		UpdateLayerWeights();
	}

	// 3) 기여 = 레이어 가중치 × 샘플 가중치
	float  Total     = 0.0f;
	float  BestWeight = -1.0f;
	for (const FLayer& Layer : Layers)
	{
		const FAnimGraphState&    State = Asset.States[Layer.State];
		const std::vector<int32>& Clips = Binding.SampleClips[Layer.State];
		for (size_t Index = 0; Index < State.Samples.size(); ++Index)
		{
			const float Weight = Layer.Weight * Layer.SampleWeights[Index];
			if (Clips[Index] < 0 || Weight <= 0.0f)
			{
				continue;
			}
			const float Duration = Binding.ClipDurations[Clips[Index]];
			Contributions.push_back({ Clips[Index], Layer.Phase * Duration, Weight });
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

std::shared_ptr<const FAnimGraphAsset> FAnimGraphLibrary::Load(const std::string& AssetPath)
{
	const std::filesystem::path Path = FPrefabLibrary::Get().ResolveAssetPath(AssetPath);
	std::wstring                Key  = Path.generic_wstring();
	std::transform(Key.begin(), Key.end(), Key.begin(), [](wchar_t Char) { return static_cast<wchar_t>(std::towlower(Char)); });
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
