#include "Core/FileWatcher.h"
#include "Core/Testing/TestFramework.h"

#include <algorithm>
#include <fstream>
#include <thread>

namespace
{
	using namespace std::chrono_literals;

	std::filesystem::path MakeTempDirectory(const wchar_t* Name)
	{
		const std::filesystem::path Dir = FTestRegistry::GetTempDirectory() / L"ProjectE_CoreTests" / Name;
		std::error_code             ErrorCode;
		std::filesystem::remove_all(Dir, ErrorCode);
		std::filesystem::create_directories(Dir, ErrorCode);
		return std::filesystem::weakly_canonical(Dir, ErrorCode);
	}

	void WriteTextFile(const std::filesystem::path& Path, const std::string& Text)
	{
		std::ofstream File(Path, std::ios::binary | std::ios::trunc);
		File << Text;
	}

	bool ContainsFile(const std::vector<std::filesystem::path>& Paths, const std::wstring& FileName)
	{
		return std::any_of(Paths.begin(), Paths.end(), [&](const std::filesystem::path& Path) { return Path.filename() == FileName; });
	}

	// 최대 Timeout 동안 폴링하며 누적 결과 반환 (Stop 조건: Expected 파일이 나타남)
	std::vector<std::filesystem::path> PollUntil(FFileWatcher& Watcher, const std::wstring& Expected, std::chrono::milliseconds Timeout)
	{
		std::vector<std::filesystem::path> All;
		const auto                         Deadline = std::chrono::steady_clock::now() + Timeout;
		while (std::chrono::steady_clock::now() < Deadline)
		{
			for (std::filesystem::path& Path : Watcher.Poll())
			{
				All.push_back(std::move(Path));
			}
			if (ContainsFile(All, Expected))
			{
				break;
			}
			std::this_thread::sleep_for(10ms);
		}
		return All;
	}
} // namespace

E_TEST(ChangeDebouncer_CoalescesAndDelays)
{
	FChangeDebouncer Debouncer(100ms);
	const auto       T0 = FChangeDebouncer::FClock::now();

	Debouncer.AddEvent(L"C:/Shaders/Mesh.hlsl", T0);
	Debouncer.AddEvent(L"C:/Shaders/Mesh.hlsl", T0 + 50ms);  // 같은 파일: 합쳐지고 시각 갱신
	Debouncer.AddEvent(L"C:/Shaders/MESH.HLSL", T0 + 60ms);  // 대소문자만 다른 경로도 같은 파일
	Debouncer.AddEvent(L"C:/Shaders/Common.hlsli", T0 + 10ms);
	E_EXPECT_EQ(Debouncer.GetPendingCount(), static_cast<size_t>(2));

	// 아직 지연 전
	E_EXPECT_TRUE(Debouncer.CollectReady(T0 + 90ms).empty());

	// Common만 준비됨 (마지막 10ms + 100ms)
	const std::vector<std::filesystem::path> First = Debouncer.CollectReady(T0 + 115ms);
	E_EXPECT_EQ(First.size(), static_cast<size_t>(1));
	E_EXPECT_TRUE(ContainsFile(First, L"Common.hlsli"));

	// Mesh는 마지막 이벤트(60ms) + 100ms 이후
	E_EXPECT_TRUE(Debouncer.CollectReady(T0 + 150ms).empty());
	const std::vector<std::filesystem::path> Second = Debouncer.CollectReady(T0 + 160ms);
	E_EXPECT_EQ(Second.size(), static_cast<size_t>(1));
	E_EXPECT_EQ(Debouncer.GetPendingCount(), static_cast<size_t>(0));

	// 보고 순서는 최초 추가 순서
	Debouncer.AddEvent(L"C:/B.txt", T0);
	Debouncer.AddEvent(L"C:/A.txt", T0);
	const std::vector<std::filesystem::path> Ordered = Debouncer.CollectReady(T0 + 1s);
	E_EXPECT_EQ(Ordered.size(), static_cast<size_t>(2));
	if (Ordered.size() == 2)
	{
		E_EXPECT_TRUE(Ordered[0].filename() == L"B.txt" && Ordered[1].filename() == L"A.txt");
	}
}

E_TEST(FileWatcher_DetectsWriteAndRename)
{
	const std::filesystem::path Dir = MakeTempDirectory(L"Watch");
	std::filesystem::create_directories(Dir / L"Sub");

	FFileWatcher Watcher;
	E_EXPECT_TRUE(Watcher.Start(Dir, true, 50ms));
	E_EXPECT_TRUE(Watcher.IsWatching());
	E_EXPECT_TRUE(Watcher.Poll().empty());

	// 1) 일반 쓰기 (여러 번 써도 한 번 보고)
	WriteTextFile(Dir / L"Test.hlsl", "float4 A;");
	WriteTextFile(Dir / L"Test.hlsl", "float4 B;");
	std::vector<std::filesystem::path> Changes = PollUntil(Watcher, L"Test.hlsl", 2000ms);
	E_EXPECT_TRUE(ContainsFile(Changes, L"Test.hlsl"));
	E_EXPECT_EQ(std::count_if(Changes.begin(), Changes.end(), [](const std::filesystem::path& P) { return P.filename() == L"Test.hlsl"; }),
	            static_cast<std::ptrdiff_t>(1));
	for (const std::filesystem::path& Path : Changes)
	{
		E_EXPECT_TRUE(Path.is_absolute());
	}

	// 2) 하위 폴더
	WriteTextFile(Dir / L"Sub" / L"Nested.hlsli", "// nested");
	Changes = PollUntil(Watcher, L"Nested.hlsli", 2000ms);
	E_EXPECT_TRUE(ContainsFile(Changes, L"Nested.hlsli"));

	// 3) 임시 파일에 쓰고 이름 바꾸기 → 최종 파일명만 보고 (임시 파일은 더 이상 존재하지 않음)
	WriteTextFile(Dir / L"Final.hlsl.tmp", "float4 C;");
	std::error_code ErrorCode;
	std::filesystem::rename(Dir / L"Final.hlsl.tmp", Dir / L"Final.hlsl", ErrorCode);
	E_EXPECT_FALSE(static_cast<bool>(ErrorCode));
	Changes = PollUntil(Watcher, L"Final.hlsl", 2000ms);
	E_EXPECT_TRUE(ContainsFile(Changes, L"Final.hlsl"));
	E_EXPECT_FALSE(ContainsFile(Changes, L"Final.hlsl.tmp"));

	Watcher.Stop();
	E_EXPECT_FALSE(Watcher.IsWatching());
	E_EXPECT_TRUE(Watcher.Poll().empty());

	// 없는 디렉터리는 시작 실패
	FFileWatcher Missing;
	E_EXPECT_FALSE(Missing.Start(Dir / L"DoesNotExist", true));

	std::filesystem::remove_all(Dir, ErrorCode);
}
