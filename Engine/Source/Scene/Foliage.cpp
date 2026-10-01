#include "Scene/Foliage.h"

#include "Core/FileSystem.h"
#include "Core/Log.h"
#include "Core/Paths.h"
#include "Core/Reflection/TypeInfo.h"
#include "Core/StringConv.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "Scene/Terrain.h"

#include <json.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <fstream>

namespace
{
	using nlohmann::json;

	// 지면 법선 정렬: 위(+Z)를 Normal로 돌리는 최단 회전
	FQuat MakeAlignRotation(const FVector3& Normal)
	{
		const FVector3 N   = Normal.GetNormalized();
		const float    Cos = FMath::Clamp(FVector3::Dot(FVector3::UpVector, N), -1.0f, 1.0f);
		if (Cos > 0.9999f)
		{
			return FQuat::Identity;
		}
		const FVector3 Axis  = FVector3::Cross(FVector3::UpVector, N).GetNormalized();
		const float    Angle = std::acos(Cos);
		FQuat          Q     = FQuat::FromAxisAngle(Axis, Angle);
		if (FVector3::Dot(Q.RotateVector(FVector3::UpVector), N) < Cos) // 회전 방향 규약이 반대면
		{
			Q = FQuat::FromAxisAngle(Axis, -Angle);
		}
		return Q;
	}

	void TypeToJson(const FFoliageType& Type, json& Out)
	{
		Out["Name"]            = Type.Name;
		Out["Mesh"]            = Type.Mesh;
		Out["Material"]        = Type.Material;
		Out["Density"]         = Type.Density;
		Out["MinScale"]        = Type.MinScale;
		Out["MaxScale"]        = Type.MaxScale;
		Out["MaxSlope"]        = Type.MaxSlope;
		Out["MinHeight"]       = Type.MinHeight;
		Out["MaxHeight"]       = Type.MaxHeight;
		Out["AlignToNormal"]   = Type.bAlignToNormal;
		Out["RandomYaw"]       = Type.bRandomYaw;
		Out["ZOffset"]         = Type.ZOffset;
		Out["CullDistance"]    = Type.CullDistance;
		Out["ShadowDistance"]  = Type.ShadowDistance;
		Out["Collision"]       = Type.bCollision;
		Out["CollisionRadius"] = Type.CollisionRadius;
		Out["CollisionHeight"] = Type.CollisionHeight;
	}

	void TypeFromJson(const json& In, FFoliageType& Type)
	{
		const FFoliageType Defaults;
		Type.Name            = In.value("Name", Defaults.Name);
		Type.Mesh            = In.value("Mesh", Defaults.Mesh);
		Type.Material        = In.value("Material", Defaults.Material);
		Type.Density         = In.value("Density", Defaults.Density);
		Type.MinScale        = In.value("MinScale", Defaults.MinScale);
		Type.MaxScale        = In.value("MaxScale", Defaults.MaxScale);
		Type.MaxSlope        = In.value("MaxSlope", Defaults.MaxSlope);
		Type.MinHeight       = In.value("MinHeight", Defaults.MinHeight);
		Type.MaxHeight       = In.value("MaxHeight", Defaults.MaxHeight);
		Type.bAlignToNormal  = In.value("AlignToNormal", Defaults.bAlignToNormal);
		Type.bRandomYaw      = In.value("RandomYaw", Defaults.bRandomYaw);
		Type.ZOffset         = In.value("ZOffset", Defaults.ZOffset);
		Type.CullDistance    = In.value("CullDistance", Defaults.CullDistance);
		Type.ShadowDistance  = In.value("ShadowDistance", Defaults.ShadowDistance);
		Type.bCollision      = In.value("Collision", Defaults.bCollision);
		Type.CollisionRadius = In.value("CollisionRadius", Defaults.CollisionRadius);
		Type.CollisionHeight = In.value("CollisionHeight", Defaults.CollisionHeight);
	}
} // namespace

