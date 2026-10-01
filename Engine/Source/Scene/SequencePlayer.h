#pragma once

#include "Core/CoreTypes.h"
#include "Core/ECS/Entity.h"
#include "Core/Math/Math.h"
#include "Scene/Sequence.h"

#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

class FScene;
struct FPropertyInfo;
struct FTypeInfo;

// 시퀀스 적용 상태 (재생 컴포넌트 런타임과 편집기 미리보기가 각자 하나씩 가진다).
//   처음 쓰는 프로퍼티마다 원래 값을 기억해 Restore로 되돌린다 (카메라 컷은 컷이 바뀌거나 끝나면 항상 되돌림).
struct FSequenceEvalState
{
	using FValue = std::variant<bool, int32, uint32, float, FVector2, FVector3, FVector4, FQuat, std::string>;
	struct FSaved
	{
		FEntity              Entity;
		const FTypeInfo*     Type     = nullptr;
		const FPropertyInfo* Property = nullptr;
		FValue               Value;
	};

	std::vector<FSaved>  Saved;
	std::vector<FEntity> Bindings;   // 트랙별 대상 (CameraCut/Event는 비움). Evaluate가 무효하면 다시 찾는다
	std::vector<FEntity> CutCameras; // 카메라 컷 트랙의 컷별 카메라
	FEntity              ActiveCamera;
	std::vector<bool>    WarnedTracks; // 대상/프로퍼티를 못 찾은 경고는 트랙마다 한 번

	bool HasSaved() const { return !Saved.empty(); }
	// 기억한 엔티티 중 사라진 것이 있는가 (씬을 다시 읽었거나 실행 취소로 엔티티가 새로 만들어짐 → 원래 값 기록이 무효)
	bool HasStaleEntities(const FScene& Scene) const;
	// 씬 값과 기억한 원래 값을 맞바꾼다 (두 번 부르면 원상태). 미리보기 중 씬 저장/스냅샷 직전·직후에
	void SwapWithScene(FScene& Scene);
	void Forget(); // 되돌리지 않고 기록만 버림
};

// 씬 컴포넌트: 시퀀스 재생기. 재생 상태는 Runtime (직렬화/리플렉션 제외, 복사하면 비워진다 — 플레이 복제/프리팹마다 따로)
//   bAutoPlay: 플레이 시작 후 첫 갱신에 처음부터 재생. bRestoreState: 끝나거나 멈추면 시퀀스가 바꾼 값을 원래대로 (끄면 마지막 값 유지 — 열린 문)
//   멀티플레이: 복제하지 않는 로컬 연출 (각 프로세스가 자기 시계로 재생). 맞추려면 서버 스크립트가 Multicast RPC로 PlaySequence를 부른다
struct FSequencePlayerRuntime
{
	std::shared_ptr<const FSequenceAsset> Asset;
	std::string                           ResolvedPath;
	uint32                                ResolvedGeneration = 0;
	bool                                  bResolved          = false;
	bool                                  bAutoPlayChecked   = false;
	bool                                  bPlaying           = false;
	bool                                  bPendingStart      = false; // Play 요청 → 다음 갱신에서 시작 시각 포함 이벤트
	float                                 Time               = 0.0f;
	FSequenceEvalState                    State;

	// 이번 갱신에 발생 (매 갱신 비우고 채움 — Lua OnSequenceEvent_<이름>, 끝나면 OnSequenceFinished)
	std::vector<std::string> Events;
	bool                     bFinishedThisUpdate = false;

	FSequencePlayerRuntime() = default;
	FSequencePlayerRuntime(const FSequencePlayerRuntime&) {}
	FSequencePlayerRuntime& operator=(const FSequencePlayerRuntime&) { return *this; }
	FSequencePlayerRuntime(FSequencePlayerRuntime&&) noexcept            = default;
	FSequencePlayerRuntime& operator=(FSequencePlayerRuntime&&) noexcept = default;
};

struct FSequencePlayerComponent
{
	std::string Sequence;             // .esequence (Content 기준)
	bool        bAutoPlay     = true;
	bool        bLoop         = false;
	float       PlayRate      = 1.0f;
	bool        bRestoreState = false;

	FSequencePlayerRuntime Runtime;
};

// 시퀀스 재생/적용 (FGameWorld 게임플레이 틱: 스크립트 뒤 · 캐릭터 이동/물리 앞 — 스크립트의 Play가 같은 틱에 반영되고,
// 시퀀스가 쓴 트랜스폼은 이번 프레임 물리·UpdateTransforms에, 애니메이션 시각은 이번 프레임 표시 틱 FAnimationSystem에 들어간다)
class FSequenceSystem
{
public:
	static void Update(FScene& Scene, float DeltaSeconds);

	// Entity의 FSequencePlayerComponent (없으면 false). Asset이 비어 있지 않으면 시퀀스를 바꾼다 (컴포넌트가 없으면 붙인다)
	static bool  Play(FScene& Scene, FEntity Entity, std::string_view Asset, float StartTime = 0.0f);
	static void  Stop(FScene& Scene, FEntity Entity);  // 정지 + 카메라 컷 해제 (+ bRestoreState면 원래 값)
	static void  Pause(FScene& Scene, FEntity Entity); // 현재 시각에서 멈춤 (값 유지, Play로 다시 — StartTime < 0이면 이어서)
	static bool  IsPlaying(const FScene& Scene, FEntity Entity);
	static float GetTime(const FScene& Scene, FEntity Entity);
	static void  SetTime(FScene& Scene, FEntity Entity, float Seconds); // 이동만 (그 사이 이벤트 없음). 다음 Update에 적용
	static float GetDuration(FScene& Scene, FEntity Entity);

	// ---- 적용 (재생기·편집기 공용)
	// Asset을 Time 시점으로 Scene에 적용. Context = 바인딩 기준 엔티티 (재생 엔티티, 없으면 NullEntity — "" 대상은 못 찾음).
	// 이벤트는 다루지 않는다 (호출자가 SequenceMath::CollectEvents)
	static void Evaluate(FScene& Scene, FEntity Context, const FSequenceAsset& Asset, float Time, FSequenceEvalState& State);
	// 기억한 원래 값 되돌리기 + 카메라 컷 해제. 상태는 비워진다
	static void Restore(FScene& Scene, FSequenceEvalState& State);
	// 이름 경로 → 엔티티 (규칙은 Scene/Sequence.h 머리 주석)
	static FEntity ResolveBinding(const FScene& Scene, FEntity Context, std::string_view Path);
	// 엔티티 → 이름 경로 (편집기 "선택한 엔티티로"). Context 하위면 그 기준, 이름이 겹치면 부모 이름을 붙인다. Context 자신이면 ""
	static std::string MakeBindingPath(const FScene& Scene, FEntity Context, FEntity Target);

	static constexpr int32 CameraCutPriority = 1 << 20; // 컷 카메라에 주는 우선순위 (플레이어 카메라 위)
};
