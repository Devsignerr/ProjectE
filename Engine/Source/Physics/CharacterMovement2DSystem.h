#pragma once

#include "Core/ECS/Entity.h"
#include "Physics/CharacterMovement2D.h"

#include <set>
#include <string>
#include <unordered_map>
#include <vector>

class FPhysics2DSystem;
class FScene;

// 2D 캐릭터 이동기 시스템 (규칙은 Physics/CharacterMovement2D.h). FGameWorld가 소유하고 2D 물리(FPhysics2DSystem)의 월드를 질의한다.
// 누가 언제 SimulateCharacter를 부를지는 FGameWorld가 정한다 (World/GameWorldCharacter2D.cpp — 조종하는 쪽 입력 / 서버의 원격 무브 /
// 클라이언트의 다른 캐릭터는 FollowTransform). 3D FPhysicsSystem의 캐릭터 API와 같은 모양이다.
//   Sync: 새/사라진 캐릭터, 대리 바디 모양·레이어 변경(다시 만듦), 스크립트가 트랜스폼을 직접 바꿨으면 순간이동(속도 유지, 화면 오프셋 버림)
//   SimulateCharacter: 무브 하나 = BeginMove(순수) → 시작 접촉(2cm 부풀린 캡슐: 바닥 법선·발판 속도·가파른 면) → 이동(바닥이면 경사 접선 방향
//     + 발판 속도) → 바닥 판정(부풀린 캡슐, 바닥이었으면 GroundSnapDistance 아래로 캐스트해 붙임) → EndMove → 트랜스폼(+ 화면 오프셋) 쓰기.
//     같은 상태 + 같은 무브 + 같은 월드 → 같은 결과 (멀티플레이 예측/재조정)
//   UpdateProxies: 2D 물리 스텝 전 — 대리 키네마틱 바디를 캐릭터 위치로 옮기고 속도를 맞춘다 (동적 바디를 밀고, 레이캐스트·트리거가 본다)
//   이벤트(점프/착지/대시 시작)는 SetRecordEvents(true)일 때만 쌓는다 (재조정의 다시 적용은 끈다) → FGameWorld가 Lua OnJumped(n)/OnLanded()/
//   OnDashStarted()로 보낸다 (충돌 알림과 같은 단계)
// 입력은 프레임마다 쌓았다가 ConsumePendingMove가 무브로 꺼낸다: AddMovementInput(합), Jump(누름 = 점프 버퍼 + 누르고 있음), StopJumping(뗌 —
//   가변 점프), Dash(방향 — 0이면 입력/속도 방향), DropDown(원웨이 내려가기). Jump는 누른 순간에 한 번 부른다 (매 프레임 부르면 공중 점프가 바로 나간다)
class FCharacterMovement2DSystem
{
public:
	struct FEvent
	{
		FEntity                Entity;
		FCharacterMove2DEvents Events;
	};

	FCharacterMovement2DSystem()  = default;
	~FCharacterMovement2DSystem() = default;
	FCharacterMovement2DSystem(const FCharacterMovement2DSystem&)            = delete;
	FCharacterMovement2DSystem& operator=(const FCharacterMovement2DSystem&) = delete;

	// Physics2D->Begin 뒤 / End 전 (FGameWorld)
	void Begin(FPhysics2DSystem& InPhysics2D);
	void End();
	bool IsActive() const;

	void   Sync(FScene& Scene);
	bool   HasCharacter(FEntity Entity) const { return Characters.contains(Entity); }
	uint32 GetCharacterCount() const { return static_cast<uint32>(Characters.size()); }

	// ---- 입력 (스크립트/게임 모듈 — 이 캐릭터를 조종하는 쪽에서). 벡터는 월드 3D (X·Z 성분)
	void AddMovementInput(FEntity Entity, const FVector3& WorldDirection);
	void Jump(FEntity Entity);
	void StopJumping(FEntity Entity);
	void Dash(FEntity Entity, const FVector3& WorldDirection);
	void DropDown(FEntity Entity);
	FCharacterMove2D ConsumePendingMove(FEntity Entity, float DeltaSeconds);

	// ---- 시뮬레이션 (FGameWorld)
	void              SimulateCharacter(FScene& Scene, FEntity Entity, const FCharacterMove2D& Move);
	FCharacterState2D GetState(FEntity Entity) const;
	void              SetState(FScene& Scene, FEntity Entity, const FCharacterState2D& State); // 보정/재조정 시작점 (트랜스폼도)
	void              FollowTransform(FScene& Scene, FEntity Entity);                         // 클라이언트의 다른 캐릭터: 복제 위치로 대리 바디만
	void              SetVisualOffset(FScene& Scene, FEntity Entity, const FVector2& Offset);  // 화면 보정 (상태는 그대로)
	void              UpdateProxies();

	// ---- 상태 질의 (없는 엔티티는 0/false)
	bool     IsGrounded(FEntity Entity) const;
	bool     IsDashing(FEntity Entity) const;
	FVector2 GetVelocity(FEntity Entity) const; // 평면 cm/s
	int32    GetJumpsRemaining(FEntity Entity) const;
	int32    GetDashesRemaining(FEntity Entity) const; // 지금 대시할 수 있는 횟수 (바닥 = 1, 쿨다운·대시 중이면 0)

	void SetRecordEvents(bool bRecord) { bRecordEvents = bRecord; }
	void ConsumeEvents(std::vector<FEvent>& OutEvents); // 끝에 붙이고 비운다

private:
	struct FCharacter
	{
		FCharacterState2D             State;
		FCharacterMovement2DComponent CreatedWith; // 대리 바디를 만든 설정 (모양·레이어가 바뀌면 다시 만든다)
		FCharacterMovement2DComponent Settings;    // 지난 Sync의 컴포넌트 값 (입력 정리·남은 횟수 질의)
		uint32                        Proxy = ~0u;
		uint8                         Layer = 0;
		float                         Depth = 0.0f; // 월드 Y (바꾸지 않는다)
		FVector2                      PendingInput;
		FVector2                      PendingDashDirection;
		bool                          bPendingJump = false;
		bool                          bJumpHeld    = false;
		bool                          bPendingDash = false;
		bool                          bPendingDrop = false;
		FVector2                      VisualOffset;
		FVector3                      WrittenPosition; // 마지막으로 쓴 로컬 위치 (스크립트 순간이동 감지)
		bool                          bWritten  = false;
		uint64                        LastSeen  = 0;
	};

	void  CreateProxy(FCharacter& Character, FEntity Entity, const FCharacterMovement2DComponent& Movement);
	void  DestroyProxy(FCharacter& Character);
	uint8 ResolveLayer(const std::string& Name);
	void  WriteTransform(FScene& Scene, FEntity Entity, FCharacter& Character);

	FPhysics2DSystem*                          Physics2D = nullptr;
	std::unordered_map<FEntity, FCharacter>    Characters;
	std::vector<FEvent>                        PendingEvents;
	std::set<std::string>                      WarnedLayers;
	uint64                                     SyncCounter   = 0;
	bool                                       bRecordEvents = true;
};