size_t FFoliageAsset::GetInstanceCount() const
{
	size_t Count = 0;
	for (const std::vector<FFoliageInstance>& List : Instances)
	{
		Count += List.size();
	}
	return Count;
}

// ---- FoliageMath --------------------------------------------------------------------------------------------------

FMatrix4x4 FoliageMath::MakeWorldMatrix(const FFoliageInstance& Instance, const FFoliageType& Type, float FadeScale)
{
	const FQuat Yaw      = FQuat::FromEuler(0.0f, Instance.Yaw, 0.0f);
	const FQuat Rotation = Type.bAlignToNormal ? MakeAlignRotation(Instance.Normal) * Yaw : Yaw; // Yaw 먼저
	const float Scale    = Instance.Scale * FadeScale;
	return FMatrix4x4::MakeTransform(Instance.Position + Rotation.RotateVector(FVector3(0.0f, 0.0f, Type.ZOffset * Instance.Scale)), Rotation,
	                                 FVector3(Scale));
}

float FoliageMath::ComputeFade(float Distance, float CullDistance, float FadeFraction)
{
	if (CullDistance <= 0.0f || Distance >= CullDistance)
	{
		return 0.0f;
	}
	const float FadeStart = CullDistance * (1.0f - FMath::Clamp(FadeFraction, 0.0f, 1.0f));
	if (Distance <= FadeStart)
	{
		return 1.0f;
	}
	const float T = (Distance - FadeStart) / std::max(CullDistance - FadeStart, 1.0e-3f);
	const float S = 1.0f - T;
	return S * S * (3.0f - 2.0f * S);
}

bool FoliageMath::AcceptsSurface(const FFoliageType& Type, const FVector3& Position, const FVector3& Normal)
{
	const float SlopeDegrees = FMath::RadiansToDegrees(std::acos(FMath::Clamp(Normal.GetNormalized().Z, -1.0f, 1.0f)));
	if (SlopeDegrees > Type.MaxSlope)
	{
		return false;
	}
	return Type.MinHeight >= Type.MaxHeight || (Position.Z >= Type.MinHeight && Position.Z <= Type.MaxHeight);
}

uint32 FoliageMath::CountInCircle(const std::vector<FFoliageInstance>& Instances, const FVector2& Center, float Radius)
{
	const float RadiusSquared = Radius * Radius;
	uint32      Count         = 0;
	for (const FFoliageInstance& Instance : Instances)
	{
		const float DX = Instance.Position.X - Center.X;
		const float DY = Instance.Position.Y - Center.Y;
		Count += DX * DX + DY * DY <= RadiusSquared ? 1u : 0u;
	}
	return Count;
}

uint32 FoliageMath::Paint(FFoliageAsset& Asset, uint32 TypeIndex, const FVector2& Center, float Radius, float DensityScale, uint32 MaxAdd,
                          const FFoliageSurfaceQuery& Surface, std::mt19937& Random)
{
	if (TypeIndex >= Asset.Types.size() || Radius <= 0.0f || !Surface)
	{
		return 0;
	}
	Asset.EnsureInstanceLists();
	const FFoliageType&            Type      = Asset.Types[TypeIndex];
	std::vector<FFoliageInstance>& Instances = Asset.Instances[TypeIndex];
	const float                    Area      = FMath::Pi * Radius * Radius / 1.0e6f; // 100 m² = 1e6 cm² 단위
	const uint32                   Target    = static_cast<uint32>(std::max(0.0f, Type.Density * DensityScale * Area));
	const uint32                   Existing  = CountInCircle(Instances, Center, Radius);
	if (Existing >= Target)
	{
		return 0;
	}
	std::uniform_real_distribution<float> Unit(0.0f, 1.0f);
	const uint32                          Wanted   = std::min(Target - Existing, MaxAdd);
	uint32                                Added    = 0;
	const uint32                          Attempts = Wanted * 3; // 경사/높이 제한으로 버려지는 것 감안
	for (uint32 Attempt = 0; Attempt < Attempts && Added < Wanted; ++Attempt)
	{
		// 원 안 균일 분포
		const float R     = Radius * std::sqrt(Unit(Random));
		const float Theta = FMath::TwoPi * Unit(Random);
		const float X     = Center.X + R * std::cos(Theta);
		const float Y     = Center.Y + R * std::sin(Theta);
		FVector3    Position;
		FVector3    Normal;
		if (!Surface(X, Y, Position, Normal) || !AcceptsSurface(Type, Position, Normal))
		{
			continue;
		}
		FFoliageInstance& Instance = Instances.emplace_back();
		Instance.Position          = Position;
		Instance.Normal            = Normal;
		Instance.Yaw               = Type.bRandomYaw ? Unit(Random) * 360.0f : 0.0f;
		Instance.Scale             = FMath::Lerp(Type.MinScale, std::max(Type.MinScale, Type.MaxScale), Unit(Random));
		++Added;
	}
	if (Added > 0)
	{
		Asset.MarkChanged(static_cast<int32>(TypeIndex));
	}
	return Added;
}

