#pragma once

#include "Core/CoreTypes.h"
#include "Renderer/TextureCompression.h"

#include <string_view>
#include <vector>

struct FVertex;
struct FMaterialGraph;

// 텍스처 밉 스트리밍 순수 식 (Phase 53 — GPU 없이 테스트: RendererTests TextureStreaming_*)
//
// 필요 밉 (화면 기준, 텍스처마다 이번 프레임 인스턴스들의 최솟값):
//   화면 픽셀 하나가 덮는 월드 크기 CmPerPixel = 2 · 시야 깊이 · tan(세로 시야각/2) / 화면 높이 (직교: 직교 높이 / 화면 높이).
//     시야 깊이는 인스턴스 경계 상자의 가장 가까운 깊이 (원근 투영에서 픽셀 크기는 깊이에 비례 — 거리보다 깊이가 작거나 같으므로 보수적)
//   텍셀/픽셀 = 텍스처 큰 변 × UV 밀도(UV/cm, 월드) × CmPerPixel
//   하드웨어 LOD ≈ log2(텍셀/픽셀) + 밉 바이어스(TAAU MaterialMipBias, 네이티브 0)
//   필요한 최상위 밉 = floor(LOD) - 여유(기본 1밉: 이방성 필터·UV 늘어남·삼각형별 밀도 차이) — 0 아래면 0
//   이방성 필터의 LOD = log2(max(짧은 축, 긴 축 / N))이고 짧은 축 발자국은 정면으로 볼 때의 발자국 이상이므로
//   정면 기준 식은 비스듬한 면에서도 필요한 밉보다 더 세밀한 쪽(보수적)이다
// UV 밀도 (메시마다 한 번, 쿠킹 모델에 저장): 삼각형별 sqrt(UV 면적 / 월드 면적)의 월드 면적 가중 백분위(상위 1% 제외 — 늘어난 조각 삼각형 무시).
//   인스턴스 월드 밀도 = 메시 밀도 / 가장 작은 축 스케일. 0 = 알 수 없음(UV 없음) → 밉 0 필요
// 상주 범위: 새 텍스처의 밉 0이 될 수 있는 밉(유효한 최상위)은 크기가 2^k로 나누어떨어지고(나머지가 있으면 하드웨어 LOD가 달라져
//   전체 밉 텍스처와 같은 값을 읽지 못한다) BC 형식이면 4의 배수인 것 — 0부터 연속이다.
//   큰 변이 NonStreamingMaxSize 이하인 텍스처는 스트리밍하지 않는다(항상 전체). 꼬리(큰 변 MinResidentSize 이하)는 항상 상주
namespace TextureStreamingMath
{
	constexpr uint32 NonStreamingMaxSize = 256; // 이하이면 항상 전체 상주 (BC7 256² 전체 밉 ≈ 85KB — 관리 비용이 더 크다)
	constexpr uint32 MinResidentSize     = 64;  // 꼬리 밉: 큰 변이 이 이하인 밉부터는 항상 상주 (BC7 64² ≈ 5KB)
	constexpr int32  DefaultMipMargin    = 1;
	constexpr float  UvDensityPercentile = 0.99f;
	constexpr uint32 InvalidMip          = 0xFFFFFFFFu;

	// ---- 텍스처 본문 배치 (FAssetCache 텍스처 본문: u8 형식, u8 sRGB, u32 밉 수, 밉마다 [u32 폭, u32 높이, u32 바이트 수, 데이터])
	constexpr uint64 PayloadHeaderSize   = 6;
	constexpr uint64 MipRecordHeaderSize = 12;
	constexpr uint64 PayloadProbeSize    = PayloadHeaderSize + 8; // 머리 + 밉 0 폭/높이 (배치 계산에 필요한 최소 읽기)

	struct FMipRecord
	{
		uint64 RecordOffset = 0; // 본문 시작 기준 (밉 머리 위치)
		uint64 DataOffset   = 0; // 본문 시작 기준 (데이터 위치 = RecordOffset + 12)
		uint64 DataSize     = 0;
		uint32 Width        = 0;
		uint32 Height       = 0;
	};

	struct FPayloadLayout
	{
		ETextureFormat          Format   = ETextureFormat::RGBA8;
		bool                    bSRGB    = false;
		uint32                  Width    = 0;
		uint32                  Height   = 0;
		uint32                  MipCount = 0;
		std::vector<FMipRecord> Mips;
		uint64                  TotalSize = 0; // 본문 전체 바이트

		bool   IsValid() const { return MipCount > 0 && Mips.size() == MipCount; }
		// [Top..MipCount) 데이터 바이트 (상주 크기)
		uint64 GetRangeDataBytes(uint32 Top) const;
		// [Top..MipCount) 읽기 범위 (본문 기준): 시작 = Mips[Top].RecordOffset, 크기 = TotalSize - 시작
		uint64 GetRangeReadOffset(uint32 Top) const { return Mips[Top].RecordOffset; }
		uint64 GetRangeReadSize(uint32 Top) const { return TotalSize - Mips[Top].RecordOffset; }
	};

