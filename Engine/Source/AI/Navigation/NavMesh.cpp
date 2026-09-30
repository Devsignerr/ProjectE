#include "AI/Navigation/NavMesh.h"

#include "AI/AIModule.h"
#include "AI/Navigation/NavCoordinates.h"
#include "Core/Serialization/BinaryArchive.h"
#include "Core/StringConv.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <string_view>
#include <type_traits>

#pragma warning(push, 0)
#include <DetourAlloc.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>
#include <DetourStatus.h>
#include <Recast.h>
#pragma warning(pop)

namespace
{
	// .enav 파일 헤더
	constexpr uint32 NavFileMagic   = 0x56414E45u; // "ENAV" (리틀 엔디언)
	constexpr uint32 NavFileVersion = 1;

	// 한 번에 다루는 격자 셀 상한 (Solo 메시). 넘으면 타일링이 필요하다
	constexpr int64 MaxGridCells = 4096ll * 4096ll;

	constexpr int32 MaxQueryNodes   = 2048;
	constexpr int32 MaxPathPolys    = 512;
	constexpr int32 MaxStraightPath = 512;

	// Detour 폴리곤 플래그/영역 (단계 4 이후 영역 종류가 늘면 확장)
	constexpr unsigned short NavPolyFlagWalk = 0x01;
	constexpr unsigned char  NavAreaGround   = 0;

	static_assert(std::is_trivially_copyable_v<FNavMeshBuildSettings>, "FNavMeshBuildSettings는 그대로 직렬화된다");

	// Recast/Detour 할당 함수 쌍을 쓰는 unique_ptr 삭제자
	template <auto FreeFunction>
	struct TRecastDeleter
	{
		template <typename T>
		void operator()(T* Pointer) const
		{
			FreeFunction(Pointer);
		}
	};

	using FHeightfieldPtr    = std::unique_ptr<rcHeightfield, TRecastDeleter<&rcFreeHeightField>>;
	using FCompactPtr        = std::unique_ptr<rcCompactHeightfield, TRecastDeleter<&rcFreeCompactHeightfield>>;
	using FContourSetPtr     = std::unique_ptr<rcContourSet, TRecastDeleter<&rcFreeContourSet>>;
	using FPolyMeshPtr       = std::unique_ptr<rcPolyMesh, TRecastDeleter<&rcFreePolyMesh>>;
	using FPolyMeshDetailPtr = std::unique_ptr<rcPolyMeshDetail, TRecastDeleter<&rcFreePolyMeshDetail>>;
	using FDetourNavMeshPtr  = std::unique_ptr<dtNavMesh, TRecastDeleter<&dtFreeNavMesh>>;
	using FDetourQueryPtr    = std::unique_ptr<dtNavMeshQuery, TRecastDeleter<&dtFreeNavMeshQuery>>;

	// Recast 로그를 엔진 로그로 넘긴다 (오류만)
	class FRecastContext : public rcContext
	{
	public:
		FRecastContext() : rcContext(true) {}

	protected:
		void doLog(const rcLogCategory Category, const char* Message, const int Length) override
		{
			if (Category == RC_LOG_ERROR)
			{
				E_LOG(LogAI, Warning, "Recast: {}", std::string_view(Message, static_cast<size_t>(std::max(Length, 0))));
			}
		}
	};

	bool Fail(std::string* OutError, std::string Message)
	{
		E_LOG(LogAI, Warning, "내비메시: {}", Message);
		if (OutError)
		{
			*OutError = std::move(Message);
		}
		return false;
	}

