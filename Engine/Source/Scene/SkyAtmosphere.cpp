#include "Scene/SkyAtmosphere.h"

#include "Core/Console/Console.h"
#include "Core/Reflection/TypeInfo.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

namespace
{
	// 확인·비교용: 0 이상이면 시간대 컴포넌트 시각을 이 값으로 고정 (--cvar sky.TimeOfDayOverride=18.5)
	TAutoConsoleVariable<float> TimeOfDayOverride("sky.TimeOfDayOverride", -1.0f,
	                                              "시간대 컴포넌트 시각 고정 (시, 0~24). 음수 = 끔 (컴포넌트 값과 하루 길이대로)", EConsoleFlags::None,
	                                              { .Range = std::pair(-1.0f, 24.0f) });

	// 씬의 첫 방향광 트랜스폼 (없으면 nullptr) — 렌더러(BuildPerFrameConstants)와 같은 "첫 방향광" 규칙
	FTransformComponent* FindSunTransform(FScene& Scene, FEntity* OutEntity = nullptr)
	{
		FTransformComponent* Found = nullptr;
		Scene.GetRegistry().View<FTransformComponent, FDirectionalLightComponent>().Each(
			[&](FEntity Entity, FTransformComponent& Transform, FDirectionalLightComponent&) {
				if (Found == nullptr)
				{
					Found = &Transform;
					if (OutEntity != nullptr)
					{
						*OutEntity = Entity;
					}
				}
			});
		return Found;
	}

	FTimeOfDayComponent* FindTimeOfDay(FScene& Scene)
	{
		FTimeOfDayComponent* Found = nullptr;
		Scene.GetRegistry().View<FTimeOfDayComponent>().Each([&](FEntity, FTimeOfDayComponent& TimeOfDay) {
			if (Found == nullptr)
			{
				Found = &TimeOfDay;
			}
		});
		return Found;
	}
} // namespace

bool FSkyScene::SetSunAngles(FScene& Scene, float ElevationDegrees, float AzimuthDegrees)
{
	FTransformComponent* Transform = FindSunTransform(Scene);
	if (Transform == nullptr)
	{
		return false;
	}
	Transform->Rotation = FSunMath::SunAnglesToLightRotation(ElevationDegrees, AzimuthDegrees);
	return true;
}

bool FSkyScene::GetSunAngles(FScene& Scene, float& OutElevationDegrees, float& OutAzimuthDegrees)
{
	FTransformComponent* Transform = FindSunTransform(Scene);
	if (Transform == nullptr)
	{
		return false;
	}
	// 로컬 회전 기준 (태양 엔티티는 보통 루트) — 빛 진행 방향의 반대가 태양
	FSunMath::SunAnglesFromDirection(-Transform->Rotation.GetForwardVector(), OutElevationDegrees, OutAzimuthDegrees);
	return true;
}

bool FSkyScene::SetTimeOfDay(FScene& Scene, float Hours)
{
	FTimeOfDayComponent* TimeOfDay = FindTimeOfDay(Scene);
	if (TimeOfDay == nullptr)
	{
		return false;
	}
	TimeOfDay->TimeOfDay = std::fmod(std::fmod(Hours, 24.0f) + 24.0f, 24.0f);
	return true;
}

bool FSkyScene::GetTimeOfDay(FScene& Scene, float& OutHours)
{
	FTimeOfDayComponent* TimeOfDay = FindTimeOfDay(Scene);
	if (TimeOfDay == nullptr)
	{
		return false;
	}
	OutHours = TimeOfDay->TimeOfDay;
	return true;
}

