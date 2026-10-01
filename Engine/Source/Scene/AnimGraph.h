#pragma once

#include "Core/CoreTypes.h"
#include "Core/Math/Math.h"
#include "Scene/Animation.h"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// 애니메이션 그래프 (언리얼 애니메이션 블루프린트의 상태 머신 + 1D 블렌드 스페이스를 데이터로).
//   에셋 .eanimgraph (JSON, Content 기준 경로) = 파라미터 목록 + 상태 목록 + 전이 목록. 모델 루트(FAnimationComponent가 있는 엔티티)에
//   FAnimGraphComponent(Graph = 에셋 경로)를 붙이면 FAnimationSystem이 FAnimationComponent의 Clip 대신 그래프로 포즈를 만든다
//   (FAnimationComponent의 Speed/Playing은 전체 배속/정지로 계속 쓰이고 Clip/Loop/BlendTime/RootMotion은 쓰지 않는다).
//
// 상태 = 클립 하나 또는 1D 블렌드 스페이스 (BlendParameter 값 → 위치가 이웃한 두 샘플을 선형 가중. 범위 밖은 끝 샘플).
//   재생 위치는 상태마다 정규화 시간(Phase, 0~1) 하나: 샘플들은 같은 Phase로 샘플링된다 (동기화 — 걷기/뛰기 발이 맞는다).
//   한 바퀴 길이 = Σ 가중치 × (클립 길이 / Rate), Phase += dt × 상태 Speed / 한 바퀴 길이. Loop가 아니면 1에서 멈춘다.
// 전이: 목록 순서대로 처음 맞는 하나만 (프레임당 최대 한 번). From = 상태 이름 또는 "*"(어느 상태든, To 자신 제외).
//   조건 = 파라미터 비교 (모두 참이어야 함), ExitTime >= 0이면 현재 상태 Phase가 그 이상일 때만.
//   크로스페이드: Duration 동안 smoothstep 가중치. 페이드 중 다른 전이가 오면 그 순간의 가중치들을 출발점으로 다시 섞는다
//   (튀지 않음, 이전 상태들도 계속 진행). 이미 섞이고 있는 상태로 돌아가면 그 상태의 재생 위치와 가중치를 이어 간다.
// 포즈 섞기: 기여(클립, 시각, 가중치) 전부의 가중 합 — 이동/스케일은 선형, 회전은 nlerp(첫 기여와 같은 반구로 맞춘 가중 합 → 정규화).
// 노티파이 (Scene/AnimNotify.h 규칙 위에): 이번 프레임 최종 가중치가 가장 큰 기여(상태 레이어 × 샘플) 하나의 클립에서만 판정한다.
//   그 기여가 바뀌면 이전 기여의 진행 중 스테이트는 End, 새 기여는 이번 진행 구간부터 판정(시작 시각이 스테이트 안이면 Begin).
//   방금 들어간 상태는 그 프레임에 진행하지 않으므로 노티파이가 없다.
// 파라미터: float/bool(0/1로 저장). Lua entity:SetAnimParam/GetAnimParam/GetAnimState, C++ FAnimationSystem::SetAnimParam 등.
//   파라미터는 복제되지 않는다 — 각 프로세스가 자기 값으로 계산한다 (캐릭터 이동 자동 공급은 FAnimGraphComponent::bUseCharacterMovement).

enum class EAnimParamType : uint8
{
	Float,
	Bool,
};

struct FAnimGraphParameter
{
	std::string    Name;
	EAnimParamType Type    = EAnimParamType::Float;
	float          Default = 0.0f;
};

struct FAnimBlendSample
{
	std::string Clip;
	float       Position = 0.0f; // 블렌드 파라미터 값
	float       Rate     = 1.0f; // 이 샘플의 재생 배속 (한 바퀴 길이 계산에 들어간다)
};

struct FAnimGraphState
{
	std::string                   Name;
	std::string                   BlendParameter; // 비면 단일 클립 (Samples[0])
	std::vector<FAnimBlendSample> Samples;        // Position 오름차순
	float                         Speed = 1.0f;
	bool                          bLoop = true;
};

enum class EAnimConditionOp : uint8
{
	Less,
	LessEqual,
	Greater,
	GreaterEqual,
	Equal,
	NotEqual,
};

struct FAnimTransitionCondition
{
	std::string      Parameter;
	EAnimConditionOp Op    = EAnimConditionOp::Equal;
	float            Value = 0.0f; // bool은 1/0
};

struct FAnimGraphTransition
{
	int32                                 From     = -1; // -1 = 어느 상태든
	int32                                 To       = 0;
	float                                 Duration = 0.2f;  // 크로스페이드 (초)
	float                                 ExitTime = -1.0f; // >= 0이면 From 상태 Phase가 이 값 이상일 때만
	std::vector<FAnimTransitionCondition> Conditions;
};

