#pragma once

#include "Core/CoreTypes.h"
#include "Core/Math/Math.h"
#include "Scene/Animation.h"

#include <filesystem>
#include <memory>
#include <optional>
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
// 상태 = 클립 하나, 1D 블렌드 스페이스 (BlendParameter 값 → 위치가 이웃한 두 샘플을 선형 가중. 범위 밖은 끝 샘플),
//   또는 2D 블렌드 스페이스 (BlendParameterY도 있음. 샘플 위치 (Position, PositionY) — 가중치는 그래디언트 밴드
//   (Freeform Cartesian, AnimGraphMath::ComputeBlendSpace2DWeights): 축마다 샘플 범위로 정규화한 뒤 계산, 샘플 위치에서 정확히 그 샘플,
//   어떤 점 배치든 삼각분할 없이 연속, 범위 밖은 가까운 쪽 샘플로 수렴. 샘플 순서는 의미 없음).
//   재생 위치는 상태마다 정규화 시간(Phase, 0~1) 하나: 샘플들은 같은 Phase로 샘플링된다 (동기화 — 걷기/뛰기 발이 맞는다, 2D도 같음).
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
// 형식 버전: 1 = 실행 데이터만, 2 = 편집기 정보 추가 (상태 "EditorPosition", 최상위 "Editor": {PreviewModel, AnyStatePosition}),
//   3 = 2D 블렌드 스페이스 (상태 "BlendParameterY", 2D 샘플 "Position": [x, y]).
//   읽기는 1/2/3 모두 받고(편집기 정보가 없으면 편집기가 자동 배치), 쓰기는 항상 3. 전이 우선순위 = Transitions 목록 순서.
// 핫 리로드: FAnimGraphLibrary::Invalidate가 세대 번호를 올리면 그 그래프를 쓰는 컴포넌트가 다음 갱신에서 다시 읽고, 파일이 바뀌었으면
//   새 에셋으로 다시 묶는다. 파라미터 값은 유지되고, 같은 이름의 상태가 새 그래프에 있으면 그 상태에서 다시 시작한다 (없으면 시작 상태).

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
	float       Position  = 0.0f; // 블렌드 파라미터 값 (2D면 X축)
	float       PositionY = 0.0f; // 2D 블렌드 스페이스의 Y축 값
	float       Rate      = 1.0f; // 이 샘플의 재생 배속 (한 바퀴 길이 계산에 들어간다)
};

struct FAnimGraphState
{
	std::string                   Name;
	std::string                   BlendParameter;  // 비면 단일 클립 (Samples[0])
	std::string                   BlendParameterY; // 있으면 2D 블렌드 스페이스 (BlendParameter = X축)
	std::vector<FAnimBlendSample> Samples;         // 1D는 Position 오름차순, 2D는 순서 무관
	float                         Speed = 1.0f;
	bool                          bLoop = true;

	std::optional<FVector2> EditorPosition; // 편집기 노드 위치 (실행에 쓰지 않음)

	bool IsBlendSpace() const { return !BlendParameter.empty(); }
	bool Is2D() const { return !BlendParameter.empty() && !BlendParameterY.empty(); }
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
	static constexpr int32          Version   = 3;
	static constexpr const wchar_t* Extension = L".eanimgraph";

	std::vector<FAnimGraphParameter>  Parameters;
	std::vector<FAnimGraphState>      States;
	std::vector<FAnimGraphTransition> Transitions;
	int32                             EntryState = 0;

	// 편집기 정보 (버전 2, 실행에 쓰지 않음)
	std::string             PreviewModel;           // 미리보기 모델 (Content 기준)
	std::optional<FVector2> AnyStateEditorPosition; // "어느 상태든" 노드 위치

	int32                      FindState(std::string_view Name) const;
	const FAnimGraphParameter* FindParameter(std::string_view Name) const;

	// 형식이 틀리면 false + 이유. 이름을 못 찾는 전이/조건은 건너뛰고 경고 목록(OutWarnings)에 남긴다
	static bool FromJsonString(const std::string& Text, FAnimGraphAsset& Out, std::string* OutError = nullptr,
	                           std::vector<std::string>* OutWarnings = nullptr);
	// 버전 2 JSON (들여쓰기 2). bool 파라미터 조건/기본값은 true/false로 쓴다
	std::string ToJsonString() const;
	bool        SaveToFile(const std::filesystem::path& Path) const;

