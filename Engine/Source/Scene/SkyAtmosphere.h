#pragma once

#include "Core/Math/Math.h"

#include <cmath>

class FScene;

// 하늘·대기·구름·시간대·물 컴포넌트 (Phase 49). 렌더링은 Renderer(FSkyAtmosphereRenderer/FVolumetricCloudRenderer/FWaterRenderer),
// 씬 쪽 로직은 시간대(FTimeOfDaySystem)와 태양 각도 도우미뿐이다. 모두 씬의 첫 컴포넌트 하나만 쓴다 (물은 여러 개)

// 물리 기반 대기 (Hillaire 2020, 식은 Renderer/AtmosphereMath.h). 있으면 하늘 배경·환경광(IBL) 원본을 대기로 바꾸고
// (하늘광 EnvironmentMap 무시), 씬의 첫 방향광을 태양으로 쓴다 (방향 = 엔티티 전방, 조도 = 색 × 강도 = 대기 위 태양 조도)
//   산란/흡수 계수 = 색 × 배율 (1/km). 행성은 월드 원점 아래 (월드 Z = 0이 지면)
struct FSkyAtmosphereComponent
{
	float    PlanetRadius           = 6360.0f; // km
	float    AtmosphereHeight       = 100.0f;  // km
	FVector3 RayleighScatteringColor = FVector3(0.175287f, 0.409607f, 1.0f);
	float    RayleighScatteringScale = 0.0331f;  // 1/km
	float    RayleighScaleHeight     = 8.0f;     // km
	FVector3 MieScatteringColor      = FVector3::OneVector;
	float    MieScatteringScale      = 0.003996f; // 1/km
	FVector3 MieAbsorptionColor      = FVector3::OneVector;
	float    MieAbsorptionScale      = 0.000444f; // 1/km
	float    MieAnisotropy           = 0.8f;
	float    MieScaleHeight          = 1.2f;     // km
	FVector3 OzoneAbsorptionColor    = FVector3(0.345561f, 1.0f, 0.045189f);
	float    OzoneAbsorptionScale    = 0.001881f; // 1/km
	FVector3 GroundAlbedo            = FVector3(0.3f, 0.3f, 0.3f);
	float    SkyLuminanceScale       = 1.0f;  // 하늘 배경·환경광·공중 원근 밝기 배율
	float    SunDiskSize             = 0.53f; // 겉보기 지름 (도)
	float    SunDiskIntensity        = 1.0f;  // 태양 원반 밝기 배율 (1 = 블룸이 과하지 않게 줄인 기본값)
	bool     bAffectSunLight         = true;  // 방향광 색에 카메라 고도의 대기 투과율을 곱한다 (해질녘 붉은 빛)
	FVector3 NightSkyColor           = FVector3(0.01f, 0.016f, 0.035f); // 밤 최소 하늘 휘도 (선형 — 환경광에도 들어간다)
	float    StarIntensity           = 1.0f;  // 밤하늘 별 (배경에만)
	float    MoonIntensity           = 0.25f; // 달빛 조도 (태양이 지평선 아래로 지면 방향광이 태양 반대편 달로 바뀐다, 0 = 끔)
	FVector3 MoonColor               = FVector3(0.62f, 0.72f, 1.0f); // 달빛 색 (선형)
	float    AerialPerspectiveScale  = 1.0f;  // 공중 원근 거리 배율 (0 = 끔)
	bool     bRealtimeEnvironmentLighting = true; // 대기(+구름)로 하늘 큐브를 다시 만들어 IBL(조도/프리필터)을 갱신 (태양·구름이 바뀔 때, 몇 프레임에 나눠)
};

