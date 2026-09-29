#include "Core/Paths.h"
#include "Core/Testing/TestFramework.h"
#include "RHI/D3D12/D3D12ShaderCompiler.h"
#include "RHI/ShaderLibrary.h"
#include "RHI/ShaderManifest.h"

#include <chrono>
#include <fstream>

namespace
{
	std::filesystem::path MakeTempDirectory(const wchar_t* Name)
	{
		const std::filesystem::path Dir = std::filesystem::temp_directory_path() / L"ProjectE_RhiTests" / Name;
		std::error_code             ErrorCode;
		std::filesystem::remove_all(Dir, ErrorCode);
		std::filesystem::create_directories(Dir, ErrorCode);
		return Dir;
	}

	void WriteTextFile(const std::filesystem::path& Path, const std::string& Text)
	{
		std::ofstream File(Path, std::ios::binary | std::ios::trunc);
		File << Text;
	}
} // namespace

E_TEST(ShaderManifest_ParseJson)
{
	FShaderManifest Manifest;
	std::string     Error;

	const std::string Json = R"({
		// 주석 허용
		"Shaders": [
			{ "File": "Mesh.hlsl", "Entry": "VSMain", "Stage": "Vertex" },
			{ "File": "Mesh.hlsl", "Entry": "PSMain", "Stage": "pixel", "Defines": ["USE_FOG", "QUALITY=2"] },
			{ "File": "GenerateMips.hlsl", "Entry": "CSMain", "Stage": "Compute" }
		]
	})";
	E_EXPECT_TRUE(Manifest.ParseJson(Json, Error));
	E_EXPECT_EQ(Manifest.Entries.size(), static_cast<size_t>(3));
	if (Manifest.Entries.size() == 3)
	{
		E_EXPECT_TRUE(Manifest.Entries[0].File == L"Mesh.hlsl" && Manifest.Entries[0].EntryPoint == L"VSMain");
		E_EXPECT_TRUE(Manifest.Entries[0].Stage == EShaderStage::Vertex);
		E_EXPECT_TRUE(Manifest.Entries[1].Stage == EShaderStage::Pixel); // 대소문자 무시
		E_EXPECT_EQ(Manifest.Entries[1].Defines.size(), static_cast<size_t>(2));
		E_EXPECT_TRUE(Manifest.Entries[1].Defines[1] == L"QUALITY=2");
		E_EXPECT_TRUE(Manifest.Entries[2].Stage == EShaderStage::Compute);

		const FShaderCompileDesc Desc = Manifest.Entries[1].ToCompileDesc();
		E_EXPECT_TRUE(Desc.FileName == L"Mesh.hlsl" && Desc.EntryPoint == L"PSMain" && Desc.Defines.size() == 2);
	}

	// BOM 허용
	E_EXPECT_TRUE(Manifest.ParseJson("\xEF\xBB\xBF{\"Shaders\":[]}", Error));
	E_EXPECT_EQ(Manifest.Entries.size(), static_cast<size_t>(0));

	// 오류: 구문, 배열 없음, 필드 누락, 알 수 없는 스테이지
	E_EXPECT_FALSE(Manifest.ParseJson("{ not json", Error));
	E_EXPECT_FALSE(Manifest.ParseJson(R"({"Foo":1})", Error));
	E_EXPECT_FALSE(Manifest.ParseJson(R"({"Shaders":[{"File":"A.hlsl","Entry":"Main"}]})", Error));
	E_EXPECT_FALSE(Manifest.ParseJson(R"({"Shaders":[{"File":"A.hlsl","Entry":"Main","Stage":"Geometry"}]})", Error));
	E_EXPECT_TRUE(Error.find("Geometry") != std::string::npos);
}

