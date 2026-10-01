#include "Editor/TerrainDemoGenerator.h"

#include "Core/Log.h"
#include "Editor/Panels/FoliageToolPanel.h"
#include "Scene/Foliage.h"
#include "Core/StringConv.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "Scene/SceneSerializer.h"
#include "Scene/Terrain.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <string>
#include <vector>

E_DECLARE_LOG_CATEGORY(LogEditor)

namespace
{
	constexpr uint32 DemoResolution = 513;
	constexpr float  DemoSize       = 20000.0f; // cm (200m)
	constexpr float  DemoRange      = 8000.0f;  // cm (±40m)
	constexpr uint32 TextureSize    = 256;

	uint32 Hash(int32 X, int32 Y, uint32 Seed)
	{
		uint32 H = static_cast<uint32>(X) * 0x8DA6B343u ^ static_cast<uint32>(Y) * 0xD8163841u ^ Seed * 0xCB1AB31Fu;
		H ^= H >> 13;
		H *= 0x5BD1E995u;
		H ^= H >> 15;
		return H;
	}

	float Smooth(float T) { return T * T * (3.0f - 2.0f * T); }

	// 격자 주기 Period로 반복되는 값 잡음 (0~1) — 텍스처 이음매 없음. Period 0이면 반복 없음
	float Noise(float X, float Y, uint32 Seed, int32 Period)
	{
		const int32 IX = static_cast<int32>(std::floor(X));
		const int32 IY = static_cast<int32>(std::floor(Y));
		const float FX = Smooth(X - std::floor(X));
		const float FY = Smooth(Y - std::floor(Y));
		auto        Corner = [&](int32 CX, int32 CY) {
            if (Period > 0)
            {
                CX = ((CX % Period) + Period) % Period;
                CY = ((CY % Period) + Period) % Period;
            }
            return static_cast<float>(Hash(CX, CY, Seed) & 0xFFFFu) / 65535.0f;
		};
		const float A = Corner(IX, IY) + (Corner(IX + 1, IY) - Corner(IX, IY)) * FX;
		const float B = Corner(IX, IY + 1) + (Corner(IX + 1, IY + 1) - Corner(IX, IY + 1)) * FX;
		return A + (B - A) * FY;
	}

	// 여러 옥타브 (0~1)
	float Fbm(float X, float Y, uint32 Seed, int32 Octaves, int32 Period)
	{
		float Sum = 0.0f, Amplitude = 0.5f, Total = 0.0f;
		for (int32 Octave = 0; Octave < Octaves; ++Octave)
		{
			Sum += Noise(X, Y, Seed + static_cast<uint32>(Octave) * 31u, Period) * Amplitude;
			Total += Amplitude;
			X *= 2.0f;
			Y *= 2.0f;
			Period *= 2;
			Amplitude *= 0.5f;
		}
		return Sum / Total;
	}

	float SmoothStep(float Edge0, float Edge1, float X)
	{
		const float T = std::clamp((X - Edge0) / (Edge1 - Edge0), 0.0f, 1.0f);
		return Smooth(T);
	}

	uint8 ToSrgb8(float Linear)
	{
		Linear        = std::clamp(Linear, 0.0f, 1.0f);
		const float S = Linear <= 0.0031308f ? Linear * 12.92f : 1.055f * std::pow(Linear, 1.0f / 2.4f) - 0.055f;
		return static_cast<uint8>(std::lround(S * 255.0f));
	}

	uint32 Pack(uint8 R, uint8 G, uint8 B, uint8 A = 255)
	{
		return static_cast<uint32>(R) | (static_cast<uint32>(G) << 8) | (static_cast<uint32>(B) << 16) | (static_cast<uint32>(A) << 24);
	}

	bool WriteBytes(const std::filesystem::path& Path, const std::vector<uint8>& Bytes)
	{
		std::error_code ErrorCode;
		std::filesystem::create_directories(Path.parent_path(), ErrorCode);
		std::ofstream File(Path, std::ios::binary | std::ios::trunc);
		File.write(reinterpret_cast<const char*>(Bytes.data()), static_cast<std::streamsize>(Bytes.size()));
		return File.good();
	}