// 볼류메트릭 구름 (식은 Renderer/CloudMath.h). 대기 컴포넌트와 함께 쓴다 (태양 투과율·하늘 조도·공중 원근)
//   낮은 적운 층(레이마칭, 3D 노이즈) + 높은 권운 층(2D, 선택). 저해상도 + 시간 누적 → 업샘플 합성 (하늘 픽셀)
struct FVolumetricCloudComponent
{
	float    LayerBottomAltitude = 1.5f;  // km (지면 기준)
	float    LayerThickness      = 2.5f;  // km
	float    Coverage            = 0.45f; // 0 = 맑음, 1 = 흐림
	float    CloudType           = 0.75f; // 0 = 낮고 평평, 1 = 높이 솟은 적운
	float    Extinction          = 40.0f; // 1/km (밀도 1에서)
	float    ShapeTileSize       = 18.0f; // km (모양 노이즈 한 장)
	float    DetailTileSize      = 1.2f;  // km (세부 노이즈 한 장)
	float    WeatherTileSize     = 40.0f; // km (덮임 분포 한 장)
	float    DetailStrength      = 0.35f;
	FVector3 Albedo              = FVector3::OneVector;
	float    WindDirection       = 30.0f; // 도 (방위각, +X에서 +Y 쪽)
	float    WindSpeed           = 10.0f; // m/s
	float    SilverLining        = 0.6f;  // 앞쪽 위상 이심률 g
	float    AmbientScale        = 1.0f;  // 하늘 조도 앰비언트 배율
	float    CirrusCoverage      = 0.3f;  // 높은 권운 층 (0 = 없음)
	float    CirrusAltitude      = 8.0f;  // km
	bool     bAffectEnvironmentLighting = true; // 환경광(IBL 하늘 큐브)에 구름을 넣는다 (저해상도)
};

// 시간대: 씬의 첫 방향광 회전을 시간으로 정한다 (FSunMath::TimeOfDayToSunAngles). DayLengthMinutes > 0이면 플레이 중 시간이 흐른다
struct FTimeOfDayComponent
{
	float TimeOfDay        = 14.0f; // 시 (0~24)
	float DayLengthMinutes = 0.0f;  // 실제 몇 분에 하루가 도는가 (0 = 멈춤)
	float MaxSunElevation  = 60.0f; // 정오 태양 고도 (도)
	float NorthAzimuth     = 0.0f;  // 북쪽 방위각 (도, +X 기준)
	bool  bAnimateInEditor = false; // 플레이가 아닐 때도 시간이 흐른다 (에디터 미리보기)
};

// 소규모 물 (수영장·분수·웅덩이·강 구간, 식은 Renderer/WaterMath.h). 상자 = 엔티티 위치 중심 Size (회전은 요만), 수면 = 윗면.
// 반투명 위치의 전용 물 패스가 굴절(씬 컬러 복사본)·반사(화면 공간 → 하늘/캡처)·깊이 색·가장자리 거품을 그린다. 바다·부력 없음
// (물리 트리거가 필요하면 같은 엔티티에 BoxCollider(IsTrigger)를 붙인다)
struct FWaterBodyComponent
{
	FVector3 Size               = FVector3(1000.0f, 1000.0f, 200.0f); // cm (전체 크기)
	FVector3 ScatterColor       = FVector3(0.02f, 0.09f, 0.10f);      // 물속 산란 색 (선형, 조명 곱)
	FVector3 Absorption         = FVector3(0.45f, 0.09f, 0.065f);     // 1/m (빨강이 먼저 사라짐)
	float    NormalStrength     = 0.4f;
	float    WaveScale          = 300.0f; // cm (잔물결 노멀 한 장)
	float    WaveSpeed          = 15.0f;  // cm/s
	float    FlowDirection      = 0.0f;   // 도 (로컬, 강 흐름)
	float    FlowSpeed          = 0.0f;   // cm/s (0 = 고인 물)
	float    FoamIntensity      = 0.6f;
	float    FoamDistance       = 25.0f;  // cm (물 두께가 이보다 얇으면 거품)
	float    RefractionStrength = 0.04f;  // 화면 UV 오프셋 (노멀 xy 배율)
	float    ReflectionIntensity = 1.0f;
	float    Roughness          = 0.06f;  // 태양 반사광 거칠기
};