	bool ValidateSettings(const FNavMeshBuildSettings& Settings, std::string* OutError)
	{
		if (!(Settings.CellSize > 0.0f) || !(Settings.CellHeight > 0.0f))
		{
			return Fail(OutError, "CellSize/CellHeight는 0보다 커야 합니다");
		}
		if (!(Settings.AgentHeight > 0.0f) || Settings.AgentRadius < 0.0f || Settings.AgentMaxClimb < 0.0f)
		{
			return Fail(OutError, "에이전트 크기가 잘못되었습니다");
		}
		if (!(Settings.AgentMaxSlopeDegrees >= 0.0f && Settings.AgentMaxSlopeDegrees < 90.0f))
		{
			return Fail(OutError, "AgentMaxSlopeDegrees는 0 이상 90 미만이어야 합니다");
		}
		if (Settings.VertsPerPoly < 3 || Settings.VertsPerPoly > DT_VERTS_PER_POLYGON)
		{
			return Fail(OutError, std::format("VertsPerPoly는 3~{}이어야 합니다", DT_VERTS_PER_POLYGON));
		}
		if (Settings.RegionMinSize < 0 || Settings.RegionMergeSize < 0 || Settings.EdgeMaxLength < 0.0f ||
		    Settings.EdgeMaxError < 0.0f || Settings.DetailSampleDistance < 0.0f || Settings.DetailSampleMaxError < 0.0f)
		{
			return Fail(OutError, "영역/윤곽/디테일 설정은 음수일 수 없습니다");
		}
		return true;
	}

	// FNV-1a 64비트 (타일 데이터 손상 검출용)
	uint64 HashBytes(const std::vector<uint8>& Bytes)
	{
		uint64 Hash = 14695981039346656037ull;
		for (const uint8 Byte : Bytes)
		{
			Hash ^= Byte;
			Hash *= 1099511628211ull;
		}
		return Hash;
	}

	// dtNavMesh::addTile은 크기를 검사하지 않으므로 헤더의 개수로 계산한 크기와 실제 크기를 먼저 맞춰 본다
	bool ValidateTileData(const std::vector<uint8>& TileData, std::string* OutError)
	{
		if (TileData.size() < sizeof(dtMeshHeader))
		{
			return Fail(OutError, "Detour 타일 데이터가 너무 작습니다");
		}
		dtMeshHeader Header;
		std::memcpy(&Header, TileData.data(), sizeof(Header));
		if (Header.magic != DT_NAVMESH_MAGIC || Header.version != DT_NAVMESH_VERSION)
		{
			return Fail(OutError, "Detour 타일 매직/버전이 맞지 않습니다");
		}
		if (Header.polyCount <= 0 || Header.vertCount <= 0 || Header.maxLinkCount <= 0 || Header.detailMeshCount < 0 ||
		    Header.detailVertCount < 0 || Header.detailTriCount < 0 || Header.bvNodeCount < 0 || Header.offMeshConCount < 0)
		{
			return Fail(OutError, "Detour 타일 헤더의 개수가 잘못되었습니다");
		}

		const auto Align4 = [](int64 Value) { return (Value + 3) & ~int64(3); };
		const int64 ExpectedSize = Align4(sizeof(dtMeshHeader)) +
		                           Align4(int64(sizeof(float)) * 3 * Header.vertCount) +
		                           Align4(int64(sizeof(dtPoly)) * Header.polyCount) +
		                           Align4(int64(sizeof(dtLink)) * Header.maxLinkCount) +
		                           Align4(int64(sizeof(dtPolyDetail)) * Header.detailMeshCount) +
		                           Align4(int64(sizeof(float)) * 3 * Header.detailVertCount) +
		                           Align4(int64(4) * Header.detailTriCount) +
		                           Align4(int64(sizeof(dtBVNode)) * Header.bvNodeCount) +
		                           Align4(int64(sizeof(dtOffMeshConnection)) * Header.offMeshConCount);
		if (ExpectedSize != static_cast<int64>(TileData.size()))
		{
			return Fail(OutError, "Detour 타일 데이터 크기가 헤더와 맞지 않습니다");
		}
		return true;
	}

	void ExtentsToRecast(const FVector3& EngineHalfExtents, float* Out)
	{
		// 크기라 부호 없이 축만 바꾼다 (x = X, y = Z, z = Y)
		FNavCoordinates::ToRecast(EngineHalfExtents, Out);
	}
}