struct FAnimGraphAsset
{
	static constexpr int32 Version = 1;

	std::vector<FAnimGraphParameter>  Parameters;
	std::vector<FAnimGraphState>      States;
	std::vector<FAnimGraphTransition> Transitions;
	int32                             EntryState = 0;

	int32                      FindState(std::string_view Name) const;
	const FAnimGraphParameter* FindParameter(std::string_view Name) const;

	// 형식이 틀리면 false + 이유. 이름을 못 찾는 전이/조건은 건너뛰고 경고 목록(OutWarnings)에 남긴다
	static bool FromJsonString(const std::string& Text, FAnimGraphAsset& Out, std::string* OutError = nullptr,
	                           std::vector<std::string>* OutWarnings = nullptr);
};

// 파라미터 값 (이름 → 값). 그래프에 선언되지 않은 이름도 저장한다 (그래프가 나중에 로드돼도 유지)
class FAnimParameterSet
{
public:
	void  Set(std::string_view Name, float Value);
	bool  TryGet(std::string_view Name, float& OutValue) const;
	// 값이 없으면 그래프 선언 기본값, 선언도 없으면 0
	float Get(std::string_view Name, const FAnimGraphAsset& Asset) const;
	void  Clear() { Values.clear(); }

private:
	std::vector<std::pair<std::string, float>> Values;
};

// 상태 샘플 클립 이름 → 모델 클립 번호 (모델의 FAnimationSet마다 한 번)
struct FAnimGraphBinding
{
	std::vector<std::vector<int32>> SampleClips;   // [상태][샘플] → 클립 번호 (-1 = 모델에 없음 — 그 샘플은 빼고 섞는다)
	std::vector<float>              ClipDurations; // 클립 번호 → 길이 (초)

	static FAnimGraphBinding Bind(const FAnimGraphAsset& Asset, const FAnimationSet& Set, std::vector<std::string>* OutMissing = nullptr);
};

// 이번 프레임 포즈에 들어가는 클립 하나
struct FAnimClipContribution
{
	int32 Clip   = -1;
	float Time   = 0.0f; // 초
	float Weight = 0.0f; // 합 = 1
};

// 노티파이를 판정할 기여 (가중치 최대) — AnimNotifyMath::Collect 인자
struct FAnimNotifySource
{
	uint32 Key          = 0; // 기여 식별 (레이어 일련번호 + 샘플). 0 = 없음
	int32  Clip         = -1;
	float  PreviousTime = 0.0f;
	float  NewTime      = 0.0f;
	float  Delta        = 0.0f; // 부호 있는 진행량 (초)
	float  Duration     = 0.0f;
	bool   bLoop        = true;
	bool   bWrapped     = false;
};

namespace AnimGraphMath
{
	// 1D 블렌드 스페이스 가중치 (Positions 오름차순). 이웃한 두 샘플만 0이 아니고 합 = 1. 범위 밖은 끝 샘플 1
	void ComputeBlendSpace1DWeights(const std::vector<float>& Positions, float Value, std::vector<float>& OutWeights);

	bool EvaluateCondition(EAnimConditionOp Op, float Parameter, float Value);

	// 처음 맞는 전이 번호 (-1 = 없음). CurrentPhase = 현재 상태 정규화 시간
	int32 FindTransition(const FAnimGraphAsset& Asset, int32 CurrentState, float CurrentPhase, const FAnimParameterSet& Parameters);

	// 가중 포즈 합: 첫 호출은 bFirst = true. 가중치 합이 1이 되게 넘기고 마지막에 FinishWeightedPose (회전 정규화)
	void AddWeightedPose(std::vector<FNodePose>& Accumulator, const std::vector<FNodePose>& Pose, float Weight, bool bFirst);
	void FinishWeightedPose(std::vector<FNodePose>& Accumulator);
} // namespace AnimGraphMath

// 상태 머신 실행 상태 (포즈 계산 없음 — 순수 로직, 단위 테스트 대상). 결과는 기여 목록 + 노티파이 기준
class FAnimGraphInstance
{
public:
	void Reset();

	// DeltaSeconds만큼 진행 → 전이 판정 → 기여 계산. 처음 호출이면 EntryState로 시작
	void Update(const FAnimGraphAsset& Asset, const FAnimGraphBinding& Binding, const FAnimParameterSet& Parameters, float DeltaSeconds);

	const std::vector<FAnimClipContribution>& GetContributions() const { return Contributions; }
	const FAnimNotifySource&                  GetNotifySource() const { return NotifySource; }

