#pragma once

#include "Core/CoreTypes.h"
#include "Core/ECS/Entity.h"

#include <map>
#include <string>
#include <string_view>
#include <vector>

class FScene;

// 게임플레이 기본 틀 (Phase 25): 체력/데미지 + 게임 모드(규칙·점수·승패). 컴포넌트는 리플렉션 등록 → 인스펙터·저장·복제·Lua 자동.
//
// 권한: 데미지/회복/점수/매치 상태 변경은 서버(Standalone 포함)만 한다. 클라이언트는 복제된 값만 읽는다
//   (Lua 바인딩은 클라이언트에서 아무것도 하지 않고 0/false를 돌려준다. C++ 함수는 호출한 쪽 씬을 바꾸므로 서버 코드에서만 부른다).
// 이벤트: Gameplay::ApplyDamage는 값을 바로 바꾸고 이벤트를 컴포넌트 런타임 큐에 쌓는다. FGameWorld가 같은 프레임 게임플레이 단계
//   (스크립트·게임 모듈·AI 뒤, 물리 앞)에서 꺼내 Lua OnDamaged(amount, instigator)/OnDeath(instigator) + 게임 모듈 OnDamaged/OnDeath,
//   모든 스크립트에 OnEntityDied(victim, instigator)를 부르고 사망 처리(점수, 파괴, 리스폰 예약)를 한다.
// 사망 = Health <= 0 (따로 플래그 없음 — 복제된 Health만으로 클라이언트도 판단한다). 죽은 대상에는 데미지/회복이 들어가지 않는다.

// 죽었을 때 할 일 (4바이트: 리플렉션 enum — 씬 JSON은 번호로 저장되므로 끝에만 추가)
enum class EDeathAction : int32
{
	Auto,                 // 플레이어 소유(ReplicatedComponent OwnerPlayerId >= 0)면 PlayerStart에서 리스폰, 아니면 그대로 둔다
	Destroy,              // 엔티티 파괴 (이벤트 뒤, 스크립트 OnDestroy 호출)
	RespawnInPlace,       // 리스폰 지연 뒤 같은 자리에서 체력 회복 (표적/오브젝트)
	RespawnAtPlayerStart, // 리스폰 지연 뒤 PlayerStart에서 (Standalone처럼 소유자가 없는 플레이어용)
};

struct FDamageEvent
{
	float   Amount = 0.0f; // 실제로 깎인 양
	FEntity Instigator;    // 가해자 (없으면 무효)
	bool    bKilled = false;
};

// 비직렬화 런타임 상태 (리플렉션 미등록). 복사(플레이 복제/프리팹)하면 비워진다
struct FHealthRuntime
{
	std::vector<FDamageEvent> PendingEvents; // FGameWorld가 게임플레이 단계에서 비운다
	float                     RespawnTimer = -1.0f; // 서버: 리스폰까지 남은 시간 (< 0 = 예약 없음)

	FHealthRuntime() = default;
	FHealthRuntime(const FHealthRuntime&) {}
	FHealthRuntime& operator=(const FHealthRuntime&) { return *this; }
	FHealthRuntime(FHealthRuntime&&) noexcept            = default; // 풀 재배치는 값을 유지한다
	FHealthRuntime& operator=(FHealthRuntime&&) noexcept = default;
};

struct FHealthComponent
{
	float        MaxHealth     = 100.0f;
	float        Health        = 100.0f; // 현재 체력 (서버 권위, 복제)
	bool         bInvulnerable = false;
	EDeathAction DeathAction   = EDeathAction::Auto;
	int32        ScoreValue    = 0; // 처치하면 가해자 소유 플레이어가 받는 점수 (게임 모드 진행 중에만)

	FHealthRuntime Runtime;
};

// 매치 상태 (4바이트: 리플렉션 enum, 번호는 복제·저장에 쓰이므로 끝에만 추가)
enum class EMatchState : int32
{
	WaitingToStart, // StartDelay 동안 대기
	InProgress,     // 진행 (점수/제한 시간 판정)
	Ended,          // 끝 (WinnerPlayerId)
};

struct FGameModeRuntime
{
	float StateTime = 0.0f; // 서버: 현재 상태에 들어온 뒤 지난 시간 (정밀 값 — 복제는 RemainingSeconds만)