struct FNavMesh::FImpl
{
	FNavMeshBuildSettings Settings;
	std::vector<uint8>    TileData; // 직렬화용 원본 Detour 타일 데이터 (dtNavMesh는 자기 사본을 고쳐 쓴다)
	FDetourNavMeshPtr     NavMesh;
	FDetourQueryPtr       Query;
	dtQueryFilter         Filter;

	// 타일 데이터로 dtNavMesh + 쿼리를 만든다. 실패 시 nullptr
	static std::unique_ptr<FImpl> Create(std::vector<uint8> TileData, const FNavMeshBuildSettings& Settings, std::string* OutError)
	{
		if (!ValidateTileData(TileData, OutError))
		{
			return nullptr;
		}

		auto NewImpl      = std::make_unique<FImpl>();
		NewImpl->Settings = Settings;
		NewImpl->TileData = std::move(TileData);
		NewImpl->Filter.setIncludeFlags(NavPolyFlagWalk);
		NewImpl->Filter.setExcludeFlags(0);

		NewImpl->NavMesh.reset(dtAllocNavMesh());
		if (!NewImpl->NavMesh)
		{
			Fail(OutError, "dtNavMesh 할당 실패");
			return nullptr;
		}

		// dtNavMesh가 DT_TILE_FREE_DATA로 소유할 사본 (dtAlloc으로 할당해야 dtFree로 해제된다)
		const int32    DataSize = static_cast<int32>(NewImpl->TileData.size());
		unsigned char* Data     = static_cast<unsigned char*>(dtAlloc(static_cast<size_t>(DataSize), DT_ALLOC_PERM));
		if (!Data)
		{
			Fail(OutError, "Detour 타일 메모리 할당 실패");
			return nullptr;
		}
		std::memcpy(Data, NewImpl->TileData.data(), static_cast<size_t>(DataSize));

		const dtStatus InitStatus = NewImpl->NavMesh->init(Data, DataSize, DT_TILE_FREE_DATA);
		if (dtStatusFailed(InitStatus))
		{
			dtFree(Data); // 실패하면 소유권이 넘어가지 않는다
			Fail(OutError, std::format("dtNavMesh 초기화 실패 (상태 0x{:08X})", InitStatus));
			return nullptr;
		}

		NewImpl->Query.reset(dtAllocNavMeshQuery());
		if (!NewImpl->Query || dtStatusFailed(NewImpl->Query->init(NewImpl->NavMesh.get(), MaxQueryNodes)))
		{
			Fail(OutError, "dtNavMeshQuery 초기화 실패");
			return nullptr;
		}
		return NewImpl;
	}
};

FNavMesh::FNavMesh() = default;
FNavMesh::~FNavMesh() = default;
FNavMesh::FNavMesh(FNavMesh&& Other) noexcept = default;
FNavMesh& FNavMesh::operator=(FNavMesh&& Other) noexcept = default;

