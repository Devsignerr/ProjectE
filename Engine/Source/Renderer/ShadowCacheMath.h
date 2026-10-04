#pragma once

#include "Core/Math/MathUtils.h"

#include <cstring>

// 방향광 그림자 캐시·LOD 바이어스·작은 캐스터 컬링 규칙 (순수 함수 — RendererTests의 ShadowCacheTests)
//
// 정적 캐스터 캐시 (r.Shadow.Cache):
//  - 캐스케이드마다 정적 캐스터만 그린 깊이를 따로 보관하고, 프레임마다 캐시 → 섀도우 맵 복사 뒤 동적 캐스터만 그린다.
//  - 정적 캐스터 = 스킨이 아니고 월드 행렬이 r.Shadow.Cache.StaticFrames 프레임 연속 같은 인스턴스(FMeshInstance::bShadowStatic),
//    폴리지(배치 고정), 상태 해시를 알려 주는 추가 캐스터(지형 + 정적 2D 스프라이트·타일맵 — FShadowRenderer::ExtraCasterState). 그 밖은 모두 동적이다
//    (움직이는 2D 캐스터는 FShadowRenderer::ExtraDynamicCasters — FSpriteShadowRenderer).
//    새 그림자 캐스터 종류는 정적/동적을 반드시 알린다(알리지 않으면 동적 = 매 프레임 그림).
//  - 캐스케이드 키 = 해시(캐스케이드 뷰-투영 비트, 해상도·바이어스·LOD 바이어스·작은 캐스터 문턱, 정적 인스턴스 집합 해시,
//    추가 캐스터 상태 해시, 캐시 세대(셰이더 다시 로드·명시 무효화·LOD 설정 변경)). 키가 바뀌면(태양 이동, 캐스케이드 이동, 정적 캐스터
//    추가/삭제/이동/머티리얼 변경, 리소스 다시 로드) 캐시는 무효다.
//  - 메인 카메라 LOD(메시·지형 청크)는 키에 넣지 않는다 — 카메라가 움직일 때마다 캐시가 무효가 되지 않게. 캐시는 다시 그린 시점의 LOD로
//    남고 캐스케이드가 바뀌면(이동 중 칸 넘김) 맞춰진다.
//  - 카메라 이동 중 재사용(r.Shadow.Cache.Quantize): 양자화 캐스케이드(QuantizeFirstCascade부터)는 중심을 큰 격자에 맞춰
//    (ShadowMath::ComputeCascade) 칸 안에서는 뷰-투영이 비트 동일 → 칸을 넘는 프레임만 Direct, 다음 프레임 Rebuild.
//  - Decide: 키가 지난 프레임과 같고 캐시 키와도 같으면 Reuse, 지난 프레임과는 같지만 캐시가 없거나 낡았으면 Rebuild
//    (직전 캐시를 ImmediateRebuildReuseFrames 이상 재사용하다 키가 바뀌면 그 프레임에 바로 Rebuild — 이동 중 칸 넘김)
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
		uint32 ReuseFrames = 0; // 지금 캐시를 연속으로 재사용한 프레임 수
		bool   bHasLast    = false;
		bool   bCacheValid = false;
	};

	// 키가 바뀐 프레임에 바로 Rebuild하는 조건: 직전 캐시를 이만큼 재사용했다 (키가 가끔만 바뀜 — 이동 중 칸 넘김).
	// 매 프레임 바뀌는 키(움직이는 태양)는 재사용 0이라 Direct로 떨어진다
	constexpr uint32 ImmediateRebuildReuseFrames = 2;

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
		else if ((State.bHasLast && State.LastKey == Key) || (State.bCacheValid && State.ReuseFrames >= ImmediateRebuildReuseFrames))
		{
			Action            = ECacheAction::Rebuild;
			State.CachedKey   = Key;
			State.bCacheValid = true;
		}
		else
		{
			State.bCacheValid = false; // 한 프레임 안정될 때까지 캐시를 만들지 않는다
		}
		State.ReuseFrames = Action == ECacheAction::Reuse ? State.ReuseFrames + 1 : 0;
		State.LastKey     = Key;
		State.bHasLast    = bEnabled;
		return Action;
	}

	// 섀도우 맵 장 복사 생략: 캐시를 쓰는 캐스케이드(Rebuild/Reuse)가 캐시 위에 아무것도 그리지 않으면(bDrawsOnTop = false — 동적 메시·
	// 동적 추가 캐스터가 그 캐스케이드 프러스텀에 없음) 장은 프레임 끝에 캐시 내용 그대로다("깨끗함"). 지난 프레임 끝에 깨끗했고 캐시 키가 같으면
	// 장은 이미 캐시 내용이므로 이번 복사는 생략한다 (이번에 위에 그리더라도 — 동적 캐스터가 들어온 첫 프레임). 판정은 캐스케이드마다 따로 —
	// 동적 캐스터가 닿지 않는 캐스케이드는 다른 캐스케이드에 동적 캐스터가 있어도 복사하지 않는다
	struct FSliceCopyState
	{
		uint64 Key    = 0;
		bool   bClean = false;
	};
	// 반환 = 이번 프레임 복사 생략
	inline bool UpdateSliceCopy(FSliceCopyState& State, ECacheAction Action, uint64 CachedKey, bool bDrawsOnTop)
	{
		const bool bCleanNow = Action != ECacheAction::Direct && !bDrawsOnTop;
		const bool bSkip     = Action != ECacheAction::Direct && State.bClean && State.Key == CachedKey;
		State.bClean         = bCleanNow;
		State.Key            = CachedKey;
		return bSkip;
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
