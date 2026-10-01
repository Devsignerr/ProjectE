#include "Common.hlsli"
#include "Fullscreen.hlsli"
#include "SkinnedMesh.hlsli"
#include "MeshInstance.hlsli"

// 에디터 선택 아웃라인
//   1) Mask 패스: 선택된 메시를 R8 마스크에 1로 기록
//   2) Composite 패스: 마스크 경계(바깥쪽 Thickness 픽셀)를 출력 위에 알파 블렌드로 그린다

// 루트 상수 17개: 뷰-투영 + 묶음의 인스턴스 번호 시작 위치
cbuffer MaskConstants : register(b0)
{
	float4x4 ViewProjection;
	uint     InstanceOffset;
};

// 정적 메시 (인스턴싱)
float4 MaskVS(float3 Position : POSITION, uint InstanceId : SV_InstanceID) : SV_Position
{
	const FInstanceData Instance = LoadInstance(InstanceOffset, InstanceId);
	return mul(mul(float4(Position, 1.0f), Instance.World), ViewProjection);
}

// 스킨 메시 (인스턴싱): 인스턴스 팔레트가 바로 월드로 보낸다
float4 MaskSkinnedVS(float3 Position : POSITION, uint4 Joints : BLENDINDICES, float4 Weights : BLENDWEIGHT, uint InstanceId : SV_InstanceID) : SV_Position
{
	const FInstanceData Instance = LoadInstance(InstanceOffset, InstanceId);
	return mul(mul(float4(Position, 1.0f), ComputeSkinMatrix(Instance.BoneOffset, Joints, Weights)), ViewProjection);
}

float MaskPS() : SV_Target
{
	return 1.0f;
}

cbuffer CompositeConstants : register(b0)
{
	float4 OutlineColor;  // 선형 RGB + 불투명도
	float4 FillColor;     // 선택 영역 내부 틴트 (a = 강도)
	int    Thickness;     // 픽셀
	int3   Padding0;
};

Texture2D<float> SelectionMask : register(t0);

FFullscreenVSOutput CompositeVS(uint VertexId : SV_VertexID)
{
	return FullscreenVS(VertexId);
}

float4 CompositePS(FFullscreenVSOutput Input) : SV_Target
{
	uint Width;
	uint Height;
	SelectionMask.GetDimensions(Width, Height);
	const int2 Pixel = int2(Input.Position.xy);

	const float Center = SelectionMask.Load(int3(Pixel, 0));
	if (Center > 0.5f)
	{
		return FillColor; // 내부: 약한 틴트
	}

	// 바깥 픽셀: 원형 반경 안에 선택 픽셀이 있으면 아웃라인
	float Coverage = 0.0f;
	[loop]
	for (int Y = -Thickness; Y <= Thickness; ++Y)
	{
		[loop]
		for (int X = -Thickness; X <= Thickness; ++X)
		{
			const float Distance = length(float2(X, Y));
			if (Distance > Thickness + 0.5f)
			{
				continue;
			}
			const int2 Sample = clamp(Pixel + int2(X, Y), int2(0, 0), int2(Width - 1, Height - 1));
			if (SelectionMask.Load(int3(Sample, 0)) > 0.5f)
			{
				// 가장자리 픽셀은 거리로 부드럽게
				Coverage = max(Coverage, saturate(Thickness + 0.5f - Distance));
			}
		}
	}
	if (Coverage <= 0.0f)
	{
		discard;
	}
	return float4(OutlineColor.rgb, OutlineColor.a * Coverage);
}