E_TEST(ShaderManifest_EngineFileLoads)
{
	// 실제 Engine/Shaders/Shaders.json은 존재하는 셰이더/엔트리만 가리켜야 한다
	FShaderManifest Manifest;
	E_EXPECT_TRUE(Manifest.LoadFromFile(FPaths::GetEngineShaderDirectory() / FShaderManifest::DefaultFileName));
	E_EXPECT_TRUE(Manifest.Entries.size() >= 5);
	for (const FShaderManifestEntry& Entry : Manifest.Entries)
	{
		E_EXPECT_TRUE(std::filesystem::exists(FPaths::GetEngineShaderDirectory() / Entry.File));
	}
}

E_TEST(ShaderCooking_FileNameRule)
{
	FShaderCompileDesc Desc;
	Desc.FileName   = L"Mesh.hlsl";
	Desc.EntryPoint = L"VSMain";
	Desc.Stage      = EShaderStage::Vertex;

	E_EXPECT_TRUE(GetCookedShaderFileName(Desc, false) == L"Mesh_VSMain_Vertex.dxil");
	E_EXPECT_TRUE(GetCookedShaderFileName(Desc, true) == L"Mesh_VSMain_Vertex.debug.dxil");

	Desc.Stage = EShaderStage::Pixel;
	E_EXPECT_TRUE(GetCookedShaderFileName(Desc, false) == L"Mesh_VSMain_Pixel.dxil");

	// 디파인이 있으면 해시 접미사, 순서가 다르면 다른 해시
	Desc.Defines = { L"A", L"B=1" };
	const std::wstring WithDefines = GetCookedShaderFileName(Desc, false);
	E_EXPECT_TRUE(WithDefines.rfind(L"Mesh_VSMain_Pixel_", 0) == 0);
	E_EXPECT_TRUE(WithDefines.size() == std::wstring(L"Mesh_VSMain_Pixel_").size() + 16 + 5);
	FShaderCompileDesc Reordered = Desc;
	Reordered.Defines            = { L"B=1", L"A" };
	E_EXPECT_TRUE(GetCookedShaderFileName(Reordered, false) != WithDefines);
	E_EXPECT_EQ(HashShaderDefines({}), static_cast<uint64>(0));
	E_EXPECT_TRUE(HashShaderDefines(Desc.Defines) != 0);

	// 하위 폴더 파일은 스템만 사용
	Desc.FileName = L"PostProcess/Tonemap.hlsl";
	Desc.Defines.clear();
	E_EXPECT_TRUE(GetCookedShaderFileName(Desc, false) == L"Tonemap_VSMain_Pixel.dxil");

	// 캐시 키는 모든 요소 포함
	E_EXPECT_TRUE(FShaderLibrary::MakeCacheKey(Desc) != FShaderLibrary::MakeCacheKey(Reordered));
}

E_TEST(ShaderCooking_UpToDateCheck)
{
	using namespace std::chrono_literals;
	const std::filesystem::file_time_type Base = std::filesystem::file_time_type::clock::now();

	const std::filesystem::file_time_type Sources[] = { Base - 10s, Base - 5s };
	E_EXPECT_TRUE(IsCookedShaderUpToDate(Base, Sources));
	E_EXPECT_TRUE(IsCookedShaderUpToDate(Base - 5s, Sources)); // 같은 시각은 최신으로 간주
	E_EXPECT_FALSE(IsCookedShaderUpToDate(Base - 7s, Sources));
	E_EXPECT_FALSE(IsCookedShaderUpToDate(Base, {}));
}

E_TEST(ShaderCooking_DependencyScan)
{
	const std::filesystem::path Dir = MakeTempDirectory(L"Deps");
	WriteTextFile(Dir / L"Common.hlsli", "// common\n");
	WriteTextFile(Dir / L"Lighting.hlsli", "#include \"Common.hlsli\"\n");
	WriteTextFile(Dir / L"Main.hlsl", "  # include \"Lighting.hlsli\"\n#include \"Common.hlsli\"\n#include \"Missing.hlsli\"\nvoid Main() {}\n");

	const std::vector<std::filesystem::path> Deps = CollectShaderDependencies(Dir / L"Main.hlsl", Dir);
	E_EXPECT_EQ(Deps.size(), static_cast<size_t>(3)); // Main, Lighting, Common (중복/누락 제외)
	if (!Deps.empty())
	{
		E_EXPECT_TRUE(Deps[0].filename() == L"Main.hlsl");
	}

	// 존재하지 않는 소스는 빈 목록
	E_EXPECT_TRUE(CollectShaderDependencies(Dir / L"Nope.hlsl", Dir).empty());

	std::error_code ErrorCode;
	std::filesystem::remove_all(Dir, ErrorCode);
}