	bool WriteText(const std::filesystem::path& Path, const std::string& Text)
	{
		return WriteBytes(Path, std::vector<uint8>(Text.begin(), Text.end()));
	}

	// 지면 재질 하나: 색(선형 두 색 사이) + 표면 높이 → 베이스 컬러/노멀 PNG
	struct FSurfaceRecipe
	{
		const char* Name;
		float       ColorA[3];
		float       ColorB[3];
		float       Roughness;
		float       BumpStrength;
		uint32      Seed;
		int32       Kind; // 0 풀, 1 흙, 2 바위
	};

	float SurfaceHeight(const FSurfaceRecipe& Recipe, float U, float V)
	{
		switch (Recipe.Kind)
		{
		case 0: // 풀: 고운 잎 무늬 + 덩어리
			return 0.6f * Fbm(U * 32.0f, V * 32.0f, Recipe.Seed, 3, 32) + 0.4f * Fbm(U * 6.0f, V * 6.0f, Recipe.Seed + 7, 3, 6);
		case 1: // 흙: 자갈 알갱이
			return 0.5f * Fbm(U * 12.0f, V * 12.0f, Recipe.Seed, 4, 12) + 0.5f * std::pow(Noise(U * 48.0f, V * 48.0f, Recipe.Seed + 3, 48), 3.0f);
		default: // 바위: 능선 잡음 (갈라진 면)
		{
			const float Ridge = 1.0f - std::abs(Fbm(U * 8.0f, V * 8.0f, Recipe.Seed, 5, 8) * 2.0f - 1.0f);
			return 0.7f * Ridge * Ridge + 0.3f * Fbm(U * 24.0f, V * 24.0f, Recipe.Seed + 5, 3, 24);
		}
		}
	}

	bool WriteSurface(const std::filesystem::path& Directory, const FSurfaceRecipe& Recipe)
	{
		std::vector<float> Heights(TextureSize * TextureSize);
		for (uint32 Y = 0; Y < TextureSize; ++Y)
		{
			for (uint32 X = 0; X < TextureSize; ++X)
			{
				Heights[Y * TextureSize + X] = SurfaceHeight(Recipe, static_cast<float>(X) / TextureSize, static_cast<float>(Y) / TextureSize);
			}
		}
		std::vector<uint32> Base(TextureSize * TextureSize);
		std::vector<uint32> Normal(TextureSize * TextureSize);
		for (uint32 Y = 0; Y < TextureSize; ++Y)
		{
			for (uint32 X = 0; X < TextureSize; ++X)
			{
				const float H     = Heights[Y * TextureSize + X];
				const float Tint  = Fbm(static_cast<float>(X) / TextureSize * 4.0f, static_cast<float>(Y) / TextureSize * 4.0f, Recipe.Seed + 11, 2, 4);
				const float Blend = std::clamp(0.65f * H + 0.35f * Tint, 0.0f, 1.0f);
				Base[Y * TextureSize + X] =
					Pack(ToSrgb8(Recipe.ColorA[0] + (Recipe.ColorB[0] - Recipe.ColorA[0]) * Blend), ToSrgb8(Recipe.ColorA[1] + (Recipe.ColorB[1] - Recipe.ColorA[1]) * Blend),
					     ToSrgb8(Recipe.ColorA[2] + (Recipe.ColorB[2] - Recipe.ColorA[2]) * Blend));
				// 노멀: 탄젠트 공간 (+X = +U, +Y = 이미지 위쪽 = 행 감소)
				const float Left  = Heights[Y * TextureSize + (X + TextureSize - 1) % TextureSize];
				const float Right = Heights[Y * TextureSize + (X + 1) % TextureSize];
				const float Up    = Heights[((Y + TextureSize - 1) % TextureSize) * TextureSize + X];
				const float Down  = Heights[((Y + 1) % TextureSize) * TextureSize + X];
				float       NX    = -(Right - Left) * 0.5f * Recipe.BumpStrength;
				float       NY    = -(Up - Down) * 0.5f * Recipe.BumpStrength;
				const float Length = std::sqrt(NX * NX + NY * NY + 1.0f);
				NX /= Length;
				NY /= Length;
				const float NZ = 1.0f / Length;
				Normal[Y * TextureSize + X] = Pack(static_cast<uint8>(std::lround((NX * 0.5f + 0.5f) * 255.0f)),
				                                   static_cast<uint8>(std::lround((NY * 0.5f + 0.5f) * 255.0f)),
				                                   static_cast<uint8>(std::lround((NZ * 0.5f + 0.5f) * 255.0f)));
			}
		}
		const std::string Name = Recipe.Name;
		return WriteBytes(Directory / "Textures" / FStringConv::ToWide(Name + "_Base.png"), TerrainIO::EncodePngRgba8(Base, TextureSize, TextureSize)) &&
		       WriteBytes(Directory / "Textures" / FStringConv::ToWide(Name + "_Normal.png"), TerrainIO::EncodePngRgba8(Normal, TextureSize, TextureSize)) &&
		       WriteText(Directory / "Materials" / FStringConv::ToWide(Name + ".emat"),
		                 std::format("{{\n  \"Name\": \"Terrain{}\",\n  \"BaseColorFactor\": [1.0, 1.0, 1.0, 1.0],\n  \"EmissiveFactor\": [0.0, 0.0, 0.0],\n"
		                             "  \"Metallic\": 0.0,\n  \"Roughness\": {:.2f},\n  \"NormalScale\": 1.0,\n  \"OcclusionStrength\": 1.0,\n"
		                             "  \"BaseColorTexture\": \"../Textures/{}_Base.png\",\n  \"MetallicRoughnessTexture\": \"\",\n"
		                             "  \"NormalTexture\": \"../Textures/{}_Normal.png\",\n  \"OcclusionTexture\": \"\",\n  \"EmissiveTexture\": \"\"\n}}\n",
		                             Name, Recipe.Roughness, Name, Name));
	}

