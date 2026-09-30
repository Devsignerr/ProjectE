#pragma once

#include "Core/CoreTypes.h"
#include "Core/Math/Vector3.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// 내비메시 굽기 설정. 길이는 엔진 단위(cm), 각도는 도. Recast 복셀 단위 변환은 FNavMesh::Build가 한다.
// 직렬화(.enav)에 그대로 들어가므로 필드를 바꾸면 FNavMesh의 파일 버전을 올린다.
struct FNavMeshBuildSettings
{
	float CellSize             = 20.0f;  // 수평 복셀 크기 (cm)
	float CellHeight           = 10.0f;  // 수직 복셀 크기 (cm)
	float AgentRadius          = 35.0f;  // 에이전트 반경 (cm). 벽에서 이만큼 침식한다
	float AgentHeight          = 180.0f; // 에이전트 키 (cm). 천장이 이보다 낮으면 못 지나간다
	float AgentMaxClimb        = 40.0f;  // 오를 수 있는 턱 높이 (cm)
	float AgentMaxSlopeDegrees = 45.0f;  // 걸을 수 있는 최대 경사 (도)
	int32 RegionMinSize        = 8;      // 이보다 작은 고립 영역 제거 (한 변 셀 수, 면적 = 제곱)
	int32 RegionMergeSize      = 20;     // 이보다 작은 영역은 이웃과 병합 (한 변 셀 수, 면적 = 제곱)
	float EdgeMaxLength        = 1200.0f; // 윤곽 가장자리 최대 길이 (cm, 0 = 제한 없음)
	float EdgeMaxError         = 26.0f;  // 윤곽 단순화 허용 오차 (cm)
	int32 VertsPerPoly         = 6;      // 폴리곤당 최대 정점 수 (3~6)
	float DetailSampleDistance = 120.0f; // 높이 디테일 샘플 간격 (cm, CellSize × 0.9 미만이면 디테일 샘플 없음)
	float DetailSampleMaxError = 10.0f;  // 디테일 메시가 실제 표면에서 벗어날 수 있는 높이 (cm)
};

// 굽기 입력: 엔진 월드 좌표(cm) 삼각형 목록.
// 와인딩은 엔진 규약 — Cross(P1 - P0, P2 - P0)가 앞면(바깥) 노멀. 위(+Z)를 향한 면만 걸을 수 있다.
struct FNavMeshBuildInput
{
	std::vector<FVector3> Vertices;
	std::vector<uint32>   Indices; // 삼각형당 3개
};

enum class ENavPathResult : uint8
{
	Failed,   // 시작/끝을 메시에 투영하지 못했거나 경로 없음
	Partial,  // 목표에 닿지 못해 가장 가까운 곳까지의 경로
	Complete, // 목표까지 이어진 경로
};

// Recast로 구운 단일 타일(Solo) 내비메시 + Detour 경로 쿼리. 입출력은 모두 엔진 좌표(cm, Z-up).
// Recast/Detour 객체는 구현(.cpp)에만 있다 (공개 헤더에 Recast 헤더를 노출하지 않음).
// 이동 가능, 복사 불가. 스레드 안전하지 않다 (쿼리 객체를 공유함).
class FNavMesh
{
public:
	static constexpr const char* FileExtension = ".enav";

	FNavMesh();
	~FNavMesh();

	FNavMesh(FNavMesh&& Other) noexcept;
	FNavMesh& operator=(FNavMesh&& Other) noexcept;

	FNavMesh(const FNavMesh&)            = delete;
	FNavMesh& operator=(const FNavMesh&) = delete;

	// 굽는다. 실패하면 false(+ OutError에 이유)이고 기존 내용은 그대로 둔다
	bool Build(const FNavMeshBuildInput& Input, const FNavMeshBuildSettings& Settings, std::string* OutError = nullptr);

	bool IsValid() const;
	void Reset();

	// 굽기(또는 로드)에 쓰인 설정. 유효하지 않으면 기본값
	const FNavMeshBuildSettings& GetSettings() const;

	// 시작/끝을 메시에 투영한 뒤 경로를 찾아 직선화된 경로점(시작·끝 포함)을 돌려준다.
	// Partial이면 OutPoints의 끝은 목표에 가장 가까운 도달 가능 지점. Failed면 OutPoints는 비어 있다
	ENavPathResult FindPath(const FVector3& Start, const FVector3& End, std::vector<FVector3>& OutPoints) const;

	// 검색 범위(GetQueryExtents) 안에서 가장 가까운 메시 위 점. 없으면 false
	bool ProjectPoint(const FVector3& Point, FVector3& OutPoint) const;

	// 점을 메시에 투영할 때의 검색 반경 (엔진 축별 반 크기, cm). 설정의 에이전트 반경/키에서 얻는다
	FVector3 GetQueryExtents() const;

	// 디버그 표시용 삼각형 목록 (엔진 좌표, 3개씩, 엔진 와인딩 = 위를 향함). 디테일 메시 기준
	void GetDebugTriangles(std::vector<FVector3>& OutTriangles) const;

	int32 GetPolygonCount() const;

	// 직렬화: 자체 헤더(매직 + 버전 + 설정) + Detour 타일 데이터 + 해시.
	// 로드 실패(손상/버전 불일치) 시 false이고 기존 내용은 그대로 둔다
	std::vector<uint8> SaveToBytes() const;
	bool               LoadFromBytes(const uint8* Data, size_t Size, std::string* OutError = nullptr);
	bool               LoadFromBytes(const std::vector<uint8>& Bytes, std::string* OutError = nullptr);

	bool SaveToFile(const std::filesystem::path& Path) const;
	bool LoadFromFile(const std::filesystem::path& Path, std::string* OutError = nullptr);

private:
	struct FImpl;
	std::unique_ptr<FImpl> Impl;
};