	// 밉 크기 규칙(max(1, 절반))으로 배치 계산. 밉 수가 잘못되면(0 또는 체인보다 김) 무효
	FPayloadLayout ComputePayloadLayout(ETextureFormat Format, bool bSRGB, uint32 Width, uint32 Height, uint32 MipCount);
	// PayloadProbeSize 바이트(본문 머리 + 밉 0 폭/높이)로 배치 계산. 손상이면 무효
	FPayloadLayout ParsePayloadProbe(const uint8* Data, size_t Size);
	// [Top..) 범위 읽기 결과(GetRangeReadOffset부터 GetRangeReadSize 바이트)를 밉 목록으로 (머리·크기 검증). 실패면 false
	bool ParseMipRange(const FPayloadLayout& Layout, uint32 Top, const uint8* Data, size_t Size, std::vector<FTextureMip>& OutMips);

	// ---- 상주 범위
	// 새 텍스처의 밉 0이 될 수 있는 가장 큰 밉 번호 (0부터 연속)
	uint32 GetMaxValidTopMip(ETextureFormat Format, uint32 Width, uint32 Height, uint32 MipCount);
	// 항상 상주하는 꼬리의 시작 밉 (= 스트리밍으로 내려갈 수 있는 가장 낮은 품질). 0이면 스트리밍하지 않음
	uint32 ComputeTailTopMip(ETextureFormat Format, uint32 Width, uint32 Height, uint32 MipCount, uint32 NonStreamingMax = NonStreamingMaxSize,
	                         uint32 MinResident = MinResidentSize);

	// ---- 필요 밉
	float ComputePerspectiveCmPerPixel(float ViewDepth, float TanHalfFovY, uint32 ScreenHeight);
	float ComputeOrthographicCmPerPixel(float OrthoHeight, uint32 ScreenHeight);
	// log2(UV 밀도 × CmPerPixel) — 텍스처 크기와 무관한 부분 (머티리얼마다 인스턴스 최솟값을 모은다). 밀도/픽셀 크기가 0 이하면 -무한대
	float ComputeLog2UvPerPixel(float UvDensityPerCm, float CmPerPixel);
	// 필요한 최상위 밉 (0 = 전체). Log2UvPerPixel은 위 값 + log2(머티리얼 UV 배율)
	uint32 ComputeRequiredTopMip(uint32 TextureMaxSize, float Log2UvPerPixel, float MipBias, int32 Margin, uint32 MipCount);
	// 위 둘을 합친 편의 함수
	uint32 ComputeRequiredTopMip(uint32 TextureMaxSize, float UvDensityPerCm, float CmPerPixel, float MipBias, int32 Margin, uint32 MipCount);

	// 메시 UV 밀도 (UV/cm, 로컬 단위). 유효 삼각형이 없으면 0
	float ComputeUvDensity(const std::vector<FVertex>& Vertices, const std::vector<uint32>& Indices, float Percentile = UvDensityPercentile);

	// 그래프 머티리얼 텍스처 파라미터의 UV 배율: 모든 TextureSample(이 파라미터)의 UV가 연결 없음(기본 UV0) 또는 TexCoord(타일링 상수)면
	// 최대 |타일링|, 계산된 UV(월드 위치·곱하기 등)·상수 UV가 하나라도 있으면 -1 (알 수 없음 → 항상 전체 밉). 쓰는 노드가 없으면 1
	float ComputeGraphTextureUvScale(const FMaterialGraph& Graph, std::string_view ParameterName);

	// ---- 내릴 때 늦게 (히스테리시스)
	struct FHysteresisState
	{
		uint32 HeldTop      = InvalidMip; // 유지 중인 목표 (InvalidMip = 처음)
		float  LowerSeconds = 0.0f;       // 원하는 밉이 유지 중인 것보다 낮은 품질로 계속된 시간
	};
	// 더 세밀한 밉이 필요하면 바로, 덜 세밀해도 되면 DropDelaySeconds 동안 계속될 때만 따라간다. 반환 = 유지 목표
	uint32 UpdateHysteresis(FHysteresisState& State, uint32 WantedTop, float DeltaSeconds, float DropDelaySeconds);

	// ---- 예산 배분
	struct FBudgetItem
	{
		uint32        WantedTop = 0;
		uint32        TailTop   = 0;      // 이 밉 아래로는 줄이지 않는다
		float         Priority  = 0.0f;   // 클수록 늦게 줄인다 (화면 크기, 메인 뷰 가중)
		const uint64* RangeBytes = nullptr; // RangeBytes[k] = [k..) 상주 바이트, TailTop까지 유효
	};
	struct FBudgetResult
	{
		std::vector<uint32> Tops;
		uint64              TotalBytes  = 0;
		bool                bOverBudget = false; // 모두 꼬리까지 줄여도 넘침
	};
	// 원하는 밉으로 시작해 예산을 넘으면 "우선순위 × (이미 줄인 단계 + 1)"이 가장 작은 텍스처부터 한 밉씩 줄인다
	// (우선순위가 낮은 텍스처가 먼저·더 많이, 같은 텍스처를 연달아 줄이지 않게 고르게). 같은 값이면 번호가 작은 쪽 — 결정적
	FBudgetResult FitToBudget(const std::vector<FBudgetItem>& Items, uint64 BudgetBytes);
} // namespace TextureStreamingMath