	// 데모 지형 높이 (cm, 월드 XY cm — 가운데 원점)
	float DemoHeight(float X, float Y)
	{
		const float XM = X / 100.0f;
		const float YM = Y / 100.0f;
		float       H  = (Fbm(XM / 55.0f + 10.0f, YM / 55.0f + 10.0f, 7u, 5, 0) - 0.5f) * 1800.0f;
		// 북동쪽 산
		const float MountainDistance = std::sqrt((XM - 55.0f) * (XM - 55.0f) + (YM - 45.0f) * (YM - 45.0f));
		const float Ridge            = 1.0f - std::abs(Fbm(XM / 25.0f, YM / 25.0f, 19u, 4, 0) * 2.0f - 1.0f);
		H += SmoothStep(55.0f, 5.0f, MountainDistance) * (2200.0f + Ridge * 900.0f);
		// 남서쪽 언덕
		const float HillDistance = std::sqrt((XM + 50.0f) * (XM + 50.0f) + (YM + 55.0f) * (YM + 55.0f));
		H += SmoothStep(40.0f, 0.0f, HillDistance) * 900.0f;
		// 가운데 평지 (캐릭터 시작)
		const float CenterDistance = std::sqrt(XM * XM + YM * YM);
		H                          = H * SmoothStep(12.0f, 26.0f, CenterDistance);
		return std::clamp(H, -DemoRange * 0.45f, DemoRange * 0.45f);
	}

	// 흙길: y = 18 sin(x / 22) m 곡선까지 거리 (m)
	float PathDistance(float XM, float YM)
	{
		return std::abs(YM - 18.0f * std::sin(XM / 22.0f)) * 0.8f;
	}
} // namespace

