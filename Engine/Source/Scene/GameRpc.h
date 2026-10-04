#pragma once

#include "Core/CoreTypes.h"
#include "Core/ECS/Entity.h"
#include "Core/Math/Math.h"

#include <string>
#include <vector>

class FInput;

// 네트워크 RPC — Lua(entity:CallServer 등)와 게임 모듈 C++(IGameNet::CallRpc)이 같은 경로를 쓴다.
// 받는 쪽: 대상 엔티티 스크립트의 Server_/Client_/Multicast_<이름> 메서드 + 게임 모듈 IGameModule::OnRpc(서버에서만)
enum class EGameRpcKind : uint8
{
	Server,    // 클라이언트 → 서버 (대상 엔티티 소유자만). 서버에서 부르면 바로 로컬 실행
	Client,    // 서버 → 대상 엔티티 소유 클라이언트 (호스트 소유면 로컬)
	Multicast, // 서버 → 서버 자신 + 모든 클라이언트
};

const char* GetRpcMethodPrefix(EGameRpcKind Kind); // "Server_" / "Client_" / "Multicast_"

// RPC 인자 값
struct FGameRpcValue
{
	enum class EType : uint8
	{
		Nil,
		Bool,
		Number, // bInteger면 정수
		String,
		Vector3,
		Asset,  // String = Content 기준 경로, AssetFilter = 확장자 (Lua Prefab()/Asset() 값)
		Entity, // 전송 시 NetId로 바뀐다 (복제되지 않은 엔티티는 받는 쪽에서 무효)
	};

	EType       Type     = EType::Nil;
	bool        bBool    = false;
	bool        bInteger = false;
	double      Number   = 0.0;
	std::string String;
	std::string AssetFilter;
	FVector3    Vector;
	FEntity     Entity;

	static FGameRpcValue MakeBool(bool bValue);
	static FGameRpcValue MakeNumber(double Value, bool bIsInteger = false);
	static FGameRpcValue MakeString(std::string Value);
	static FGameRpcValue MakeVector3(const FVector3& Value);
	static FGameRpcValue MakeAsset(std::string Path, std::string Filter);
	static FGameRpcValue MakeEntity(FEntity Value);
};

using FGameRpcArgs = std::vector<FGameRpcValue>;

// 게임 모듈이 쓰는 네트워크 기능 (FGameWorld가 구현, IGameModule::GetNet()으로 얻는다). Standalone에서도 동작한다
class IGameNet
{
public:
	virtual ~IGameNet() = default;

	virtual bool  IsServer() const                = 0; // 게임 로직 권한 (서버/Standalone)
	virtual bool  IsClient() const                = 0; // 로컬 플레이어/화면이 있음
	virtual int32 GetLocalPlayerId() const        = 0; // 전용 서버 -1
	virtual int32 GetOwner(FEntity Entity) const  = 0; // 가장 가까운 복제 조상의 소유 플레이어, 없으면 -1
	// Name은 접두사 없는 이름 ("Fire" → Server_Fire). 잘못된 호출(클라이언트에서 Client/Multicast)은 경고 후 무시
	virtual void CallRpc(FEntity Target, EGameRpcKind Kind, const std::string& Name, const FGameRpcArgs& Args) = 0;
	// 이 엔티티를 조종하는 플레이어의 입력 (키 + 입력 액션 — GetActionValue("Move"), WasActionPressed("Jump") 등. Lua Input과 같은 규칙:
	// 서버는 소유 플레이어가 보낸 입력, 서버 소유/호스트 소유는 로컬 입력, 없으면 nullptr). OnUpdate 등 게임플레이 틱 안에서만 유효
	virtual const FInput* GetInput(FEntity Entity) const = 0;
	// 맵 전환 (Lua Game.OpenScene과 같음): 서버에서만, 이번 프레임 끝에 현재 씬을 SceneAsset(Content 기준 .escene)으로 바꾼다.
	// 접속한 클라이언트도 따라 이동하고 플레이어는 새 씬에서 다시 생성된다. 거절(클라이언트/파일 없음)이면 false + 경고 로그
	virtual bool OpenScene(const std::string& SceneAsset) = 0;
	// 서브 씬 (Lua Scene.LoadSubScene과 같음, 서버에서만): Asset(.escene)을 Offset 위치 루트 아래로 불러온다(파싱은 백그라운드, 붙이기는
	// 다음 게임플레이 틱). 클라이언트도 같은 파일을 붙인다. Unload는 루트째 지연 파괴
	virtual bool LoadSubScene(const std::string& Asset, const FVector3& Offset) = 0;
	virtual bool UnloadSubScene(const std::string& Asset)                       = 0;
	virtual bool IsSubSceneLoaded(const std::string& Asset) const               = 0;
	// 게임 시간 배율 (Lua Game.SetTimeScale/GetTimeScale/HitStop/GetHitStopRemaining, Time.UnscaledDeltaTime과 같은 규칙 — World/GameWorld.cpp
	// 머리 주석 "시간 배율"): Standalone 전용 — 네트워크 세션에서는 Set/HitStop이 false(경고 한 번). 배율은 다음 게임플레이 틱부터.
	// OnUpdate의 DeltaSeconds는 배율을 곱한 값이고 GetUnscaledDeltaSeconds는 이번(직전) 게임플레이 틱의 실제 dt
	virtual bool  SetTimeScale(float Scale)          = 0; // 0 = 정지, 0~100으로 자름
	virtual float GetTimeScale() const               = 0;
	virtual bool  HitStop(float Seconds)             = 0; // 실제 시간 Seconds 동안 배율 0 (겹치면 긴 쪽)
	virtual float GetHitStopRemaining() const        = 0;
	virtual float GetUnscaledDeltaSeconds() const    = 0;
};
