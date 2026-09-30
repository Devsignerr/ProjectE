#pragma once

#include "AI/Navigation/NavMesh.h"
#include "Core/CoreTypes.h"

#include <string>

// AI 컴포넌트 (RegisterAITypes로 리플렉션 등록). 실행 상태(트리 인스턴스, 이동 경로, 내비메시)는 FAISystem이 엔티티별로 가진다

// 비헤이비어 트리 실행: 플레이 중(서버/Standalone) Asset(.ebt, Content 기준)을 이 엔티티에서 돌린다
struct FBehaviorTreeComponent
{
	std::string Asset;
	bool        bAutoStart = true; // false면 스크립트/게임 모듈이 FAISystem::StartTree로 시작한다
};

// 이동 에이전트 설정 (MoveTo/RotateTo가 읽는다. 없으면 기본값)
struct FNavAgentComponent
{
	float MaxSpeed          = 300.0f; // cm/s
	float AcceptanceRadius  = 20.0f;  // 도착 판정 수평 거리 (cm)
	float TurnSpeedDegrees  = 540.0f; // 도/초
	bool  bOrientToMovement = true;   // 이동 방향으로 몸을 돌린다 (트랜스폼을 직접 옮길 때만)
};

// 내비메시: 씬에 하나. 굽기 설정 + 구운 결과 파일(.enav, Content 기준). 에디터 굽기 명령이 NavMeshAsset을 채운다
struct FNavMeshComponent
{
	std::string NavMeshAsset;
	float       AgentRadius          = 35.0f;
	float       AgentHeight          = 180.0f;
	float       AgentMaxClimb        = 40.0f;
	float       AgentMaxSlopeDegrees = 45.0f;
	float       CellSize             = 20.0f;
	float       CellHeight           = 10.0f;

	FNavMeshBuildSettings ToBuildSettings() const
	{
		FNavMeshBuildSettings Settings;
		Settings.AgentRadius          = AgentRadius;
		Settings.AgentHeight          = AgentHeight;
		Settings.AgentMaxClimb        = AgentMaxClimb;
		Settings.AgentMaxSlopeDegrees = AgentMaxSlopeDegrees;
		Settings.CellSize             = CellSize;
		Settings.CellHeight           = CellHeight;
		return Settings;
	}
};