bool GenerateTerrainDemo(const std::filesystem::path& ContentDirectory)
{
	const std::filesystem::path TerrainDirectory = ContentDirectory / L"Terrain";
	const FSurfaceRecipe        Recipes[]        = {
        { "Grass", { 0.020f, 0.040f, 0.008f }, { 0.070f, 0.110f, 0.025f }, 0.95f, 6.0f, 101u, 0 },
        { "Dirt", { 0.07f, 0.045f, 0.025f }, { 0.18f, 0.12f, 0.07f }, 0.9f, 8.0f, 202u, 1 },
        { "Rock", { 0.05f, 0.048f, 0.045f }, { 0.22f, 0.21f, 0.19f }, 0.8f, 10.0f, 303u, 2 },
	};
	for (const FSurfaceRecipe& Recipe : Recipes)
	{
		if (!WriteSurface(TerrainDirectory, Recipe))
		{
			E_LOG(LogEditor, Error, "지형 데모: 텍스처/머티리얼 쓰기 실패 ({})", Recipe.Name);
			return false;
		}
	}

	// 높이맵 + 레이어 가중치 (0 풀, 1 흙길, 2 바위 비탈, 3 높은 곳 눈)
	FTerrainComponent Component;
	Component.Size        = FVector2(DemoSize, DemoSize);
	Component.HeightRange = DemoRange;
	FTerrainData Data;
	Data.Initialize(DemoResolution);
	const FTerrainFrame Frame = FTerrainFrame::Make(FVector3(), Component, DemoResolution);
	for (uint32 Y = 0; Y < DemoResolution; ++Y)
	{
		for (uint32 X = 0; X < DemoResolution; ++X)
		{
			const FVector3 World = Frame.GridToWorld(static_cast<float>(X), static_cast<float>(Y), 0.0f);
			Data.Heights[Y * DemoResolution + X] =
				static_cast<uint16>(std::clamp(std::lround(Frame.WorldZToHeight(DemoHeight(World.X, World.Y))), 0L, 65535L));
		}
	}
	for (uint32 Y = 0; Y < DemoResolution; ++Y)
	{
		for (uint32 X = 0; X < DemoResolution; ++X)
		{
			const FVector3 World  = Frame.GridToWorld(static_cast<float>(X), static_cast<float>(Y), 0.0f);
			const FVector3 Normal = TerrainMath::ComputeNormal(Data, Frame, static_cast<float>(X), static_cast<float>(Y));
			const float    Height = Frame.HeightToWorldZ(Data.GetHeight(static_cast<int32>(X), static_cast<int32>(Y)));
			const float    XM     = World.X / 100.0f;
			const float    YM     = World.Y / 100.0f;
			const float    Jitter = Fbm(XM / 6.0f, YM / 6.0f, 55u, 3, 0) - 0.5f;
			const float    Rock   = SmoothStep(0.80f, 0.68f, Normal.Z + Jitter * 0.12f);
			const float    Snow   = SmoothStep(1900.0f, 2300.0f, Height + Jitter * 300.0f) * SmoothStep(0.6f, 0.8f, Normal.Z);
			const float    Path   = SmoothStep(3.0f, 1.2f, PathDistance(XM, YM) + Jitter * 2.0f) * (1.0f - Rock);
			float          W[4]   = { 1.0f, Path, Rock, Snow };
			W[0]                  = std::max(0.0f, 1.0f - W[1] - W[2] - W[3]);
			uint32 Packed         = 0;
			for (uint32 Layer = 0; Layer < 4; ++Layer)
			{
				Packed |= static_cast<uint32>(std::lround(std::clamp(W[Layer], 0.0f, 1.0f) * 255.0f)) << (Layer * 8);
			}
			Data.Weights[Y * DemoResolution + X] = TerrainMath::NormalizeWeight(Packed);
		}
	}
	if (!TerrainIO::SaveToFile(Data, TerrainDirectory / L"DemoTerrain.eterrain"))
	{
		return false;
	}
	FTerrainLibrary::Get().Invalidate("Terrain/DemoTerrain.eterrain"); // 열린 데이터가 있으면 새 파일로

	// 폴리지: 기본 타입 5개(풀/덤불/활엽수/침엽수/바위)를 규칙으로 흩뿌린다 (레이어 가중치·경사·높이·잡음 무리)
	FFoliageAsset Foliage = FFoliageToolPanel::MakeDefaultAsset();
	{
		uint32 State = 777u;
		auto   Random = [&State]() {
            State = Hash(static_cast<int32>(State), 17, 3u);
            return static_cast<float>(State & 0xFFFFFFu) / static_cast<float>(0xFFFFFF);
		};
		const float Half = DemoSize * 0.5f;
		// (타입, 표본 간격 cm, 기본 확률)
		struct FScatter
		{
			uint32 Type;
			float  Spacing;
			float  Chance;
		};
		const FScatter Scatters[] = { { 0, 70.0f, 0.8f }, { 1, 450.0f, 0.5f }, { 2, 700.0f, 0.9f }, { 3, 600.0f, 1.0f }, { 4, 700.0f, 0.6f } };
		for (const FScatter& Scatter : Scatters)
		{
			const FFoliageType& Type = Foliage.Types[Scatter.Type];
			for (float Y = -Half + Scatter.Spacing * 0.5f; Y < Half; Y += Scatter.Spacing)
			{
				for (float X = -Half + Scatter.Spacing * 0.5f; X < Half; X += Scatter.Spacing)
				{
					const float PX = X + (Random() - 0.5f) * Scatter.Spacing;
					const float PY = Y + (Random() - 0.5f) * Scatter.Spacing;
					const FVector2 Grid = Frame.WorldToGrid(PX, PY);
					if (Grid.X < 1.0f || Grid.Y < 1.0f || Grid.X > DemoResolution - 2.0f || Grid.Y > DemoResolution - 2.0f)
					{
						continue;
					}
					const FVector3 Normal = TerrainMath::ComputeNormal(Data, Frame, Grid.X, Grid.Y);
					const FVector3 Position(PX, PY, TerrainMath::SampleWorldHeight(Data, Frame, PX, PY));
					if (!FoliageMath::AcceptsSurface(Type, Position, Normal))
					{
						continue;
					}
					const uint32 Weight = Data.GetWeight(static_cast<int32>(Grid.X + 0.5f), static_cast<int32>(Grid.Y + 0.5f));
					const float  Grass  = TerrainMath::GetLayerWeight(Weight, 0) / 255.0f;
					const float  RockW  = TerrainMath::GetLayerWeight(Weight, 2) / 255.0f;
					const float  XM = PX / 100.0f, YM = PY / 100.0f;
					const float  Clump  = Fbm(XM / 14.0f + 3.0f, YM / 14.0f + 3.0f, 91u + Scatter.Type, 3, 0);
					const float  CenterDistance = std::sqrt(XM * XM + YM * YM);
					float        Chance = Scatter.Chance;
					switch (Scatter.Type)
					{
					case 0: Chance *= Grass * SmoothStep(0.3f, 0.55f, Clump); break;                                        // 풀: 무리
					case 1: Chance *= Grass * SmoothStep(0.45f, 0.65f, Clump); break;                                       // 덤불
					case 2: Chance *= Grass * SmoothStep(0.42f, 0.55f, Clump) * SmoothStep(15.0f, 25.0f, CenterDistance); break; // 활엽수 숲
					case 3: Chance *= SmoothStep(250.0f, 600.0f, Position.Z) * SmoothStep(2000.0f, 1600.0f, Position.Z); break; // 산 중턱 침엽수
					default: Chance *= std::max(RockW, 0.08f) * SmoothStep(0.4f, 0.6f, Clump); break;                       // 바위
					}
					if (Random() >= Chance)
					{
						continue;
					}
					FFoliageInstance& Instance = Foliage.Instances[Scatter.Type].emplace_back();
					Instance.Position          = Position;
					Instance.Normal            = Normal;
					Instance.Yaw               = Random() * 360.0f;
					Instance.Scale             = FMath::Lerp(Type.MinScale, Type.MaxScale, Random());
				}
			}
		}
	}
	// 내장 폴리지 메시용 머티리얼: 흰색(정점 색 그대로) + 거친 표면 (기본 머티리얼 거칠기 0.5는 잎이 번들거린다)
	WriteText(ContentDirectory / L"Foliage" / L"Foliage.emat",
	          "{\n  \"Name\": \"Foliage\",\n  \"BaseColorFactor\": [1.0, 1.0, 1.0, 1.0],\n  \"EmissiveFactor\": [0.0, 0.0, 0.0],\n  \"Metallic\": 0.0,\n"
	          "  \"Roughness\": 0.92,\n  \"NormalScale\": 1.0,\n  \"OcclusionStrength\": 1.0,\n  \"BaseColorTexture\": \"\",\n"
	          "  \"MetallicRoughnessTexture\": \"\",\n  \"NormalTexture\": \"\",\n  \"OcclusionTexture\": \"\",\n  \"EmissiveTexture\": \"\"\n}\n");
	for (FFoliageType& Type : Foliage.Types)
	{
		Type.Material = "Foliage/Foliage.emat";
	}
	if (!FoliageIO::SaveToFile(Foliage, ContentDirectory / L"Foliage" / L"DemoFoliage.efoliage"))
	{
		return false;
	}
	FFoliageLibrary::Get().Invalidate("Foliage/DemoFoliage.efoliage");

	// 씬
	FScene        Scene;
	const FEntity Sun = Scene.CreateEntity("Sun");
	Scene.GetTransform(Sun).Position = FVector3(0.0f, 0.0f, 3000.0f);
	Scene.GetTransform(Sun).Rotation = FQuat::FromEuler(-30.0f, 150.0f, 0.0f);
	FDirectionalLightComponent& Light = Scene.GetRegistry().Emplace<FDirectionalLightComponent>(Sun);
	Light.Color                       = FVector3(1.0f, 0.95f, 0.86f);
	Light.Intensity                   = 3.2f;

	const FEntity      TerrainEntity = Scene.CreateEntity("Terrain");
	FTerrainComponent& Terrain       = Scene.GetRegistry().Emplace<FTerrainComponent>(TerrainEntity);
	Terrain                          = Component;
	Terrain.Asset                    = "Terrain/DemoTerrain.eterrain";
	Terrain.Layer0Material           = "Terrain/Materials/Grass.emat";
	Terrain.Layer1Material           = "Terrain/Materials/Dirt.emat";
	Terrain.Layer2Material           = "Terrain/Materials/Rock.emat";
	Terrain.Layer0Tiling             = 350.0f;
	Terrain.Layer1Tiling             = 250.0f;
	Terrain.Layer2Tiling             = 600.0f;

	const FEntity FoliageEntity = Scene.CreateEntity("Foliage");
	Scene.GetRegistry().Emplace<FFoliageComponent>(FoliageEntity).Asset = "Foliage/DemoFoliage.efoliage";

	const FEntity Start = Scene.CreateEntity("PlayerStart");
	Scene.GetTransform(Start).Position = FVector3(0.0f, 0.0f, TerrainMath::SampleWorldHeight(Data, Frame, 0.0f, 0.0f) + 120.0f);

	const FEntity Camera = Scene.CreateEntity("Camera");
	const FVector3 CameraPosition(600.0f, -2400.0f, 900.0f);
	const FVector3 Target(4200.0f, 1800.0f, 400.0f);
	const FVector3 Direction = (Target - CameraPosition).GetNormalized();
	Scene.GetTransform(Camera).Position = CameraPosition;
	Scene.GetTransform(Camera).Rotation =
		FQuat::FromEuler(FMath::RadiansToDegrees(FMath::Asin(Direction.Z)), FMath::RadiansToDegrees(FMath::Atan2(Direction.Y, Direction.X)), 0.0f);
	FCameraComponent& CameraComponent = Scene.GetRegistry().Emplace<FCameraComponent>(Camera);
	CameraComponent.FarZ              = 200000.0f;
	Scene.UpdateTransforms();

	if (!FSceneSerializer::SaveToFile(Scene, ContentDirectory / L"Scenes" / L"Demo_Terrain.escene"))
	{
		return false;
	}
	E_LOG(LogEditor, Display, "지형 데모 생성: Terrain/DemoTerrain.eterrain ({}x{}), 머티리얼 3개, Scenes/Demo_Terrain.escene", DemoResolution, DemoResolution);
	return true;
}
