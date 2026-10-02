#include "Core/Testing/TestFramework.h"
#include "Editor/GameModuleHotReload.h"
#include "Core/Platform/WindowsHeaders.h"

#include <fstream>
#include <iterator>

// ---------------------------------------------------------------- 그림자 복사본 이름

E_TEST(HotReload_NextShadowCopyNumber)
{
	using GameModuleHotReload::FindNextShadowCopyNumber;
	E_EXPECT_EQ(FindNextShadowCopyNumber({}, "SampleGame"), 1u);
	E_EXPECT_EQ(FindNextShadowCopyNumber({ L"SampleGame_1.dll", L"SampleGame_1.pdb", L"SampleGame_3.dll" }, "SampleGame"), 4u);
	// 대소문자 무시, 다른 모듈/형식/숫자 아닌 꼬리는 무시
	E_EXPECT_EQ(FindNextShadowCopyNumber({ L"samplegame_7.DLL", L"OtherGame_20.dll", L"SampleGame_x.dll", L"SampleGame_.dll", L"SampleGame_9.pdb",
	                                       L"SampleGame_12_old.dll", L"SampleGameX_30.dll" },
	                                     "SampleGame"),
	            8u);
}

E_TEST(HotReload_ShadowCopyPath)
{
	const std::filesystem::path Path = GameModuleHotReload::MakeShadowCopyPath(L"C:/Saved/HotReload", "SampleGame", 12);
	E_EXPECT_TRUE(Path.filename() == L"SampleGame_12.dll");
	E_EXPECT_TRUE(Path.parent_path() == std::filesystem::path(L"C:/Saved/HotReload"));
}

// ---------------------------------------------------------------- 변경 감지 안정화

E_TEST(HotReload_DebouncerWaitsUntilStable)
{
	using FStamp = FFileChangeDebouncer::FStamp;
	FFileChangeDebouncer Debouncer;
	Debouncer.StableSeconds = 1.0;
	Debouncer.SetBaseline(FStamp{ 100, 1000, true });

	E_EXPECT_FALSE(Debouncer.Update(0.0, FStamp{ 100, 1000, true })); // 그대로
	E_EXPECT_FALSE(Debouncer.IsPending());

	// 링커가 쓰는 중: 크기/시각이 계속 바뀌면 기다린다
	E_EXPECT_FALSE(Debouncer.Update(1.0, FStamp{ 200, 10, true }));
	E_EXPECT_TRUE(Debouncer.IsPending());
	E_EXPECT_FALSE(Debouncer.Update(1.5, FStamp{ 210, 5000, true }));
	E_EXPECT_FALSE(Debouncer.Update(2.4, FStamp{ 210, 5000, true })); // 0.9초 — 아직
	// 파일이 잠깐 없어져도 기다리기만
	E_EXPECT_FALSE(Debouncer.Update(2.45, FStamp{}));
	E_EXPECT_TRUE(Debouncer.IsPending());
	// 안정 시간이 지났어도 아직 쓰는 중이면 내보내지 않는다
	E_EXPECT_FALSE(Debouncer.Update(2.6, FStamp{ 210, 5000, true }, false));
	E_EXPECT_TRUE(Debouncer.Update(2.7, FStamp{ 210, 5000, true }));
	E_EXPECT_FALSE(Debouncer.IsPending());
	// 한 번만: 새 기준과 같으면 다시 내보내지 않는다
	E_EXPECT_FALSE(Debouncer.Update(5.0, FStamp{ 210, 5000, true }));

	// 바뀌었다가 기준으로 돌아오면 대기 취소
	E_EXPECT_FALSE(Debouncer.Update(6.0, FStamp{ 300, 1, true }));
	E_EXPECT_FALSE(Debouncer.Update(6.5, FStamp{ 210, 5000, true }));
	E_EXPECT_FALSE(Debouncer.IsPending());
}

// ---------------------------------------------------------------- PDB 경로 (CodeView RSDS)

E_TEST(HotReload_PatchPdbPathInRealImage)
{
	// 테스트 실행 파일 옆의 엔진 DLL (실제 링커 출력)
	wchar_t Buffer[MAX_PATH]{};
	GetModuleFileNameW(nullptr, Buffer, MAX_PATH);
	const std::filesystem::path Dll = std::filesystem::path(Buffer).parent_path() / L"ProjectEEngine.dll";
	std::ifstream               Stream(Dll, std::ios::binary);
	E_EXPECT_TRUE(static_cast<bool>(Stream));
	std::vector<uint8> Image{ std::istreambuf_iterator<char>(Stream), std::istreambuf_iterator<char>() };

	std::string Original;
	E_EXPECT_TRUE(GameModuleHotReload::ReadPdbPath(Image, Original));
	E_EXPECT_TRUE(Original.size() > 4 && Original.ends_with(".pdb"));

	const size_t SizeBefore = Image.size();
	E_EXPECT_TRUE(GameModuleHotReload::PatchPdbPath(Image, "ProjectEEngine_3.pdb"));
	std::string Patched;
	E_EXPECT_TRUE(GameModuleHotReload::ReadPdbPath(Image, Patched));
	E_EXPECT_TRUE(Patched == "ProjectEEngine_3.pdb");
	E_EXPECT_EQ(Image.size(), SizeBefore);

	// 원래 칸보다 긴 경로는 거부 (이미지 그대로)
	E_EXPECT_FALSE(GameModuleHotReload::PatchPdbPath(Image, std::string(Original.size() + 64, 'x')));
	E_EXPECT_TRUE(GameModuleHotReload::ReadPdbPath(Image, Patched) && Patched == "ProjectEEngine_3.pdb");
}

E_TEST(HotReload_PdbPathRejectsNonImage)
{
	std::vector<uint8> NotImage(512, 0x41);
	std::string        Path;
	E_EXPECT_FALSE(GameModuleHotReload::ReadPdbPath(NotImage, Path));
	E_EXPECT_FALSE(GameModuleHotReload::PatchPdbPath(NotImage, "a.pdb"));
	std::vector<uint8> Empty;
	E_EXPECT_FALSE(GameModuleHotReload::ReadPdbPath(Empty, Path));
}
