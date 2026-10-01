#ifndef E_SCREEN_SPACE_HLSLI
#define E_SCREEN_SPACE_HLSLI

// 화면 공간 버퍼 공용 식 (Renderer/TemporalMath.h와 같은 식, 테스트 TemporalTests)
//   법선 버퍼 (R10G10B10A2_UNORM): RG = 월드 법선(기하) 팔면체 인코딩 * 0.5 + 0.5, B = 거칠기(머티리얼), A 예약 (0)
//   움직임 벡터 (R16G16_FLOAT): 현재 UV - 이전 UV (지터 없는 위치 기준, UV 왼쪽 위 원점). 지운 값 0 = 기하가 없는 픽셀(하늘)
//     → 사용하는 쪽은 깊이가 1(먼 평면)인 픽셀을 카메라 재투영으로 계산한다

float2 OctahedralWrap(float2 V)
{
	return (1.0f - abs(V.yx)) * select(V.xy >= 0.0f, 1.0f, -1.0f);
}

float2 EncodeOctahedral(float3 N)
{
	N /= max(abs(N.x) + abs(N.y) + abs(N.z), 1.0e-8f);
	return N.z >= 0.0f ? N.xy : OctahedralWrap(N.xy);
}

float3 DecodeOctahedral(float2 E)
{
	float3      N = float3(E, 1.0f - abs(E.x) - abs(E.y));
	const float T = saturate(-N.z);
	N.xy += select(N.xy >= 0.0f, -T, T);
	return normalize(N);
}

// B = 지각적 거칠기 (SSR이 거칠면 건너뛰고 GGX로 흔든다, 33-6)
float4 EncodeScreenNormal(float3 WorldNormal, float Roughness)
{
	return float4(EncodeOctahedral(WorldNormal) * 0.5f + 0.5f, saturate(Roughness), 0.0f);
}

float DecodeScreenRoughness(float4 Encoded)
{
	return Encoded.b;
}

float3 DecodeScreenNormal(float4 Encoded)
{
	return DecodeOctahedral(Encoded.xy * 2.0f - 1.0f);
}

// 데칼 DBuffer(B = 법선, C = 거칠기, a = 남은 원래 표면 비중)를 화면 법선/거칠기에 적용 — Mesh.hlsl ApplyDecals와 같은 식.
//   데칼은 사전 패스 뒤에 그려지므로 사전 패스 버퍼를 읽는 화면 효과(SSR)는 이걸로 메인 패스와 같은 표면을 본다
void ApplyScreenDecals(Texture2D<float4> DecalNormal, Texture2D<float4> DecalMaterial, int2 Pixel, inout float3 N, inout float Roughness)
{
	const float4 B = DecalNormal.Load(int3(Pixel, 0));
	const float4 C = DecalMaterial.Load(int3(Pixel, 0));
	N              = normalize(N * B.a + B.rgb * 2.0f - (1.0f - B.a));
	Roughness      = Roughness * C.a + C.r;
}

// 클립 좌표(지터 없음) 두 개 → UV 단위 움직임 벡터 (현재 - 이전)
float2 ComputeVelocity(float4 CurrentClip, float4 PreviousClip)
{
	if (CurrentClip.w <= 1.0e-6f || PreviousClip.w <= 1.0e-6f)
	{
		return 0.0f;
	}
	const float2 Current  = CurrentClip.xy / CurrentClip.w;
	const float2 Previous = PreviousClip.xy / PreviousClip.w;
	return (Current - Previous) * float2(0.5f, -0.5f);
}

#endif // E_SCREEN_SPACE_HLSLI