	int32  GetCurrentState() const { return Layers.empty() ? -1 : Layers.back().State; }
	float  GetCurrentPhase() const { return Layers.empty() ? 0.0f : Layers.back().Phase; }
	float  GetStateElapsed() const { return Layers.empty() ? 0.0f : Layers.back().Elapsed; } // 현재 상태에 들어온 뒤 (초)
	size_t GetLayerCount() const { return Layers.size(); }
	bool   IsBlending() const { return Layers.size() > 1; }

private:
	struct FLayer
	{
		int32              State         = 0;
		float              Phase         = 0.0f;
		float              PreviousPhase = 0.0f;
		float              PhaseDelta    = 0.0f; // 이번 프레임 진행량 (감기 전, 부호 있음)
		bool               bWrapped      = false;
		float              Elapsed       = 0.0f;
		float              StartWeight   = 0.0f; // 마지막 전이가 시작될 때의 가중치
		float              Weight        = 0.0f;
		uint32             Serial        = 0;
		std::vector<float> SampleWeights;
	};

	void ComputeSampleWeights(const FAnimGraphAsset& Asset, const FAnimGraphBinding& Binding, const FAnimParameterSet& Parameters, FLayer& Layer);
	void StartTransition(const FAnimGraphTransition& Transition);
	void UpdateLayerWeights();

	std::vector<FLayer>                Layers; // 마지막 = 현재 상태
	float                              BlendElapsed  = 0.0f;
	float                              BlendDuration = 0.0f;
	uint32                             NextSerial    = 1;
	std::vector<FAnimClipContribution> Contributions;
	FAnimNotifySource                  NotifySource;
	std::vector<float>                 PositionScratch;
	std::vector<float>                 WeightScratch;
};

// FAnimGraphComponent의 런타임 (직렬화·리플렉션 제외). 복사(플레이 복제/프리팹)하면 비워지고 다음 갱신에서 다시 해석한다
struct FAnimGraphRuntime
{
	std::shared_ptr<const FAnimGraphAsset> Asset;
	std::string                            ResolvedGraph; // Asset을 읽은 경로 (Graph가 바뀌면 다시 읽는다)
	bool                                   bResolved = false;
	const FAnimationSet*                   BoundSet  = nullptr; // Binding을 만든 세트 (모델이 다시 인스턴스화되면 다시 묶는다)
	FAnimGraphBinding                      Binding;
	FAnimParameterSet                      Parameters;
	FAnimGraphInstance                     Instance;
	uint32                                 NotifyKey = 0; // 직전 프레임 노티파이 기준 기여

	// 캐릭터 이동 자동 공급 (FGameWorld): 위치 변화로 속도를 계산할 때의 직전 월드 위치
	FVector3 PreviousMovementPosition;
	FVector3 SmoothedMovementVelocity;
	bool     bHasMovementSample = false;

	std::vector<FNodePose> PoseScratch;
	std::vector<FNodePose> SampleScratch;

	FAnimGraphRuntime() = default;
	FAnimGraphRuntime(const FAnimGraphRuntime&) {}
	FAnimGraphRuntime& operator=(const FAnimGraphRuntime&) { return *this; }
	FAnimGraphRuntime(FAnimGraphRuntime&&) noexcept            = default; // 풀 재배치는 값을 유지한다
	FAnimGraphRuntime& operator=(FAnimGraphRuntime&&) noexcept = default;
};

// 모델 루트(FAnimationComponent와 같은 엔티티)에 붙여 애니메이션 그래프로 재생한다
//   bUseCharacterMovement: 자신 또는 가장 가까운 조상의 FCharacterMovementComponent 상태를 매 게임플레이 틱 파라미터로 넣는다 (FGameWorld)
//     Speed = 수평 속도 (cm/s), VerticalSpeed = 수직 속도 (cm/s), Grounded = 바닥 (bool). 같은 이름을 스크립트가 써도 덮인다.
//     속도: 이 프로세스가 시뮬레이션하는 캐릭터(서버/Standalone, 예측하는 소유 클라이언트)는 캐릭터 이동 상태, 그 밖(복제 보간을 따르는
//     다른 플레이어)은 보간된 월드 위치의 변화(짧게 평활). 바닥은 어디서나 캡슐 접촉(복제 위치에 맞춘 캡슐 포함)
struct FAnimGraphComponent
{
	std::string Graph;                         // .eanimgraph (Content 기준)
	bool        bUseCharacterMovement = true;

	FAnimGraphRuntime Runtime;
};

// .eanimgraph 공유 캐시 (경로별 한 번 읽음). 경로는 Content 기준(FPrefabLibrary의 Content 폴더) 또는 절대 경로
class FAnimGraphLibrary
{
public:
	static FAnimGraphLibrary& Get();

	// 실패하면 nullptr (경고 로그, 같은 경로는 Invalidate 전까지 다시 읽지 않는다)
	std::shared_ptr<const FAnimGraphAsset> Load(const std::string& AssetPath);
	void                                   Invalidate() { Cache.clear(); }

private:
	std::unordered_map<std::wstring, std::shared_ptr<const FAnimGraphAsset>> Cache;
};