// 태양 각도 ↔ 방향·시간대 (엔진 Z-up, 방위각 0 = +X, 90 = +Y). 태양 방향 = 태양을 향함 (빛 진행 방향의 반대)
namespace FSunMath
{
	inline FVector3 SunDirectionFromAngles(float ElevationDegrees, float AzimuthDegrees)
	{
		const float Elevation = FMath::DegreesToRadians(ElevationDegrees);
		const float Azimuth   = FMath::DegreesToRadians(AzimuthDegrees);
		return FVector3(std::cos(Elevation) * std::cos(Azimuth), std::cos(Elevation) * std::sin(Azimuth), std::sin(Elevation));
	}
	inline void SunAnglesFromDirection(const FVector3& SunDirection, float& OutElevationDegrees, float& OutAzimuthDegrees)
	{
		const FVector3 D    = SunDirection.GetNormalized();
		OutElevationDegrees = FMath::RadiansToDegrees(FMath::Asin(D.Z));
		OutAzimuthDegrees   = FMath::RadiansToDegrees(FMath::Atan2(D.Y, D.X));
	}
	// 방향광 엔티티 회전 (FQuat::FromEuler 피치/요): 빛 진행 방향(전방) = -태양 방향
	inline FQuat SunAnglesToLightRotation(float ElevationDegrees, float AzimuthDegrees)
	{
		return FQuat::FromEuler(-ElevationDegrees, AzimuthDegrees + 180.0f, 0.0f);
	}
	// 시간대 → 태양 각도 (간이 모델: 6시 일출·18시 일몰, 정오 고도 = MaxElevation, 방위는 동 → 남 → 서).
	// 밤에는 고도가 음수 (지평선 아래 — 대기가 박명/밤을 만든다)
	inline void TimeOfDayToSunAngles(float Hours, float MaxElevationDegrees, float NorthAzimuthDegrees, float& OutElevationDegrees, float& OutAzimuthDegrees)
	{
		float H = std::fmod(Hours, 24.0f);
		if (H < 0.0f)
		{
			H += 24.0f;
		}
		const float DayAngle = (H - 6.0f) / 12.0f * FMath::Pi; // 6시 0, 12시 π/2, 18시 π
		OutElevationDegrees  = std::sin(DayAngle) * MaxElevationDegrees;
		OutAzimuthDegrees    = std::fmod(NorthAzimuthDegrees + 90.0f + FMath::RadiansToDegrees(DayAngle) + 720.0f, 360.0f); // 동(북 + 90) → 남 → 서
	}
} // namespace FSunMath

// 씬 도우미 (Lua Sky 테이블·시간대 시스템 공용)
namespace FSkyScene
{
	// 씬의 첫 방향광 회전을 태양 각도로. 방향광이 없으면 false
	bool SetSunAngles(FScene& Scene, float ElevationDegrees, float AzimuthDegrees);
	bool GetSunAngles(FScene& Scene, float& OutElevationDegrees, float& OutAzimuthDegrees);
	// 시간대 컴포넌트 (없으면 false)
	bool SetTimeOfDay(FScene& Scene, float Hours);
	bool GetTimeOfDay(FScene& Scene, float& OutHours);
} // namespace FSkyScene

// 시간대 갱신: 씬의 첫 FTimeOfDayComponent가 있으면 시간을 흘리고(bAdvance 또는 bAnimateInEditor) 첫 방향광 회전을 맞춘다.
// FGameWorld::TickPresentation이 매 프레임 부른다 (트랜스폼 갱신 전)
struct FTimeOfDaySystem
{
	static void Update(FScene& Scene, float DeltaSeconds, bool bAdvance);
};

// 리플렉션 등록 (RegisterSceneTypes 끝에서 부른다)
void RegisterSkyTypes();