	// 상태 Index 삭제: 그 상태가 From/To인 전이를 지우고 나머지 전이·시작 상태 번호를 당긴다
	void RemoveState(int32 Index);
	// 편집기 "새 애니메이션 그래프" 기본값 (파라미터 Speed + 빈 클립 상태 하나)
	static FAnimGraphAsset MakeDefault();
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
	// 2D 블렌드 스페이스 가중치 (그래디언트 밴드). 축마다 샘플 범위로 정규화 → w_i = min_j clamp(1 - (p-p_i)·(p_j-p_i)/|p_j-p_i|², 0, 1)
	// → 합 1로 정규화. 겹친 샘플 쌍은 건너뛰고, 모두 0이면 가장 가까운 샘플 1
	void ComputeBlendSpace2DWeights(const std::vector<FVector2>& Positions, const FVector2& Value, std::vector<float>& OutWeights);

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
	// Reset + 첫 Update는 EntryState 대신 State에서 시작 (핫 리로드로 상태 이어 가기). State < 0이면 Reset과 같다
	void ResetToState(int32 State);

	// DeltaSeconds만큼 진행 → 전이 판정 → 기여 계산. 처음 호출이면 EntryState로 시작
	void Update(const FAnimGraphAsset& Asset, const FAnimGraphBinding& Binding, const FAnimParameterSet& Parameters, float DeltaSeconds);

	const std::vector<FAnimClipContribution>& GetContributions() const { return Contributions; }
	const FAnimNotifySource&                  GetNotifySource() const { return NotifySource; }

	int32  GetCurrentState() const { return Layers.empty() ? -1 : Layers.back().State; }
	float  GetCurrentPhase() const { return Layers.empty() ? 0.0f : Layers.back().Phase; }
	float  GetStateElapsed() const { return Layers.empty() ? 0.0f : Layers.back().Elapsed; } // 현재 상태에 들어온 뒤 (초)
	size_t GetLayerCount() const { return Layers.size(); }
	bool   IsBlending() const { return Layers.size() > 1; }
	// 레이어 Index의 상태/가중치 (0 = 가장 오래된 것, 마지막 = 현재). 편집기 디버그 표시용
	int32 GetLayerState(size_t Index) const { return Layers[Index].State; }
	float GetLayerWeight(size_t Index) const { return Layers[Index].Weight; }
	// 마지막 전이 번호 (-1 = 없음)와 지금까지 전이 횟수 (새 전이 감지용)
	int32  GetLastTransition() const { return LastTransition; }
	uint32 GetTransitionCount() const { return TransitionCount; }

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
	int32                              StartState      = -1; // ResetToState
	int32                              LastTransition  = -1;
	uint32                             TransitionCount = 0;
	std::vector<FAnimClipContribution> Contributions;
	FAnimNotifySource                  NotifySource;
	std::vector<float>                 PositionScratch;
	std::vector<FVector2>              Position2DScratch;
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

	uint32      ResolvedGeneration = 0; // 읽을 때의 FAnimGraphLibrary 세대 (바뀌면 다시 읽는다 — 핫 리로드)
	std::string ResumeState;            // 다시 묶을 때 이어 갈 상태 이름 (SetAsset)

	// 에셋 교체 (같은 포인터면 무시) → 다음 Rebind. bKeepState면 현재 상태 이름을 기억해 그 상태에서 다시 시작한다
	void SetAsset(std::shared_ptr<const FAnimGraphAsset> NewAsset, bool bKeepState);
	// Set 클립에 다시 묶고 인스턴스를 처음부터 (ResumeState가 새 그래프에 있으면 그 상태에서). 모델에 없는 클립 이름은 OutMissing에
	void Rebind(const FAnimationSet& Set, std::vector<std::string>* OutMissing);

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
	// 캐시 비우기 + 세대 증가 → 사용 중인 컴포넌트가 다음 갱신에서 다시 읽는다 (내용이 같아도 새 객체 = 다시 묶임)
	void   Invalidate();
	void   Invalidate(const std::string& AssetPath); // 경로 하나만 (Content 기준 또는 절대)
	uint32 GetGeneration() const { return Generation; }

private:
	static std::wstring MakeKey(const std::string& AssetPath);

	std::unordered_map<std::wstring, std::shared_ptr<const FAnimGraphAsset>> Cache;
	uint32                                                                   Generation = 1;
};
