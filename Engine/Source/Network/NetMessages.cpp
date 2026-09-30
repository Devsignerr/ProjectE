#include "Network/NetMessages.h"

#include "Core/Serialization/BinaryArchive.h"

namespace
{
	FBinaryWriter BeginMessage(ENetMessageType Type)
	{
		FBinaryWriter Writer;
		Writer.Write(static_cast<uint8>(Type));
		return Writer;
	}

	// 종류 바이트를 확인하고 본문 읽기 위치에 둔 리더
	std::optional<FBinaryReader> BeginRead(const std::vector<uint8>& Data, ENetMessageType Type)
	{
		if (Data.empty() || Data[0] != static_cast<uint8>(Type))
		{
			return std::nullopt;
		}
		FBinaryReader Reader(Data.data(), Data.size());
		Reader.Read<uint8>();
		return Reader;
	}

	template <typename T>
	std::optional<T> FinishRead(const FBinaryReader& Reader, T&& Message)
	{
		if (!Reader.IsOk() || !Reader.IsAtEnd())
		{
			return std::nullopt;
		}
		return std::forward<T>(Message);
	}
} // namespace

namespace NetMessages
{
	std::vector<uint8> Encode(const FNetHello& Message)
	{
		FBinaryWriter Writer = BeginMessage(ENetMessageType::Hello);
		Writer.Write(Message.ProtocolVersion);
		Writer.WriteString(Message.EngineVersion);
		Writer.WriteString(Message.ProjectName);
		Writer.WriteString(Message.SceneAsset);
		Writer.WriteString(Message.PlayerName);
		return Writer.GetBuffer();
	}

	std::vector<uint8> Encode(const FNetWelcome& Message)
	{
		FBinaryWriter Writer = BeginMessage(ENetMessageType::Welcome);
		Writer.Write(Message.PlayerId);
		return Writer.GetBuffer();
	}

	std::vector<uint8> Encode(const FNetReject& Message)
	{
		FBinaryWriter Writer = BeginMessage(ENetMessageType::Reject);
		Writer.WriteString(Message.Reason);
		return Writer.GetBuffer();
	}

	std::optional<ENetMessageType> PeekType(const std::vector<uint8>& Data)
	{
		if (Data.empty())
		{
			return std::nullopt;
		}
		return static_cast<ENetMessageType>(Data[0]);
	}

	std::optional<FNetHello> DecodeHello(const std::vector<uint8>& Data)
	{
		std::optional<FBinaryReader> Reader = BeginRead(Data, ENetMessageType::Hello);
		if (!Reader)
		{
			return std::nullopt;
		}
		FNetHello Message;
		Message.ProtocolVersion = Reader->Read<uint32>();
		Message.EngineVersion   = Reader->ReadString();
		Message.ProjectName     = Reader->ReadString();
		Message.SceneAsset      = Reader->ReadString();
		Message.PlayerName      = Reader->ReadString();
		return FinishRead(*Reader, std::move(Message));
	}

	std::optional<FNetWelcome> DecodeWelcome(const std::vector<uint8>& Data)
	{
		std::optional<FBinaryReader> Reader = BeginRead(Data, ENetMessageType::Welcome);
		if (!Reader)
		{
			return std::nullopt;
		}
		FNetWelcome Message;
		Message.PlayerId = Reader->Read<uint32>();
		return FinishRead(*Reader, std::move(Message));
	}

	std::optional<FNetReject> DecodeReject(const std::vector<uint8>& Data)
	{
		std::optional<FBinaryReader> Reader = BeginRead(Data, ENetMessageType::Reject);
		if (!Reader)
		{
			return std::nullopt;
		}
		FNetReject Message;
		Message.Reason = Reader->ReadString();
		return FinishRead(*Reader, std::move(Message));
	}
}