uint32 FoliageMath::Erase(FFoliageAsset& Asset, uint32 TypeIndex, const FVector2& Center, float Radius)
{
	const float RadiusSquared = Radius * Radius;
	uint32      Removed       = 0;
	for (uint32 Index = 0; Index < Asset.Instances.size(); ++Index)
	{
		if (TypeIndex < Asset.Types.size() && Index != TypeIndex)
		{
			continue;
		}
		std::vector<FFoliageInstance>& List = Asset.Instances[Index];
		const size_t                   Before = List.size();
		List.erase(std::remove_if(List.begin(), List.end(),
		                          [&](const FFoliageInstance& Instance) {
			                          const float DX = Instance.Position.X - Center.X;
			                          const float DY = Instance.Position.Y - Center.Y;
			                          return DX * DX + DY * DY <= RadiusSquared;
		                          }),
		           List.end());
		Removed += static_cast<uint32>(Before - List.size());
	}
	if (Removed > 0)
	{
		Asset.MarkChanged(TypeIndex < Asset.Types.size() ? static_cast<int32>(TypeIndex) : -1);
	}
	return Removed;
}

// ---- FoliageIO ----------------------------------------------------------------------------------------------------

std::string FoliageIO::ToJsonString(const FFoliageAsset& Asset)
{
	json Root;
	Root["Version"]  = Version;
	Root["Revision"] = Asset.Revision;
	json Types       = json::array();
	for (size_t Index = 0; Index < Asset.Types.size(); ++Index)
	{
		json Type;
		TypeToJson(Asset.Types[Index], Type);
		const std::vector<FFoliageInstance>* List = Index < Asset.Instances.size() ? &Asset.Instances[Index] : nullptr;
		Type["InstanceCount"]                    = List != nullptr ? List->size() : 0;
		// 인스턴스: 실수 8개 (위치 XYZ, Yaw, Scale, 법선 XYZ) 리틀 엔디언
		Type["Instances"] = List != nullptr && !List->empty()
		                        ? TerrainIO::EncodeBase64(reinterpret_cast<const uint8*>(List->data()), List->size() * sizeof(FFoliageInstance))
		                        : std::string();
		Types.push_back(std::move(Type));
	}
	Root["Types"] = std::move(Types);
	return Root.dump(1, '\t') + "\n";
}

