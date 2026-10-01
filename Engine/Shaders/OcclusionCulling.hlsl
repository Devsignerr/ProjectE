#include "Common.hlsli"

// HZB(계층 Z) 오클루전 컬링 (Renderer/OcclusionCuller, 식은 Renderer/HzbMath.h와 같다)
//   HzbFromDepthCS / HzbDownsampleCS: 깊이 → HZB 밉 0 → 밉 k (2x2 최댓값 = 가장 먼 깊이). HZB 크기는 2의 거듭제곱(깊이 밖은 0)
//   CullCS: 메인 패스 정적 묶음의 인스턴스마다 AABB를 투영해 HZB와 비교 → 보이면 그 단계의 묶음 간접 인자 InstanceCount를 올리고
//           인스턴스 번호를 단계별 목록의 묶음 구간에 쓴다. 1단계 = 이전 프레임 HZB(+그때 뷰-투영), 2단계 = 1단계에서 가려진 것만
//           이번 프레임 HZB(1단계 깊이)로 다시 → 새로 드러난 물체가 한 프레임 늦게 보이는 깜빡임이 없다

// ---- HZB 만들기 (루트 상수 4개)
cbuffer HzbBuildConstants : register(b0)
{
	uint2 SourceSize;
	uint2 DestSize;
};

Texture2D<float>   SourceDepth : register(t0); // 밉 0: 씬 깊이 (R32_FLOAT)
RWTexture2D<float> DestMip     : register(u0);
RWTexture2D<float> SourceMip   : register(u1); // 밉 k ≥ 1: 바로 위 밉

[numthreads(8, 8, 1)]
void HzbFromDepthCS(uint3 Id : SV_DispatchThreadID)
{
	if (any(Id.xy >= DestSize))
	{
		return;
	}
	// 깊이 밖 텍셀은 0 (아무것도 가리지 않음 — 최댓값에 영향 없음)
	float Farthest = 0.0f;
	[unroll]
	for (uint DY = 0; DY < 2; ++DY)
	{
		[unroll]
		for (uint DX = 0; DX < 2; ++DX)
		{
			const uint2 S = Id.xy * 2 + uint2(DX, DY);
			if (all(S < SourceSize))
			{
				Farthest = max(Farthest, SourceDepth.Load(int3(S, 0)));
			}
		}
	}
	DestMip[Id.xy] = Farthest;
}

[numthreads(8, 8, 1)]
void HzbDownsampleCS(uint3 Id : SV_DispatchThreadID)
{
	if (any(Id.xy >= DestSize))
	{
		return;
	}
	const uint2 S0 = Id.xy * 2;
	const uint2 S1 = min(S0 + 1, SourceSize - 1);
	const float A  = max(SourceMip[S0], SourceMip[uint2(S1.x, S0.y)]);
	const float B  = max(SourceMip[uint2(S0.x, S1.y)], SourceMip[S1]);
	DestMip[Id.xy] = max(A, B);
}

// ---- 컬링 (ShaderTypes.h FOcclusionItem / FOcclusionCullConstants와 1:1)
struct FOcclusionItem
{
	float3 BoundsMin;
	uint   Batch;
	float3 BoundsMax;
	uint   Instance;
	uint   BatchFirst;
	uint3  Padding;
};

cbuffer CullConstants : register(b1) // HZB 만들기와 레지스터가 겹치지 않게 (같은 파일)
{
	float4x4 CullViewProjection;
	float2   DepthSize;
	uint     HzbMipCount;
	uint     ItemCount;
	uint     Phase;
	uint     bHzbValid;
	uint2    CullPadding;
};

StructuredBuffer<FOcclusionItem> Items         : register(t1);
Texture2D<float>                 Hzb           : register(t2);
RWByteAddressBuffer              DrawArguments : register(u2); // 묶음마다 [1단계 D3D12_DRAW_INDEXED_ARGUMENTS, 2단계 …] (각 20바이트)
RWStructuredBuffer<uint>         Phase1Indices : register(u3);
RWStructuredBuffer<uint>         Phase2Indices : register(u4);
RWStructuredBuffer<uint>         Occluded      : register(u5); // 항목별: 1단계에서 가려졌으면 1

