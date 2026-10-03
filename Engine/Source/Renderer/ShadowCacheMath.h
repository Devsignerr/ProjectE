#pragma once

#include "Core/Math/MathUtils.h"

#include <cstring>

// 방향광 그림자 캐시·LOD 바이어스·작은 캐스터 컬링 규칙 (순수 함수 — RendererTests의 ShadowCacheTests)
//
// 정적 캐스터 캐시 (r.Shadow.Cache):
//  - 캐스케이드마다 정적 캐스터만 그린 깊이를 따로 보관하고, 프레임마다 캐시 → 섀도우 맵 복사 뒤 동적 캐스터만 그린다.
//  - 정적 캐스터 = 스킨이 아니고 월드 행렬이 r.Shadow.Cache.StaticFrames 프레임 연속 같은 인스턴스(FMeshInstance::bShadowStatic),
//    폴리지(배치 고정), 상태 해시를 알려 주는 추가 캐스터(지형 — FShadowRenderer::ExtraCasterState). 그 밖은 모두 동적이다.
//    새 그림자 캐스터 종류는 정적/동적을 반드시 알린다(알리지 않으면 동적 = 매 프레임 그림).
//  - 캐스케이드 키 = 해시(캐스케이드 뷰-투영 비트, 해상도·바이어스·LOD 바이어스·작은 캐스터 문턱, 정적 인스턴스 집합 해시,
//    추가 캐스터 상태 해시, 캐시 세대(셰이더 다시 로드·명시 무효화)). 키가 바뀌면(태양 이동, 캐스케이드 스냅 이동, 정적 캐스터
//    추가/삭제/이동/LOD·머티리얼 변경, 리소스 다시 로드) 캐시는 무효다.
//  - Decide: 키가 지난 프레임과 같고 캐시 키와도 같으면 Reuse, 지난 프레임과는 같지만 캐시가 없거나 낡았으면 Rebuild
//    (정적을 캐시에 그림 → 복사 → 동적), 지난 프레임과 다르면 Direct(캐시 없이 전부 그림 — 태양이 매 프레임 움직이는 시간대 애니메이션은
//    복사 비용 없이 예전과 같다).
//
// LOD 바이어스 (r.Shadow.LodBias): 캐스케이드 c의 그림자 LOD = min(메인 LOD + floor(c × 바이어스), LOD 수 - 1). bFixedLod/스킨은 그대로.
// 작은 캐스터 (r.Shadow.MinCasterTexels): 경계 구 지름이 그 캐스케이드 텍셀 몇 개보다 작으면 그 캐스케이드에서 그리지 않는다.
namespace ShadowCacheMath
{
	enum class ECacheAction : uint8
	{
		Direct,  // 캐시 없이 모든 캐스터를 섀도우 맵에 그린다
		Rebuild, // 정적 캐스터를 캐시에 다시 그리고 복사한 뒤 동적 캐스터
		Reuse,   // 캐시 복사 뒤 동적 캐스터만
	};

	struct FCascadeCacheState
	{
		uint64 LastKey     = 0;
		uint64 CachedKey   = 0;
		bool   bHasLast    = false;
		bool   bCacheValid = false;
	};

	// 이번 프레임 키로 행동을 고르고 상태를 갱신한다
	inline ECacheAction Decide(FCascadeCacheState& State, uint64 Key, bool bEnabled)
	{
		ECacheAction Action = ECacheAction::Direct;
		if (!bEnabled)
		{
			State.bCacheValid = false;
		}
		else if (State.bCacheValid && State.CachedKey == Key)
		{
			Action = ECacheAction::Reuse;
		}
		else if (State.bHasLast && State.LastKey == Key)
		{
			Action            = ECacheAction::Rebuild;
			State.CachedKey   = Key;
			State.bCacheValid = true;
		}
		else
		{
			State.bCacheValid = false; // 한 프레임 안정될 때까지 캐시를 만들지 않는다
		}
		State.LastKey  = Key;
		State.bHasLast = bEnabled;
		return Action;
	}

	// 캐스케이드 c의 LOD 바이어스 = floor(c × BiasPerCascade)
	inline uint32 ComputeCascadeLodBias(uint32 Cascade, float BiasPerCascade)
	{
		return BiasPerCascade > 0.0f ? static_cast<uint32>(FMath::Floor(static_cast<float>(Cascade) * BiasPerCascade + 1.0e-4f)) : 0u;
	}

	inline uint32 SelectShadowLod(uint32 MainLod, uint32 LodCount, uint32 CascadeBias, bool bKeepLod)
	{
		if (bKeepLod || LodCount <= 1)
		{
			return MainLod;
		}
		return FMath::Min(MainLod + CascadeBias, LodCount - 1);
	}

	// 경계 구 지름(2R)이 텍셀 MinTexels개보다 작으면 true (MinTexels <= 0 = 끔)
	inline bool IsCasterTooSmall(float BoundsRadius, float TexelWorldSize, float MinTexels)
	{
		return MinTexels > 0.0f && TexelWorldSize > 0.0f && 2.0f * BoundsRadius < MinTexels * TexelWorldSize;
	}

	// 위치가 Required 프레임 연속 같았으면 정적
	inline bool IsStatic(uint32 StableFrames, uint32 RequiredFrames)
	{
		return StableFrames >= RequiredFrames;
	}

	// ---- 해시 (곱셈 섞기). 정적 집합 해시는 인스턴스 해시의 합(순서 무관)
	constexpr uint64 HashSeed = 1469598103934665603ull;

	// 8바이트 단위로 섞는다 (남는 바이트는 하나씩). 같은 입력 → 같은 값 (실행·플랫폼 내 결정적)
	inline uint64 HashBytes(uint64 Hash, const void* Data, size_t Size)
	{
		const uint8* Bytes = static_cast<const uint8*>(Data);
		while (Size >= 8)
		{
			uint64 Word = 0;
			std::memcpy(&Word, Bytes, 8);
			Hash = (Hash ^ Word) * 0x9E3779B97F4A7C15ull;
			Hash ^= Hash >> 29;
			Bytes += 8;
			Size -= 8;
		}
		while (Size > 0)
		{
			Hash = (Hash ^ *Bytes) * 1099511628211ull;
			++Bytes;
			--Size;
		}
		return Hash;
	}

	template <typename T>
	inline uint64 HashValue(uint64 Hash, const T& Value)
	{
		return HashBytes(Hash, &Value, sizeof(T));
	}

	// 합으로 모을 인스턴스 해시가 고르게 퍼지도록 마지막에 섞는다 (splitmix64 마무리)
	inline uint64 Finalize(uint64 Hash)
	{
		Hash ^= Hash >> 30;
		Hash *= 0xbf58476d1ce4e5b9ull;
		Hash ^= Hash >> 27;
		Hash *= 0x94d049bb133111ebull;
		Hash ^= Hash >> 31;
		return Hash;
	}
} // namespace ShadowCacheMath