bool FoliageIO::FromJsonString(std::string_view Text, FFoliageAsset& OutAsset, std::string* OutError)
{
	auto Fail = [&](const std::string& Message) {
		if (OutError != nullptr)
		{
			*OutError = Message;
		}
		return false;
	};
	const json Root = json::parse(Text.begin(), Text.end(), nullptr, false);
	if (!Root.is_object() || !Root.contains("Types") || !Root["Types"].is_array())
	{
		return Fail("JSON 형식 오류");
	}
	FFoliageAsset Asset;
	Asset.Revision = Root.value("Revision", 0u);
	for (const json& TypeJson : Root["Types"])
	{
		FFoliageType& Type = Asset.Types.emplace_back();
		TypeFromJson(TypeJson, Type);
		std::vector<FFoliageInstance>& List = Asset.Instances.emplace_back();
		std::vector<uint8>             Bytes;
		if (!TerrainIO::DecodeBase64(TypeJson.value("Instances", std::string()), Bytes) || Bytes.size() % sizeof(FFoliageInstance) != 0)
		{
			return Fail("인스턴스 데이터 오류: " + Type.Name);
		}
		List.resize(Bytes.size() / sizeof(FFoliageInstance));
		if (!Bytes.empty())
		{
			std::memcpy(List.data(), Bytes.data(), Bytes.size());
		}
	}
	OutAsset = std::move(Asset);
	return true;
}

bool FoliageIO::SaveToFile(const FFoliageAsset& Asset, const std::filesystem::path& Path)
{
	std::error_code ErrorCode;
	if (Path.has_parent_path())
	{
		std::filesystem::create_directories(Path.parent_path(), ErrorCode);
	}
	std::ofstream File(Path, std::ios::binary | std::ios::trunc);
	if (!File)
	{
		return false;
	}
	const std::string Text = ToJsonString(Asset);
	File.write(Text.data(), static_cast<std::streamsize>(Text.size()));
	return File.good();
}

bool FoliageIO::LoadFromFile(const std::filesystem::path& Path, FFoliageAsset& OutAsset, std::string* OutError)
{
	std::string Text;
	if (!FFileSystem::ReadTextFile(Path, Text))
	{
		if (OutError != nullptr)
		{
			*OutError = "파일을 읽을 수 없음";
		}
		return false;
	}
	return FromJsonString(Text, OutAsset, OutError);
}

// ---- FFoliageLibrary ----------------------------------------------------------------------------------------------

FFoliageLibrary& FFoliageLibrary::Get()
{
	static FFoliageLibrary Instance;
	return Instance;
}

void FFoliageLibrary::SetContentDirectory(const std::filesystem::path& Directory)
{
	ContentDirectory = Directory;
}

std::filesystem::path FFoliageLibrary::ResolveAssetPath(const std::string& Asset) const
{
	const std::filesystem::path Path = FStringConv::ToWide(Asset);
	if (Path.is_absolute())
	{
		return Path;
	}
	if (!ContentDirectory.empty())
	{
		return ContentDirectory / Path;
	}
	return FPaths::HasProject() ? FPaths::GetProjectContentDirectory() / Path : Path;
}

std::string FFoliageLibrary::MakeAssetPath(const std::filesystem::path& AbsolutePath) const
{
	const std::filesystem::path Root     = !ContentDirectory.empty() ? ContentDirectory : (FPaths::HasProject() ? FPaths::GetProjectContentDirectory() : std::filesystem::path());
	const std::filesystem::path Relative = Root.empty() ? std::filesystem::path() : AbsolutePath.lexically_normal().lexically_relative(Root.lexically_normal());
	if (Relative.empty() || Relative.native().starts_with(L".."))
	{
		return FStringConv::ToUtf8(AbsolutePath.generic_wstring());
	}
	return FStringConv::ToUtf8(Relative.generic_wstring());
}

std::shared_ptr<FFoliageAsset> FFoliageLibrary::Find(const std::string& Asset) const
{
	const auto Found = Cache.find(Asset);
	return Found != Cache.end() ? Found->second : nullptr;
}

std::shared_ptr<FFoliageAsset> FFoliageLibrary::Load(const std::string& Asset)
{
	if (Asset.empty())
	{
		return nullptr;
	}
	if (const auto Found = Cache.find(Asset); Found != Cache.end())
	{
		return Found->second;
	}
	auto        Data = std::make_shared<FFoliageAsset>();
	std::string Error;
	if (!FoliageIO::LoadFromFile(ResolveAssetPath(Asset), *Data, &Error))
	{
		E_LOG(LogTerrain, Warning, "폴리지 로드 실패 '{}': {}", Asset, Error);
		Cache[Asset] = nullptr;
		return nullptr;
	}
	E_LOG(LogTerrain, Log, "폴리지 로드: {} (타입 {}개, 인스턴스 {}개)", Asset, Data->Types.size(), Data->GetInstanceCount());
	Cache[Asset] = Data;
	return Data;
}