FEntity FTimeOfDaySystem::Update(FScene& Scene, float DeltaSeconds, bool bAdvance)
{
	FTimeOfDayComponent* TimeOfDay = FindTimeOfDay(Scene);
	if (TimeOfDay == nullptr)
	{
		return NullEntity;
	}
	if (const float Override = TimeOfDayOverride.Get(); Override >= 0.0f)
	{
		TimeOfDay->TimeOfDay = Override;
	}
	else if ((bAdvance || TimeOfDay->bAnimateInEditor) && TimeOfDay->DayLengthMinutes > 0.0f && DeltaSeconds > 0.0f)
	{
		const float HoursPerSecond = 24.0f / (TimeOfDay->DayLengthMinutes * 60.0f);
		TimeOfDay->TimeOfDay       = std::fmod(TimeOfDay->TimeOfDay + DeltaSeconds * HoursPerSecond, 24.0f);
	}
	float Elevation = 0.0f;
	float Azimuth   = 0.0f;
	FSunMath::TimeOfDayToSunAngles(TimeOfDay->TimeOfDay, TimeOfDay->MaxSunElevation, TimeOfDay->NorthAzimuth, Elevation, Azimuth);
	FEntity              Sun       = NullEntity;
	FTransformComponent* Transform = FindSunTransform(Scene, &Sun);
	if (Transform == nullptr)
	{
		return NullEntity;
	}
	Transform->Rotation = FSunMath::SunAnglesToLightRotation(Elevation, Azimuth); // FSkyScene::SetSunAngles와 같은 식
	return Sun;
}