bool FNavMesh::Build(const FNavMeshBuildInput& Input, const FNavMeshBuildSettings& Settings, std::string* OutError)
{
	if (!ValidateSettings(Settings, OutError))
	{
		return false;
	}
	if (Input.Vertices.empty() || Input.Indices.empty())
	{
		return Fail(OutError, "입력 지오메트리가 비어 있습니다");
	}
	if (Input.Indices.size() % 3 != 0)
	{
		return Fail(OutError, "인덱스 수가 3의 배수가 아닙니다");
	}
	if (Input.Vertices.size() > static_cast<size_t>(INT32_MAX / 3) || Input.Indices.size() > static_cast<size_t>(INT32_MAX))
	{
		return Fail(OutError, "입력 지오메트리가 너무 큽니다");
	}

	// 엔진 → Recast 좌표, 와인딩 반전 (NavCoordinates.h 머리 주석)
	const int32        VertexCount = static_cast<int32>(Input.Vertices.size());
	const int32        TriCount    = static_cast<int32>(Input.Indices.size() / 3);
	std::vector<float> Verts(static_cast<size_t>(VertexCount) * 3);
	for (int32 Index = 0; Index < VertexCount; ++Index)
	{
		FNavCoordinates::ToRecast(Input.Vertices[static_cast<size_t>(Index)], &Verts[static_cast<size_t>(Index) * 3]);
	}
	std::vector<int32> Tris(Input.Indices.size());
	for (size_t Index = 0; Index < Input.Indices.size(); ++Index)
	{
		if (Input.Indices[Index] >= Input.Vertices.size())
		{
			return Fail(OutError, std::format("인덱스 {}가 정점 범위를 벗어났습니다", Input.Indices[Index]));
		}
		Tris[Index] = static_cast<int32>(Input.Indices[Index]);
	}
	for (int32 Tri = 0; Tri < TriCount; ++Tri)
	{
		int32* T = &Tris[static_cast<size_t>(Tri) * 3];
		FNavCoordinates::FlipWinding(T[0], T[1], T[2]);
	}

	// 설정: 엔진 cm → Recast m / 복셀 단위
	rcConfig Config;
	std::memset(&Config, 0, sizeof(Config));
	Config.cs                     = FNavCoordinates::ToMeters(Settings.CellSize);
	Config.ch                     = FNavCoordinates::ToMeters(Settings.CellHeight);
	Config.walkableSlopeAngle     = Settings.AgentMaxSlopeDegrees;
	Config.walkableHeight         = static_cast<int32>(std::ceil(Settings.AgentHeight / Settings.CellHeight));
	Config.walkableClimb          = static_cast<int32>(std::floor(Settings.AgentMaxClimb / Settings.CellHeight));
	Config.walkableRadius         = static_cast<int32>(std::ceil(Settings.AgentRadius / Settings.CellSize));
	Config.maxEdgeLen             = static_cast<int32>(Settings.EdgeMaxLength / Settings.CellSize);
	Config.maxSimplificationError = Settings.EdgeMaxError / Settings.CellSize;
	Config.minRegionArea          = Settings.RegionMinSize * Settings.RegionMinSize;
	Config.mergeRegionArea        = Settings.RegionMergeSize * Settings.RegionMergeSize;
	Config.maxVertsPerPoly        = Settings.VertsPerPoly;
	Config.detailSampleDist       = Settings.DetailSampleDistance < Settings.CellSize * 0.9f ? 0.0f : FNavCoordinates::ToMeters(Settings.DetailSampleDistance);
	Config.detailSampleMaxError   = FNavCoordinates::ToMeters(Settings.DetailSampleMaxError);

	rcCalcBounds(Verts.data(), VertexCount, Config.bmin, Config.bmax);
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		if (!std::isfinite(Config.bmin[Axis]) || !std::isfinite(Config.bmax[Axis]))
		{
			return Fail(OutError, "입력 정점에 유한하지 않은 값이 있습니다");
		}
	}
	rcCalcGridSize(Config.bmin, Config.bmax, Config.cs, &Config.width, &Config.height);
	if (Config.width <= 0 || Config.height <= 0 || static_cast<int64>(Config.width) * Config.height > MaxGridCells)
	{
		return Fail(OutError, std::format("격자 크기 {}×{}가 범위를 벗어났습니다 (큰 월드는 타일링 필요)", Config.width, Config.height));
	}

	FRecastContext Context;

	// 1. 높이장 + 걸을 수 있는 면 표시 + 래스터화
	FHeightfieldPtr Solid(rcAllocHeightfield());
	if (!Solid || !rcCreateHeightfield(&Context, *Solid, Config.width, Config.height, Config.bmin, Config.bmax, Config.cs, Config.ch))
	{
		return Fail(OutError, "높이장 생성 실패");
	}
	std::vector<unsigned char> TriAreas(static_cast<size_t>(TriCount), 0);
	rcMarkWalkableTriangles(&Context, Config.walkableSlopeAngle, Verts.data(), VertexCount, Tris.data(), TriCount, TriAreas.data());
	if (!rcRasterizeTriangles(&Context, Verts.data(), VertexCount, Tris.data(), TriAreas.data(), TriCount, *Solid, Config.walkableClimb))
	{
		return Fail(OutError, "삼각형 래스터화 실패");
	}

	// 2. 필터 (낮은 장애물 넘기, 낭떠러지 가장자리, 천장 높이)
	rcFilterLowHangingWalkableObstacles(&Context, Config.walkableClimb, *Solid);
	rcFilterLedgeSpans(&Context, Config.walkableHeight, Config.walkableClimb, *Solid);
	rcFilterWalkableLowHeightSpans(&Context, Config.walkableHeight, *Solid);

	// 3. 압축 + 에이전트 반경만큼 침식
	FCompactPtr Compact(rcAllocCompactHeightfield());
	if (!Compact || !rcBuildCompactHeightfield(&Context, Config.walkableHeight, Config.walkableClimb, *Solid, *Compact))
	{
		return Fail(OutError, "압축 높이장 생성 실패");
	}
	Solid.reset();
	if (!rcErodeWalkableArea(&Context, Config.walkableRadius, *Compact))
	{
		return Fail(OutError, "침식 실패");
	}

	// 4. 거리장 + 영역 (Watershed)
	if (!rcBuildDistanceField(&Context, *Compact) ||
	    !rcBuildRegions(&Context, *Compact, 0, Config.minRegionArea, Config.mergeRegionArea))
	{
		return Fail(OutError, "영역 생성 실패");
	}

	// 5. 윤곽 → 폴리 메시 → 디테일 메시
	FContourSetPtr Contours(rcAllocContourSet());
	if (!Contours || !rcBuildContours(&Context, *Compact, Config.maxSimplificationError, Config.maxEdgeLen, *Contours))
	{
		return Fail(OutError, "윤곽 생성 실패");
	}
	if (Contours->nconts == 0)
	{
		return Fail(OutError, "걸을 수 있는 면이 없습니다 (와인딩/경사/에이전트 크기 확인)");
	}
	FPolyMeshPtr PolyMesh(rcAllocPolyMesh());
	if (!PolyMesh || !rcBuildPolyMesh(&Context, *Contours, Config.maxVertsPerPoly, *PolyMesh))
	{
		return Fail(OutError, "폴리 메시 생성 실패");
	}
	FPolyMeshDetailPtr DetailMesh(rcAllocPolyMeshDetail());
	if (!DetailMesh || !rcBuildPolyMeshDetail(&Context, *PolyMesh, *Compact, Config.detailSampleDist, Config.detailSampleMaxError, *DetailMesh))
	{
		return Fail(OutError, "디테일 메시 생성 실패");
	}
	if (PolyMesh->npolys <= 0)
	{
		return Fail(OutError, "걸을 수 있는 폴리곤이 없습니다");
	}
	if (PolyMesh->nverts >= 0xffff)
	{
		return Fail(OutError, "폴리 메시 정점이 너무 많습니다 (타일링 필요)");
	}

	// 6. Detour 데이터
	std::vector<unsigned short> PolyFlags(static_cast<size_t>(PolyMesh->npolys), 0);
	for (int32 Poly = 0; Poly < PolyMesh->npolys; ++Poly)
	{
		if (PolyMesh->areas[Poly] == RC_WALKABLE_AREA)
		{
			PolyMesh->areas[Poly]                = NavAreaGround;
			PolyFlags[static_cast<size_t>(Poly)] = NavPolyFlagWalk;
		}
	}

	dtNavMeshCreateParams Params;
	std::memset(&Params, 0, sizeof(Params));
	Params.verts            = PolyMesh->verts;
	Params.vertCount        = PolyMesh->nverts;
	Params.polys            = PolyMesh->polys;
	Params.polyAreas        = PolyMesh->areas;
	Params.polyFlags        = PolyFlags.data();
	Params.polyCount        = PolyMesh->npolys;
	Params.nvp              = PolyMesh->nvp;
	Params.detailMeshes     = DetailMesh->meshes;
	Params.detailVerts      = DetailMesh->verts;
	Params.detailVertsCount = DetailMesh->nverts;
	Params.detailTris       = DetailMesh->tris;
	Params.detailTriCount   = DetailMesh->ntris;
	Params.walkableHeight   = FNavCoordinates::ToMeters(Settings.AgentHeight);
	Params.walkableRadius   = FNavCoordinates::ToMeters(Settings.AgentRadius);
	Params.walkableClimb    = FNavCoordinates::ToMeters(Settings.AgentMaxClimb);
	rcVcopy(Params.bmin, PolyMesh->bmin);
	rcVcopy(Params.bmax, PolyMesh->bmax);
	Params.cs          = Config.cs;
	Params.ch          = Config.ch;
	Params.buildBvTree = true;

	unsigned char* NavData     = nullptr;
	int32          NavDataSize = 0;
	if (!dtCreateNavMeshData(&Params, &NavData, &NavDataSize) || !NavData || NavDataSize <= 0)
	{
		dtFree(NavData);
		return Fail(OutError, "Detour 내비메시 데이터 생성 실패");
	}
	std::vector<uint8> TileData(NavData, NavData + NavDataSize);
	dtFree(NavData);

	std::unique_ptr<FImpl> NewImpl = FImpl::Create(std::move(TileData), Settings, OutError);
	if (!NewImpl)
	{
		return false;
	}
	Impl = std::move(NewImpl);

	E_LOG(LogAI, Log, "내비메시 굽기 완료: 삼각형 {}개 → 폴리곤 {}개 (격자 {}×{})", TriCount, PolyMesh->npolys, Config.width, Config.height);
	return true;
}