std::shared_ptr<FFoliageAsset> FFoliageLibrary::Create(const std::string& Asset, FFoliageAsset Initial)
{
	auto Data = std::make_shared<FFoliageAsset>(std::move(Initial));
	Data->EnsureInstanceLists();
	Cache[Asset] = Data;
	return Save(Asset) ? Data : nullptr;
}

bool FFoliageLibrary::Save(const std::string& Asset)
{
	const std::shared_ptr<FFoliageAsset> Data = Find(Asset);
	if (!Data || !FoliageIO::SaveToFile(*Data, ResolveAssetPath(Asset)))
	{
		return false;
	}
	Data->bUnsaved = false;
	E_LOG(LogTerrain, Log, "폴리지 저장: {} (인스턴스 {}개)", Asset, Data->GetInstanceCount());
	return true;
}

void FFoliageLibrary::SaveAllUnsaved()
{
	for (const auto& [Asset, Data] : Cache)
	{
		if (Data && Data->bUnsaved)
		{
			Save(Asset);
		}
	}
}

void FFoliageLibrary::OnAssetMoved(const std::filesystem::path& From, const std::filesystem::path& To)
{
	const std::wstring                               FromKey = From.lexically_normal().generic_wstring();
	std::vector<std::pair<std::string, std::string>> Renames;
	for (const auto& [Asset, Data] : Cache)
	{
		const std::wstring Path = ResolveAssetPath(Asset).lexically_normal().generic_wstring();
		if (Path.size() < FromKey.size() || _wcsnicmp(Path.c_str(), FromKey.c_str(), FromKey.size()) != 0 ||
		    (Path.size() > FromKey.size() && Path[FromKey.size()] != L'/'))
		{
			continue;
		}
		Renames.emplace_back(Asset, MakeAssetPath(std::filesystem::path(To.generic_wstring() + Path.substr(FromKey.size()))));
	}
	for (const auto& [Old, New] : Renames)
	{
		std::shared_ptr<FFoliageAsset> Data = Cache[Old];
		Cache.erase(Old);
		Cache[New] = std::move(Data);
	}
}

void FFoliageLibrary::Invalidate(const std::string& Asset)
{
	Cache.erase(Asset);
}

void GatherFoliage(FScene& Scene, std::vector<FFoliageInstanceSet>& OutSets)
{
	OutSets.clear();
	FFoliageLibrary& Library = FFoliageLibrary::Get();
	Scene.GetRegistry().View<FFoliageComponent>().Each([&](FEntity Entity, FFoliageComponent& Component) {
		const std::shared_ptr<FFoliageAsset> Asset = Library.Load(Component.Asset);
		if (!Asset)
		{
			return;
		}
		Asset->EnsureInstanceLists();
		OutSets.push_back({ Entity, &Component, Asset.get() });
	});
}

void RegisterFoliageTypes()
{
	FTypeRegistry& Registry = FTypeRegistry::Get();
	if (Registry.IsRegistered<FFoliageComponent>())
	{
		return;
	}
	// 풀·나무: 타입/인스턴스는 .efoliage (폴리지 도구 창에서 칠하고 편집). 인스턴스는 월드 좌표
	Registry.RegisterType<FFoliageComponent>("FoliageComponent", "폴리지")
		.Property(&FFoliageComponent::Asset, "Asset", "폴리지 데이터", PF_ReadOnly).AssetFilter(".efoliage")
		.Property(&FFoliageComponent::bVisible, "Visible", "표시")
		.Property(&FFoliageComponent::EditRevision, "EditRevision", "편집 버전", PF_Hidden | PF_NoReplicate)
		.AsComponent();
}
