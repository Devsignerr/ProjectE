#pragma once

#include "Core/CoreTypes.h"

#include <cstring>
#include <filesystem>
#include <string>
#include <type_traits>
#include <vector>

// 리틀 엔디언 바이너리 쓰기 (쿠킹 에셋용). 메모리 버퍼에 누적 후 파일로 저장한다.
class FBinaryWriter
{
public:
	template <typename T>
	void Write(const T& Value)
	{
		static_assert(std::is_trivially_copyable_v<T>, "Write<T>는 trivially copyable 타입만 지원합니다");
		WriteBytes(&Value, sizeof(T));
	}

	void WriteBytes(const void* Data, size_t Size)
	{
		const size_t Offset = Buffer.size();
		Buffer.resize(Offset + Size);
		if (Size > 0)
		{
			std::memcpy(Buffer.data() + Offset, Data, Size);
		}
	}

	void WriteString(const std::string& Value)
	{
		Write(static_cast<uint32>(Value.size()));
		WriteBytes(Value.data(), Value.size());
	}

	// 원소 수(uint32) + 원소 배열 (trivially copyable 원소)
	template <typename T>
	void WriteArray(const std::vector<T>& Values)
	{
		static_assert(std::is_trivially_copyable_v<T>, "WriteArray<T>는 trivially copyable 원소만 지원합니다");
		Write(static_cast<uint32>(Values.size()));
		WriteBytes(Values.data(), Values.size() * sizeof(T));
	}

	const std::vector<uint8>& GetBuffer() const { return Buffer; }

	// 상위 디렉터리 생성 후 원자적으로 저장 (임시 파일 → 이름 변경). 실패 시 false
	bool SaveToFile(const std::filesystem::path& Path) const;

private:
	std::vector<uint8> Buffer;
};

// 경계 검사를 하는 바이너리 읽기. 범위를 벗어나면 실패 상태가 되고 이후 읽기는 기본값을 반환한다.
class FBinaryReader
{
public:
	FBinaryReader() = default;
	FBinaryReader(const uint8* InData, size_t InSize) : Data(InData), Size(InSize) {}

	template <typename T>
	T Read()
	{
		static_assert(std::is_trivially_copyable_v<T>, "Read<T>는 trivially copyable 타입만 지원합니다");
		T Value{};
		ReadBytes(&Value, sizeof(T));
		return Value;
	}

	bool ReadBytes(void* Out, size_t Count)
	{
		if (bFailed || Count > Size - Offset)
		{
			bFailed = true;
			return false;
		}
		if (Count > 0)
		{
			std::memcpy(Out, Data + Offset, Count);
		}
		Offset += Count;
		return true;
	}

	std::string ReadString()
	{
		const uint32 Length = Read<uint32>();
		if (bFailed || Length > Size - Offset)
		{
			bFailed = true;
			return {};
		}
		std::string Value(reinterpret_cast<const char*>(Data + Offset), Length);
		Offset += Length;
		return Value;
	}

	// MaxCount: 손상된 파일로 인한 과대 할당 방지용 상한
	template <typename T>
	std::vector<T> ReadArray(uint32 MaxCount = 0x10000000u)
	{
		static_assert(std::is_trivially_copyable_v<T>, "ReadArray<T>는 trivially copyable 원소만 지원합니다");
		const uint32 Count = Read<uint32>();
		if (bFailed || Count > MaxCount || static_cast<uint64>(Count) * sizeof(T) > Size - Offset)
		{
			bFailed = true;
			return {};
		}
		std::vector<T> Values(Count);
		ReadBytes(Values.data(), Values.size() * sizeof(T));
		return Values;
	}

	bool   IsOk() const { return !bFailed; }
	bool   IsAtEnd() const { return Offset == Size; }
	size_t GetOffset() const { return Offset; }

private:
	const uint8* Data   = nullptr;
	size_t       Size   = 0;
	size_t       Offset = 0;
	bool         bFailed = false;
};

// 파일 전체를 읽는다. 실패 시 false
bool ReadFileBytes(const std::filesystem::path& Path, std::vector<uint8>& OutBytes);