static const uint DrawArgumentsStride = 20;

bool IsVisible(FOcclusionItem Item)
{
	if (bHzbValid == 0)
	{
		return true;
	}
	float2 NdcMin = 1.0e30f;
	float2 NdcMax = -1.0e30f;
	float  MinZ   = 1.0f;
	[unroll]
	for (uint Corner = 0; Corner < 8; ++Corner)
	{
		const float3 P    = float3((Corner & 1) ? Item.BoundsMax.x : Item.BoundsMin.x, (Corner & 2) ? Item.BoundsMax.y : Item.BoundsMin.y,
		                           (Corner & 4) ? Item.BoundsMax.z : Item.BoundsMin.z);
		const float4 Clip = mul(float4(P, 1.0f), CullViewProjection);
		if (Clip.w <= 1.0e-4f)
		{
			return true; // 근평면을 지난다
		}
		const float3 Ndc = Clip.xyz / Clip.w;
		NdcMin           = min(NdcMin, Ndc.xy);
		NdcMax           = max(NdcMax, Ndc.xy);
		MinZ             = min(MinZ, Ndc.z);
	}
	if (MinZ <= 0.0f || any(NdcMax < -1.0f) || any(NdcMin > 1.0f))
	{
		return true; // 판정 불가 (이 시점의 화면 밖)
	}
	NdcMin = clamp(NdcMin, -1.0f, 1.0f);
	NdcMax = clamp(NdcMax, -1.0f, 1.0f);
	const float2 PixelMin = float2(NdcMin.x * 0.5f + 0.5f, 0.5f - NdcMax.y * 0.5f) * DepthSize;
	const float2 PixelMax = float2(NdcMax.x * 0.5f + 0.5f, 0.5f - NdcMin.y * 0.5f) * DepthSize;

	// 사각형이 텍셀 2개 이하로 들어가는 밉 (밉 k 텍셀 = 2^(k+1) 픽셀)
	const float Size = max(PixelMax.x - PixelMin.x, PixelMax.y - PixelMin.y);
	uint        Mip  = 0;
	while (Mip + 1 < HzbMipCount && (float)(2u << Mip) < Size)
	{
		++Mip;
	}
	uint MipWidth, MipHeight, Levels;
	Hzb.GetDimensions(Mip, MipWidth, MipHeight, Levels);
	const float Scale  = 1.0f / (float)(2u << Mip);
	const int2  MaxXY  = int2(MipWidth, MipHeight) - 1;
	const int2  Texel0 = clamp((int2)floor(PixelMin * Scale), 0, MaxXY);
	const int2  Texel1 = clamp((int2)floor(PixelMax * Scale), 0, MaxXY);

	float Farthest = 0.0f;
	for (int Y = Texel0.y; Y <= Texel1.y; ++Y)
	{
		for (int X = Texel0.x; X <= Texel1.x; ++X)
		{
			Farthest = max(Farthest, Hzb.Load(int3(X, Y, Mip)));
		}
	}
	return MinZ <= Farthest;
}

[numthreads(64, 1, 1)]
void CullCS(uint3 Id : SV_DispatchThreadID)
{
	if (Id.x >= ItemCount)
	{
		return;
	}
	const FOcclusionItem Item = Items[Id.x];
	if (Phase == 1)
	{
		if (IsVisible(Item))
		{
			uint Slot;
			DrawArguments.InterlockedAdd((Item.Batch * 2) * DrawArgumentsStride + 4, 1, Slot);
			Phase1Indices[Item.BatchFirst + Slot] = Item.Instance;
			Occluded[Id.x]                        = 0;
		}
		else
		{
			Occluded[Id.x] = 1;
		}
	}
	else if (Occluded[Id.x] != 0 && IsVisible(Item))
	{
		uint Slot;
		DrawArguments.InterlockedAdd((Item.Batch * 2 + 1) * DrawArgumentsStride + 4, 1, Slot);
		Phase2Indices[Item.BatchFirst + Slot] = Item.Instance;
	}
}