void RegisterSkyTypes()
{
	FTypeRegistry& Registry = FTypeRegistry::Get();
	if (Registry.IsRegistered<FSkyAtmosphereComponent>())
	{
		return;
	}
	Registry.RegisterType<FSkyAtmosphereComponent>("SkyAtmosphereComponent", "대기 (하늘)")
		.Property(&FSkyAtmosphereComponent::PlanetRadius, "PlanetRadius", "행성 반지름 (km)").Range(1.0f, 100000.0f, 1.0f)
		.Property(&FSkyAtmosphereComponent::AtmosphereHeight, "AtmosphereHeight", "대기 두께 (km)").Range(1.0f, 1000.0f, 0.5f)
		.Property(&FSkyAtmosphereComponent::RayleighScatteringColor, "RayleighScatteringColor", "레일리 산란 색")
		.Property(&FSkyAtmosphereComponent::RayleighScatteringScale, "RayleighScatteringScale", "레일리 산란 (1/km)").Range(0.0f, 2.0f, 0.0005f)
		.Property(&FSkyAtmosphereComponent::RayleighScaleHeight, "RayleighScaleHeight", "레일리 높이 감쇠 (km)").Range(0.01f, 100.0f, 0.1f)
		.Property(&FSkyAtmosphereComponent::MieScatteringColor, "MieScatteringColor", "미 산란 색")
		.Property(&FSkyAtmosphereComponent::MieScatteringScale, "MieScatteringScale", "미 산란 (1/km)").Range(0.0f, 5.0f, 0.0005f)
		.Tooltip("먼지·연무. 키우면 하늘이 뿌옇고 태양 주변 광륜이 커진다")
		.Property(&FSkyAtmosphereComponent::MieAbsorptionColor, "MieAbsorptionColor", "미 흡수 색")
		.Property(&FSkyAtmosphereComponent::MieAbsorptionScale, "MieAbsorptionScale", "미 흡수 (1/km)").Range(0.0f, 5.0f, 0.0001f)
		.Property(&FSkyAtmosphereComponent::MieAnisotropy, "MieAnisotropy", "미 비등방성 (g)").Range(0.0f, 0.999f, 0.01f)
		.Property(&FSkyAtmosphereComponent::MieScaleHeight, "MieScaleHeight", "미 높이 감쇠 (km)").Range(0.01f, 20.0f, 0.05f)
		.Property(&FSkyAtmosphereComponent::OzoneAbsorptionColor, "OzoneAbsorptionColor", "오존 흡수 색")
		.Property(&FSkyAtmosphereComponent::OzoneAbsorptionScale, "OzoneAbsorptionScale", "오존 흡수 (1/km)").Range(0.0f, 0.2f, 0.0001f)
		.Property(&FSkyAtmosphereComponent::GroundAlbedo, "GroundAlbedo", "지면 반사율", PF_Color)
		.Property(&FSkyAtmosphereComponent::SkyLuminanceScale, "SkyLuminanceScale", "하늘 밝기 배율").Range(0.0f, 20.0f, 0.01f)
		.Tooltip("하늘 배경·환경광·공중 원근에 곱한다 (태양 직접광은 방향광 강도)")
		.Property(&FSkyAtmosphereComponent::SunDiskSize, "SunDiskSize", "태양 원반 지름 (도)").Range(0.0f, 10.0f, 0.01f)
		.Property(&FSkyAtmosphereComponent::SunDiskIntensity, "SunDiskIntensity", "태양 원반 밝기").Range(0.0f, 100.0f, 0.01f)
		.Property(&FSkyAtmosphereComponent::bAffectSunLight, "AffectSunLight", "태양 빛에 대기 투과율")
		.Tooltip("방향광 색에 카메라 위치에서 태양 쪽 대기 투과율을 곱한다 (해질녘 붉은 빛, 지평선 아래면 0)")
		.Property(&FSkyAtmosphereComponent::NightSkyColor, "NightSkyColor", "밤하늘 최소 밝기", PF_Color)
		.Tooltip("선형 휘도. 하늘 배경과 환경광에 더한다 (밤이 검게 무너지지 않게)")
		.Property(&FSkyAtmosphereComponent::StarIntensity, "StarIntensity", "별 밝기").Range(0.0f, 100.0f, 0.01f)
		.Property(&FSkyAtmosphereComponent::MoonIntensity, "MoonIntensity", "달빛 조도").Range(0.0f, 10.0f, 0.01f)
		.Tooltip("태양이 지평선 아래로 지면 첫 방향광을 태양 반대편의 달빛으로 바꾼다 (박명 동안 서서히, 0 = 끔)")
		.Property(&FSkyAtmosphereComponent::MoonColor, "MoonColor", "달빛 색", PF_Color)
		.Property(&FSkyAtmosphereComponent::AerialPerspectiveScale, "AerialPerspectiveScale", "공중 원근 배율").Range(0.0f, 100.0f, 0.01f)
		.Tooltip("먼 물체가 대기에 묻히는 정도의 거리 배율 (0 = 끔)")
		.Property(&FSkyAtmosphereComponent::bRealtimeEnvironmentLighting, "RealtimeEnvironmentLighting", "환경광 실시간 갱신")
		.Tooltip("태양·구름이 바뀌면 하늘 큐브와 IBL을 몇 프레임에 나눠 다시 만든다. 반사 캡처는 다시 구워야 한다")
		.AsComponent();

	Registry.RegisterType<FVolumetricCloudComponent>("VolumetricCloudComponent", "볼류메트릭 구름")
		.Property(&FVolumetricCloudComponent::LayerBottomAltitude, "LayerBottomAltitude", "층 바닥 고도 (km)").Range(0.0f, 20.0f, 0.05f)
		.Property(&FVolumetricCloudComponent::LayerThickness, "LayerThickness", "층 두께 (km)").Range(0.1f, 20.0f, 0.05f)
		.Property(&FVolumetricCloudComponent::Coverage, "Coverage", "덮임").Range(0.0f, 1.0f, 0.01f)
		.Property(&FVolumetricCloudComponent::CloudType, "CloudType", "구름 종류").Range(0.0f, 1.0f, 0.01f).Tooltip("0 = 낮고 평평, 1 = 높이 솟은 적운")
		.Property(&FVolumetricCloudComponent::Extinction, "Extinction", "소멸 (1/km)").Range(0.0f, 1000.0f, 0.5f)
		.Property(&FVolumetricCloudComponent::ShapeTileSize, "ShapeTileSize", "모양 크기 (km)").Range(0.5f, 200.0f, 0.1f)
		.Property(&FVolumetricCloudComponent::DetailTileSize, "DetailTileSize", "세부 크기 (km)").Range(0.05f, 20.0f, 0.01f)
		.Property(&FVolumetricCloudComponent::WeatherTileSize, "WeatherTileSize", "덮임 분포 크기 (km)").Range(1.0f, 500.0f, 0.5f)
		.Property(&FVolumetricCloudComponent::DetailStrength, "DetailStrength", "세부 깎기").Range(0.0f, 1.0f, 0.01f)
		.Property(&FVolumetricCloudComponent::Albedo, "Albedo", "반사율", PF_Color)
		.Property(&FVolumetricCloudComponent::WindDirection, "WindDirection", "바람 방향 (도)").Range(-360.0f, 360.0f, 1.0f)
		.Property(&FVolumetricCloudComponent::WindSpeed, "WindSpeed", "바람 속도 (m/s)").Range(0.0f, 200.0f, 0.1f)
		.Property(&FVolumetricCloudComponent::SilverLining, "SilverLining", "은빛 가장자리 (g)").Range(0.0f, 0.95f, 0.01f)
		.Property(&FVolumetricCloudComponent::AmbientScale, "AmbientScale", "하늘빛 배율").Range(0.0f, 10.0f, 0.01f)
		.Property(&FVolumetricCloudComponent::CirrusCoverage, "CirrusCoverage", "권운 덮임").Range(0.0f, 1.0f, 0.01f).Tooltip("높은 얇은 층 (2D)")
		.Property(&FVolumetricCloudComponent::CirrusAltitude, "CirrusAltitude", "권운 고도 (km)").Range(1.0f, 20.0f, 0.1f)
		.Property(&FVolumetricCloudComponent::bAffectEnvironmentLighting, "AffectEnvironmentLighting", "환경광에 구름")
		.AsComponent();

	Registry.RegisterType<FTimeOfDayComponent>("TimeOfDayComponent", "시간대")
		.Property(&FTimeOfDayComponent::TimeOfDay, "TimeOfDay", "시각 (시)").Range(0.0f, 24.0f, 0.05f).Tooltip("씬의 첫 방향광을 이 시각의 태양으로 돌린다")
		.Property(&FTimeOfDayComponent::DayLengthMinutes, "DayLengthMinutes", "하루 길이 (분)").Range(0.0f, 1440.0f, 0.1f).Tooltip("0 = 시간 멈춤")
		.Property(&FTimeOfDayComponent::MaxSunElevation, "MaxSunElevation", "정오 태양 고도 (도)").Range(0.0f, 90.0f, 0.5f)
		.Property(&FTimeOfDayComponent::NorthAzimuth, "NorthAzimuth", "북쪽 방위 (도)").Range(-360.0f, 360.0f, 1.0f)
		.Property(&FTimeOfDayComponent::bAnimateInEditor, "AnimateInEditor", "에디터에서도 흐름")
		.AsComponent();

	Registry.RegisterType<FWaterBodyComponent>("WaterBodyComponent", "물")
		.Property(&FWaterBodyComponent::Size, "Size", "크기 (cm)").Range(1.0f, 1000000.0f, 1.0f).Tooltip("엔티티 위치가 가운데, 윗면이 수면 (회전은 요만)")
		.Property(&FWaterBodyComponent::ScatterColor, "ScatterColor", "물속 산란 색", PF_Color)
		.Property(&FWaterBodyComponent::Absorption, "Absorption", "흡수 (1/m)").Range(0.0f, 100.0f, 0.005f).Tooltip("채널별. 클수록 얕아도 색이 진하다")
		.Property(&FWaterBodyComponent::NormalStrength, "NormalStrength", "잔물결 세기").Range(0.0f, 4.0f, 0.01f)
		.Property(&FWaterBodyComponent::WaveScale, "WaveScale", "잔물결 크기 (cm)").Range(1.0f, 100000.0f, 1.0f)
		.Property(&FWaterBodyComponent::WaveSpeed, "WaveSpeed", "잔물결 속도 (cm/s)").Range(0.0f, 1000.0f, 0.5f)
		.Property(&FWaterBodyComponent::FlowDirection, "FlowDirection", "흐름 방향 (도)").Range(-360.0f, 360.0f, 1.0f)
		.Property(&FWaterBodyComponent::FlowSpeed, "FlowSpeed", "흐름 속도 (cm/s)").Range(0.0f, 2000.0f, 1.0f).Tooltip("강: 잔물결과 거품이 이 방향으로 흐른다")
		.Property(&FWaterBodyComponent::FoamIntensity, "FoamIntensity", "거품 세기").Range(0.0f, 4.0f, 0.01f)
		.Property(&FWaterBodyComponent::FoamDistance, "FoamDistance", "거품 두께 (cm)").Range(0.0f, 500.0f, 0.5f)
		.Property(&FWaterBodyComponent::RefractionStrength, "RefractionStrength", "굴절 세기").Range(0.0f, 0.5f, 0.001f)
		.Property(&FWaterBodyComponent::ReflectionIntensity, "ReflectionIntensity", "반사 세기").Range(0.0f, 4.0f, 0.01f)
		.Property(&FWaterBodyComponent::Roughness, "Roughness", "거칠기").Range(0.01f, 1.0f, 0.005f)
		.AsComponent();
}