bool FNavMesh::IsValid() const
{
	return Impl && Impl->NavMesh && Impl->Query;
}

void FNavMesh::Reset()
{
	Impl.reset();
}

const FNavMeshBuildSettings& FNavMesh::GetSettings() const
{
	static const FNavMeshBuildSettings DefaultSettings;
	return Impl ? Impl->Settings : DefaultSettings;
}

FVector3 FNavMesh::GetQueryExtents() const
{
	const FNavMeshBuildSettings& Settings   = GetSettings();
	const float                  Horizontal = std::max(Settings.AgentRadius * 2.0f, Settings.CellSize * 2.0f);
	return FVector3(Horizontal, Horizontal, Settings.AgentHeight);
}

ENavPathResult FNavMesh::FindPath(const FVector3& Start, const FVector3& End, std::vector<FVector3>& OutPoints) const
{
	OutPoints.clear();
	if (!IsValid())
	{
		return ENavPathResult::Failed;
	}

	float StartPos[3];
	float EndPos[3];
	float Extents[3];
	FNavCoordinates::ToRecast(Start, StartPos);
	FNavCoordinates::ToRecast(End, EndPos);
	ExtentsToRecast(GetQueryExtents(), Extents);

	const dtNavMeshQuery& Query     = *Impl->Query;
	dtPolyRef             StartRef  = 0;
	dtPolyRef             EndRef    = 0;
	float                 StartNear[3] = {};
	float                 EndNear[3]   = {};
	if (dtStatusFailed(Query.findNearestPoly(StartPos, Extents, &Impl->Filter, &StartRef, StartNear)) || StartRef == 0 ||
	    dtStatusFailed(Query.findNearestPoly(EndPos, Extents, &Impl->Filter, &EndRef, EndNear)) || EndRef == 0)
	{
		return ENavPathResult::Failed;
	}

	std::array<dtPolyRef, MaxPathPolys> Polys{};
	int32                               PolyCount  = 0;
	const dtStatus                      PathStatus = Query.findPath(StartRef, EndRef, StartNear, EndNear, &Impl->Filter, Polys.data(), &PolyCount, MaxPathPolys);
	if (dtStatusFailed(PathStatus) || PolyCount <= 0)
	{
		return ENavPathResult::Failed;
	}

	const dtPolyRef LastRef  = Polys[static_cast<size_t>(PolyCount - 1)];
	const bool      bPartial = LastRef != EndRef || dtStatusDetail(PathStatus, DT_PARTIAL_RESULT) || dtStatusDetail(PathStatus, DT_BUFFER_TOO_SMALL);

	// 목표에 닿지 못했으면 마지막 폴리곤 위에서 목표에 가장 가까운 점까지
	float Target[3] = { EndNear[0], EndNear[1], EndNear[2] };
	if (LastRef != EndRef)
	{
		bool bOverPoly = false;
		if (dtStatusFailed(Query.closestPointOnPoly(LastRef, EndNear, Target, &bOverPoly)))
		{
			return ENavPathResult::Failed;
		}
	}

	std::array<float, MaxStraightPath * 3>       Straight{};
	std::array<unsigned char, MaxStraightPath>   StraightFlags{};
	std::array<dtPolyRef, MaxStraightPath>       StraightRefs{};
	int32                                        StraightCount  = 0;
	const dtStatus                               StraightStatus = Query.findStraightPath(StartNear, Target, Polys.data(), PolyCount,
	                                                                                     Straight.data(), StraightFlags.data(), StraightRefs.data(),
	                                                                                     &StraightCount, MaxStraightPath);
	if (dtStatusFailed(StraightStatus) || StraightCount <= 0)
	{
		return ENavPathResult::Failed;
	}

	OutPoints.reserve(static_cast<size_t>(StraightCount));
	for (int32 Index = 0; Index < StraightCount; ++Index)
	{
		OutPoints.push_back(FNavCoordinates::FromRecast(&Straight[static_cast<size_t>(Index) * 3]));
	}
	return bPartial ? ENavPathResult::Partial : ENavPathResult::Complete;
}

