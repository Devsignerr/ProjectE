#pragma once

#include "Core/CoreTypes.h"
#include "Core/ECS/Entity.h"

#include <functional>

class FBinaryReader;
class FBinaryWriter;
class FScene;
struct FPropertyInfo;
struct FTypeInfo;

// 복제 대상 표시 (에디터에서 붙인다). 이 엔티티의 리플렉션 등록 컴포넌트 값을 서버 → 클라이언트로 보낸다.
// 제외: TF_NoReplicate 타입(계층/프리팹 연결 등), PF_NoReplicate 프로퍼티, 리소스 핸들(클라이언트가 경로로 다시 해석)
struct FReplicatedComponent
{
	int32 OwnerPlayerId = -1; // 소유 플레이어 (서버가 정한다, -1 = 서버 소유). 저장하지 않지만 복제는 된다
};

// 런타임 네트워크 ID (리플렉션 미등록 — 저장/복제/플레이 복제 대상 아님).
// 씬 파일에 있던 복제 엔티티는 양쪽이 같은 씬을 로드한 순서로 1부터, 실행 중 생성은 서버가 DynamicNetIdBase부터 부여
struct FNetIdComponent
{
	uint32 NetId = 0;
};

inline constexpr uint32 InvalidNetId      = 0;
inline constexpr uint32 DynamicNetIdBase  = 1u << 20;
// 서브 씬 (Phase 31-2): 불러온 서브 씬마다 NetId 구간 = SubSceneNetIdBase + (번호 - 1) * SubSceneNetIdStride부터 하위 트리 순서
inline constexpr uint32 SubSceneNetIdBase   = 1u << 26;
inline constexpr uint32 SubSceneNetIdStride = 1u << 14; // 서브 씬 하나에 복제 엔티티 16384개까지

// 복제 컴포넌트 타입 등록 (씬 로드 전에 앱이 호출, 여러 번 호출해도 된다)
void RegisterNetworkTypes();

namespace NetReplication
{
	// 씬에 있는 복제 엔티티에 정적 NetId를 매긴다 (로드 직후, 동적 생성 전에). 서버와 클라이언트가 같은 씬 파일을 로드했으면
	// 같은 결과 (ReplicatedComponent가 추가된 순서 기준 — 엔티티 인덱스와 무관)
	void AssignStaticNetIds(FScene& Scene);

	uint32 GetNetId(const FScene& Scene, FEntity Entity); // 없으면 InvalidNetId

	// 서브 씬 루트 하위(부모 → 자식, 자식 목록 순서)의 복제 엔티티에 NetId를 매긴다. 서버와 클라이언트가 같은 파일을 붙였으면 같은 결과.
	// OnAssigned(엔티티, NetId)는 매길 때마다 (복제 객체가 추적 목록에 넣는다). 구간을 넘는 엔티티는 경고 후 건너뛴다
	uint32 GetSubSceneNetIdBase(uint32 InstanceId);
	void   AssignSubSceneNetIds(FScene& Scene, FEntity Root, uint32 InstanceId, const std::function<void(FEntity, uint32)>& OnAssigned);

	bool IsReplicated(const FTypeInfo& Type);
	bool IsReplicated(const FPropertyInfo& Property);

	// 컴포넌트 값 → 바이트 (복제 대상 프로퍼티만, 선언 순서). Entity 프로퍼티는 NetId로 바꿔 쓴다
	using FEntityToNetId = std::function<uint32(FEntity)>;
	using FNetIdToEntity = std::function<FEntity(uint32)>;
	void WriteComponent(FBinaryWriter& Writer, const FTypeInfo& Type, const void* Component, const FEntityToNetId& ToNetId);
	// 바이트 → 컴포넌트. 리소스 핸들은 비워 경로로 다시 해석되게 한다. 형식이 맞지 않으면 false (컴포넌트는 일부만 바뀔 수 있음)
	bool ReadComponent(FBinaryReader& Reader, const FTypeInfo& Type, void* Component, const FNetIdToEntity& ToEntity);
}
