#include "Scene/ModelMetadata.h"

#include "Core/Log.h"
#include "Core/StringConv.h"

#include <json.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>

E_DECLARE_LOG_CATEGORY(LogScene)

namespace
{
	using nlohmann::json;

	json ToJson(const FVector3& V) { return json::array({ V.X, V.Y, V.Z }); }
	json ToJson(const FQuat& Q) { return json::array({ Q.X, Q.Y, Q.Z, Q.W }); }

	FVector3 ReadVector3(const json& Node, const FVector3& Fallback)
	{
		if (!Node.is_array() || Node.size() < 3 || !Node[0].is_number() || !Node[1].is_number() || !Node[2].is_number())
		{
			return Fallback;
		}
		return FVector3(Node[0].get<float>(), Node[1].get<float>(), Node[2].get<float>());
	}

	FQuat ReadQuat(const json& Node)
	{
		if (!Node.is_array() || Node.size() < 4)
		{
			return FQuat::Identity;
		}
		for (const json& Value : Node)
		{
			if (!Value.is_number())
			{
				return FQuat::Identity;
			}
		}
		return FQuat(Node[0].get<float>(), Node[1].get<float>(), Node[2].get<float>(), Node[3].get<float>()).GetNormalized();
	}
} // namespace

const std::vector<FAnimNotify>* FModelMetadata::FindNotifies(std::string_view Clip) const
{
	for (const FClipNotifies& Entry : Clips)
	{
		if (Entry.Clip == Clip)
		{
			return &Entry.Notifies;
		}
	}
	return nullptr;
}

std::vector<FAnimNotify>& FModelMetadata::GetOrAddNotifies(std::string_view Clip)
{
	for (FClipNotifies& Entry : Clips)
	{
		if (Entry.Clip == Clip)
		{
			return Entry.Notifies;
		}
	}
	Clips.push_back({ std::string(Clip), {} });
	return Clips.back().Notifies;
}

const FModelSocket* FModelMetadata::FindSocket(std::string_view Name) const
{
	for (const FModelSocket& Socket : Sockets)
	{
		if (Socket.Name == Name)
		{
			return &Socket;
		}
	}
	return nullptr;
}

bool FModelMetadata::IsEmpty() const
{
	return Sockets.empty() && std::all_of(Clips.begin(), Clips.end(), [](const FClipNotifies& Entry) { return Entry.Notifies.empty(); });
}

std::string FModelMetadata::ToJsonString() const
{
	json Document;
	Document["Version"] = Version;
	json ClipArray      = json::array();
	for (const FClipNotifies& Entry : Clips)
	{
		if (Entry.Notifies.empty())
		{
			continue;
		}
		json Notifies = json::array();
		for (const FAnimNotify& Notify : Entry.Notifies)
		{
			json Node;
			Node["Name"] = Notify.Name;
			Node["Time"] = Notify.Time;
			if (Notify.Kind == EAnimNotifyKind::State)
			{
				Node["Duration"] = Notify.Duration;
			}
			Notifies.push_back(Node);
		}
		ClipArray.push_back({ { "Clip", Entry.Clip }, { "Notifies", Notifies } });
	}
	Document["Clips"] = ClipArray;
	json SocketArray  = json::array();
	for (const FModelSocket& Socket : Sockets)
	{
		SocketArray.push_back({ { "Name", Socket.Name },
		                        { "Bone", Socket.Bone },
		                        { "Position", ToJson(Socket.Position) },
		                        { "Rotation", ToJson(Socket.Rotation) },
		                        { "Scale", ToJson(Socket.Scale) } });
	}
	Document["Sockets"] = SocketArray;
	return Document.dump(2);
}

bool FModelMetadata::FromJsonString(const std::string& Json)
{
	const json Document = json::parse(Json, nullptr, false, true);
	if (Document.is_discarded() || !Document.is_object())
	{
		E_LOG(LogScene, Error, "모델 메타데이터(.emeta) JSON 파싱 실패");
		return false;
	}
	*this = FModelMetadata{};
	if (const auto It = Document.find("Clips"); It != Document.end() && It->is_array())
	{
		for (const json& ClipNode : *It)
		{
			FClipNotifies Entry;
			Entry.Clip = ClipNode.value("Clip", std::string());
			if (const auto Notifies = ClipNode.find("Notifies"); Notifies != ClipNode.end() && Notifies->is_array())
			{
				for (const json& Node : *Notifies)
				{
					FAnimNotify Notify;
					Notify.Name     = AnimNotifyMath::MakeValidName(Node.value("Name", std::string("Notify")));
					Notify.Time     = std::max(Node.value("Time", 0.0f), 0.0f);
					Notify.Duration = std::max(Node.value("Duration", 0.0f), 0.0f);
					Notify.Kind     = Node.contains("Duration") && Notify.Duration > 0.0f ? EAnimNotifyKind::State : EAnimNotifyKind::Notify;
					Entry.Notifies.push_back(std::move(Notify));
				}
			}
			std::sort(Entry.Notifies.begin(), Entry.Notifies.end(), [](const FAnimNotify& L, const FAnimNotify& R) { return L.Time < R.Time; });
			Clips.push_back(std::move(Entry));
		}
	}
	if (const auto It = Document.find("Sockets"); It != Document.end() && It->is_array())
	{
		for (const json& Node : *It)
		{
			FModelSocket Socket;
			Socket.Name     = Node.value("Name", std::string("Socket"));
			Socket.Bone     = Node.value("Bone", std::string());
			Socket.Position = ReadVector3(Node.value("Position", json()), FVector3::ZeroVector);
			Socket.Rotation = ReadQuat(Node.value("Rotation", json()));
			Socket.Scale    = ReadVector3(Node.value("Scale", json()), FVector3::OneVector);
			Sockets.push_back(std::move(Socket));
		}
	}
	return true;
}

std::filesystem::path FModelMetadata::GetSidecarPath(const std::filesystem::path& SourcePath)
{
	std::filesystem::path Path = SourcePath;
	Path += Extension;
	return Path;
}

FModelMetadata FModelMetadata::LoadForSource(const std::filesystem::path& SourcePath)
{
	FModelMetadata Metadata;
	std::ifstream  File(GetSidecarPath(SourcePath), std::ios::binary);
	if (File)
	{
		std::stringstream Buffer;
		Buffer << File.rdbuf();
		if (!Metadata.FromJsonString(Buffer.str()))
		{
			E_LOG(LogScene, Warning, "모델 메타데이터를 읽지 못해 비워 둡니다: {}", FStringConv::ToUtf8(GetSidecarPath(SourcePath).wstring()));
			Metadata = FModelMetadata{};
		}
	}
	return Metadata;
}

bool FModelMetadata::SaveForSource(const std::filesystem::path& SourcePath) const
{
	const std::filesystem::path Path = GetSidecarPath(SourcePath);
	if (IsEmpty())
	{
		std::error_code ErrorCode;
		std::filesystem::remove(Path, ErrorCode);
		return !ErrorCode;
	}
	std::ofstream File(Path, std::ios::binary | std::ios::trunc);
	if (!File)
	{
		E_LOG(LogScene, Error, "모델 메타데이터를 쓸 수 없습니다: {}", FStringConv::ToUtf8(Path.wstring()));
		return false;
	}
	File << ToJsonString();
	return true;
}