bool FNavMesh::ProjectPoint(const FVector3& Point, FVector3& OutPoint) const
{
	if (!IsValid())
	{
		return false;
	}
	float Center[3];
	float Extents[3];
	FNavCoordinates::ToRecast(Point, Center);
	ExtentsToRecast(GetQueryExtents(), Extents);

	dtPolyRef Ref        = 0;
	float     Nearest[3] = {};
	if (dtStatusFailed(Impl->Query->findNearestPoly(Center, Extents, &Impl->Filter, &Ref, Nearest)) || Ref == 0)
	{
		return false;
	}
	OutPoint = FNavCoordinates::FromRecast(Nearest);
	return true;
}

void FNavMesh::GetDebugTriangles(std::vector<FVector3>& OutTriangles) const
{
	OutTriangles.clear();
	if (!IsValid())
	{
		return;
	}
	const dtNavMesh& NavMesh = *Impl->NavMesh;
	for (int32 TileIndex = 0; TileIndex < NavMesh.getMaxTiles(); ++TileIndex)
	{
		const dtMeshTile* Tile = NavMesh.getTile(TileIndex);
		if (!Tile || !Tile->header)
		{
			continue;
		}
		for (int32 PolyIndex = 0; PolyIndex < Tile->header->polyCount; ++PolyIndex)
		{
			const dtPoly& Poly = Tile->polys[PolyIndex];
			if (Poly.getType() == DT_POLYTYPE_OFFMESH_CONNECTION)
			{
				continue;
			}
			const dtPolyDetail& Detail = Tile->detailMeshes[PolyIndex];
			for (uint32 TriIndex = 0; TriIndex < Detail.triCount; ++TriIndex)
			{
				const unsigned char* Tri = &Tile->detailTris[(Detail.triBase + TriIndex) * 4];
				FVector3             Corners[3];
				for (int32 Corner = 0; Corner < 3; ++Corner)
				{
					const float* Vertex = Tri[Corner] < Poly.vertCount
					                          ? &Tile->verts[Poly.verts[Tri[Corner]] * 3]
					                          : &Tile->detailVerts[(Detail.vertBase + Tri[Corner] - Poly.vertCount) * 3];
					Corners[Corner] = FNavCoordinates::FromRecast(Vertex);
				}
				// Recast 와인딩 → 엔진 와인딩
				FNavCoordinates::FlipWinding(Corners[0], Corners[1], Corners[2]);
				OutTriangles.push_back(Corners[0]);
				OutTriangles.push_back(Corners[1]);
				OutTriangles.push_back(Corners[2]);
			}
		}
	}
}