E_TEST(ShaderLibrary_CompileCookReloadRoundtrip)
{
	using namespace std::chrono_literals;

	FD3D12ShaderCompiler Compiler;
	E_EXPECT_TRUE(Compiler.Init());

	const std::filesystem::path CookedDir = MakeTempDirectory(L"Cooked");

	FShaderCompileDesc Desc;
	Desc.FileName   = L"Triangle.hlsl";
	Desc.EntryPoint = L"VSMain";
	Desc.Stage      = EShaderStage::Vertex;

	size_t CompiledSize = 0;
	{
		// 1) 캐시 없음 → 컴파일 + 쿠킹 기록
		FShaderLibrary Library;
		E_EXPECT_TRUE(Library.Init(Compiler, CookedDir, true));
		const ComPtr<IDxcBlob> Blob = Library.GetShader(Desc);
		E_EXPECT_TRUE(Blob != nullptr);
		if (Blob)
		{
			CompiledSize = Blob->GetBufferSize();
		}
		E_EXPECT_EQ(Library.GetStats().Compiles, 1u);
		E_EXPECT_EQ(Library.GetStats().CookedLoads, 0u);
		E_EXPECT_TRUE(std::filesystem::exists(Library.GetCookedPath(Desc)));

		// 2) 같은 인스턴스 재요청 → 메모리 캐시
		E_EXPECT_TRUE(Library.GetShader(Desc) == Blob);
		E_EXPECT_EQ(Library.GetStats().MemoryHits, 1u);
	}
	{
		// 3) 새 인스턴스 → 쿠킹 파일 로드, 내용 동일
		FShaderLibrary Library;
		Library.Init(Compiler, CookedDir, true);
		const ComPtr<IDxcBlob> Blob = Library.GetShader(Desc);
		E_EXPECT_TRUE(Blob != nullptr);
		E_EXPECT_EQ(Library.GetStats().CookedLoads, 1u);
		E_EXPECT_EQ(Library.GetStats().Compiles, 0u);
		if (Blob)
		{
			E_EXPECT_EQ(Blob->GetBufferSize(), CompiledSize);
		}
	}
	{
		// 4) 쿠킹 파일을 소스보다 오래되게 만들면 재컴파일
		FShaderLibrary Library;
		Library.Init(Compiler, CookedDir, true);
		std::error_code ErrorCode;
		std::filesystem::last_write_time(Library.GetCookedPath(Desc), std::filesystem::file_time_type::clock::now() - 24h * 3650, ErrorCode);
		const ComPtr<IDxcBlob> Blob = Library.GetShader(Desc);
		E_EXPECT_TRUE(Blob != nullptr);
		E_EXPECT_EQ(Library.GetStats().Compiles, 1u);
		E_EXPECT_EQ(Library.GetStats().CookedLoads, 0u);
	}
	{
		// 5) CookShader는 항상 컴파일 + 기록, 실패하는 엔트리는 false
		FShaderLibrary Library;
		Library.Init(Compiler, CookedDir, true);
		E_EXPECT_TRUE(Library.CookShader(Desc));
		FShaderCompileDesc Bad = Desc;
		Bad.EntryPoint         = L"NoSuchEntry";
		E_EXPECT_FALSE(Library.CookShader(Bad));
		E_EXPECT_EQ(Library.GetStats().Failures, 1u);
	}

	Compiler.Shutdown();
	std::error_code ErrorCode;
	std::filesystem::remove_all(CookedDir, ErrorCode);
}