	FGameModeRuntime() = default;
	FGameModeRuntime(const FGameModeRuntime&) {}
	FGameModeRuntime& operator=(const FGameModeRuntime&) { return *this; }
	FGameModeRuntime(FGameModeRuntime&&) noexcept            = default; // 풀 재배치는 값을 유지한다
	FGameModeRuntime& operator=(FGameModeRuntime&&) noexcept = default;
};

// 게임 모드 + 게임 상태 (언리얼 GameMode/GameState를 한 컴포넌트로): 씬에 하나 (첫 번째만 쓴다).
// 규칙은 저장되는 값, 상태는 PF_Transient(저장 안 함, 복제됨). 멀티플레이에서는 같은 엔티티에 ReplicatedComponent를 붙인다
struct FGameModeComponent
{
	// ---- 규칙 (서버가 읽는다)
	float StartDelay   = 0.0f; // 대기 → 진행까지 (초)
	float TimeLimit    = 0.0f; // 진행 제한 시간 (초, 0 = 무제한)
	int32 ScoreToWin   = 0;    // 이 점수에 먼저 닿으면 승리 (0 = 없음)
	float RespawnDelay = 3.0f; // 사망 → 리스폰 (초, < 0 = 리스폰 안 함)

	// ---- 상태 (복제)
	EMatchState MatchState       = EMatchState::WaitingToStart;
	int32       RemainingSeconds = 0;  // 진행 중 남은 시간 (올림, 제한 시간이 없으면 0)
	int32       WinnerPlayerId   = -1; // 끝났을 때 승자 (-1 = 없음/무승부)
	std::string Scores;                // "플레이어=점수;..." (플레이어 ID 순, Gameplay::ParseScores)

	FGameModeRuntime Runtime;
};

namespace Gameplay
{
	// ---- 체력 (순수 규칙)
	// 깎인 양을 돌려준다. 0 이하/비정상 값, 무적, 이미 죽음이면 0. bOutKilled = 이번 데미지로 죽었다 (Health는 0으로 맞춘다)
	float ApplyDamage(FHealthComponent& Health, float Amount, bool& bOutKilled);
	// 회복한 양 (MaxHealth를 넘지 않음). 죽은 대상은 회복하지 않는다 (리스폰으로만 되살아난다)
	float Heal(FHealthComponent& Health, float Amount);
	inline bool IsDead(const FHealthComponent& Health) { return Health.Health <= 0.0f; }

	// ---- 체력 (씬): 대상에 FHealthComponent가 없으면 0. 데미지는 이벤트를 쌓는다 (머리 주석)
	float ApplyDamage(FScene& Scene, FEntity Target, float Amount, FEntity Instigator);
	float Heal(FScene& Scene, FEntity Target, float Amount);
	bool  IsDead(const FScene& Scene, FEntity Target); // 체력 컴포넌트가 없으면 false

	// ---- 게임 모드
	const char* ToString(EMatchState State); // "WaitingToStart" / "InProgress" / "Ended"
	FEntity     FindGameMode(FScene& Scene);               // 첫 FGameModeComponent 엔티티 (없으면 무효)

	std::map<int32, int32> ParseScores(std::string_view Text); // 잘못된 항목은 건너뛴다
	std::string            FormatScores(const std::map<int32, int32>& Scores);
	int32                  GetScore(const FGameModeComponent& GameMode, int32 PlayerId);
	void                   AddScore(FGameModeComponent& GameMode, int32 PlayerId, int32 Amount); // PlayerId < 0이면 무시
	int32                  FindLeader(const std::map<int32, int32>& Scores); // 최고 점수 플레이어, 없거나 공동 1위면 -1

	// 상태 전환 (StateTime 초기화). 진행으로 들어가면 점수·승자를 비우고 남은 시간을 채운다
	void SetMatchState(FGameModeComponent& GameMode, EMatchState State);
	// 서버 한 틱: 대기 → (StartDelay) → 진행 → (제한 시간 / ScoreToWin) → 끝. 상태가 바뀌었으면 true
	bool TickMatch(FGameModeComponent& GameMode, float DeltaSeconds);
	void EndMatch(FGameModeComponent& GameMode, int32 WinnerPlayerId); // 진행 중이 아니어도 끝낸다
	void RestartMatch(FGameModeComponent& GameMode);                    // 대기로 (점수 초기화)
} // namespace Gameplay