int32 FNavMesh::GetPolygonCount() const
{
	if (!IsValid())
	{
		return 0;
	}
	int32            Count   = 0;
	const dtNavMesh& NavMesh = *Impl->NavMesh;
	for (int32 TileIndex = 0; TileIndex < NavMesh.getMaxTiles(); ++TileIndex)
	{
		const dtMeshTile* Tile = NavMesh.getTile(TileIndex);
		if (Tile && Tile->header)
		{
			Count += Tile->header->polyCount;
		}
	}
	return Count;
}

std::vector<uint8> FNavMesh::SaveToBytes() const
{
	if (!IsValid())
	{
		return {};
	}
	FBinaryWriter Writer;
	Writer.Write(NavFileMagic);
	Writer.Write(NavFileVersion);
	Writer.Write(static_cast<uint32>(sizeof(FNavMeshBuildSettings)));
	Writer.Write(Impl->Settings);
	Writer.WriteArray(Impl->TileData);
	Writer.Write(HashBytes(Impl->TileData));
	return Writer.GetBuffer();
}

bool FNavMesh::LoadFromBytes(const uint8* Data, size_t Size, std::string* OutError)
{
	if (!Data || Size == 0)
	{
		return Fail(OutError, "데이터가 비어 있습니다");
	}
	FBinaryReader Reader(Data, Size);
	if (Reader.Read<uint32>() != NavFileMagic || !Reader.IsOk())
	{
		return Fail(OutError, "내비메시 파일이 아닙니다 (매직 불일치)");
	}
	const uint32 Version = Reader.Read<uint32>();
	if (Version != NavFileVersion)
	{
		return Fail(OutError, std::format("지원하지 않는 내비메시 버전 {} (현재 {})", Version, NavFileVersion));
	}
	if (Reader.Read<uint32>() != sizeof(FNavMeshBuildSettings))
	{
		return Fail(OutError, "설정 블록 크기가 맞지 않습니다");
	}
	const FNavMeshBuildSettings Settings = Reader.Read<FNavMeshBuildSettings>();
	std::vector<uint8>          TileData = Reader.ReadArray<uint8>();
	const uint64                Hash     = Reader.Read<uint64>();
	if (!Reader.IsOk() || !Reader.IsAtEnd())
	{
		return Fail(OutError, "내비메시 데이터가 잘렸거나 손상되었습니다");
	}
	if (Hash != HashBytes(TileData))
	{
		return Fail(OutError, "내비메시 타일 데이터 해시가 맞지 않습니다 (손상)");
	}
	if (!ValidateSettings(Settings, OutError))
	{
		return false;
	}

	std::unique_ptr<FImpl> NewImpl = FImpl::Create(std::move(TileData), Settings, OutError);
	if (!NewImpl)
	{
		return false;
	}
	Impl = std::move(NewImpl);
	return true;
}

