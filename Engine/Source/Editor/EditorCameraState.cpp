#include "Editor/EditorCameraState.h"

#include "Core/Log.h"

#include <json.hpp>

#include <fstream>
#include <sstream>

E_DECLARE_LOG_CATEGORY(LogEditor)

namespace
{
	using nlohmann::json;

	bool ReadFloats(const json& Value, float* Out, size_t Count)
	{
		if (!Value.is_array() || Value.size() != Count)
		{
			return false;
		}
		for (size_t Index = 0; Index < Count; ++Index)
		{
			if (!Value[Index].is_number())
			{
				return false;
			}
			Out[Index] = Value[Index].get<float>();
		}
		return true;
	}
} // namespace

std::string FEditorCameraState::ToJsonString() const
{
	json Document;
	Document["Position"]  = json::array({ Position.X, Position.Y, Position.Z });
	Document["Rotation"]  = json::array({ Rotation.X, Rotation.Y, Rotation.Z, Rotation.W });
	Document["MoveSpeed"] = MoveSpeed;
	return Document.dump(2);
}

bool FEditorCameraState::FromJsonString(const std::string& Json)
{
	const json Document = json::parse(Json, nullptr, false);
	if (Document.is_discarded() || !Document.is_object())
	{
		return false;
	}

	float PositionValues[3];
	float RotationValues[4];
	if (!ReadFloats(Document.value("Position", json()), PositionValues, 3) || !ReadFloats(Document.value("Rotation", json()), RotationValues, 4))
	{
		return false;
	}
	const FQuat LoadedRotation(RotationValues[0], RotationValues[1], RotationValues[2], RotationValues[3]);
	if (LoadedRotation.LengthSquared() < 0.5f)
	{
		return false;
	}

	Position = FVector3(PositionValues[0], PositionValues[1], PositionValues[2]);
	Rotation = LoadedRotation.GetNormalized();
	if (const auto Found = Document.find("MoveSpeed"); Found != Document.end() && Found->is_number())
	{
		MoveSpeed = FMath::Max(Found->get<float>(), 1.0f);
	}
	return true;
}

bool FEditorCameraState::Save(const std::filesystem::path& Path) const
{
	std::error_code ErrorCode;
	std::filesystem::create_directories(Path.parent_path(), ErrorCode);
	std::ofstream File(Path, std::ios::binary | std::ios::trunc);
	if (!File)
	{
		E_LOG(LogEditor, Warning, "에디터 카메라 상태를 저장하지 못했습니다");
		return false;
	}
	File << ToJsonString();
	return true;
}

bool FEditorCameraState::Load(const std::filesystem::path& Path)
{
	std::ifstream File(Path, std::ios::binary);
	if (!File)
	{
		return false;
	}
	std::stringstream Buffer;
	Buffer << File.rdbuf();
	if (!FromJsonString(Buffer.str()))
	{
		E_LOG(LogEditor, Warning, "에디터 카메라 상태 파일 형식이 잘못되어 무시합니다");
		return false;
	}
	return true;
}

FVector3 FEditorCameraState::ComputeFramingPosition(const FBox& Bounds, const FVector3& Forward, float FovYDegrees, float AspectRatio)
{
	const FVector3 Center = Bounds.GetCenter();
	// 경계 구가 좁은 쪽 시야각 안에 들어오는 거리 (최소 50cm)
	const float Radius    = FMath::Max(Bounds.GetExtent().Length(), 50.0f);
	const float HalfFovY  = FMath::DegreesToRadians(FovYDegrees) * 0.5f;
	const float HalfFovX  = std::atan(FMath::Tan(HalfFovY) * FMath::Max(AspectRatio, 0.01f));
	const float HalfFov   = FMath::Min(HalfFovX, HalfFovY);
	const float Distance  = Radius / FMath::Sin(HalfFov);
	const FVector3 Direction = Forward.GetNormalized();
	return Center - Direction * Distance;
}