bool FNavMesh::LoadFromBytes(const std::vector<uint8>& Bytes, std::string* OutError)
{
	return LoadFromBytes(Bytes.data(), Bytes.size(), OutError);
}

bool FNavMesh::SaveToFile(const std::filesystem::path& Path) const
{
	if (!IsValid())
	{
		E_LOG(LogAI, Warning, "내비메시: 유효하지 않은 내비메시는 저장할 수 없습니다 ({})", FStringConv::ToUtf8(Path.wstring()));
		return false;
	}
	const std::vector<uint8> Bytes = SaveToBytes();
	FBinaryWriter            Writer;
	Writer.WriteBytes(Bytes.data(), Bytes.size());
	if (!Writer.SaveToFile(Path))
	{
		E_LOG(LogAI, Warning, "내비메시: 저장 실패 {}", FStringConv::ToUtf8(Path.wstring()));
		return false;
	}
	return true;
}

bool FNavMesh::LoadFromFile(const std::filesystem::path& Path, std::string* OutError)
{
	std::vector<uint8> Bytes;
	if (!ReadFileBytes(Path, Bytes))
	{
		return Fail(OutError, std::format("파일을 읽을 수 없습니다: {}", FStringConv::ToUtf8(Path.wstring())));
	}
	return LoadFromBytes(Bytes, OutError);
}
