#include "Core/Paths.h"

#include "Core/Assert.h"
#include "Core/CommandLine.h"
#include "Core/Log.h"
#include "Core/Platform/WindowsHeaders.h"
#include "Core/StringConv.h"

namespace
{
	constexpr const wchar_t* GEngineMarker         = L"Engine/Shaders/Shaders.json";
	constexpr const wchar_t* GDefaultProjectFile   = L"Projects/Sample/Sample.eproject";
	constexpr const wchar_t* GProjectOption        = L"--project";

	struct FPathsState
	{
		bool                  bInitialized = false;
		std::filesystem::path ExecutableDirectory;
		std::filesystem::path EngineDirectory;
		std::filesystem::path ProjectFile;
		FProjectDescriptor    ProjectDescriptor;
		bool                  bHasProject = false;
	};

	FPathsState& GetState()
	{
		static FPathsState State;
		return State;
	}

	std::filesystem::path QueryExecutableDirectory()
	{
		wchar_t     Buffer[MAX_PATH * 4];
		const DWORD Length = GetModuleFileNameW(nullptr, Buffer, static_cast<DWORD>(std::size(Buffer)));
		if (Length == 0 || Length >= std::size(Buffer))
		{
			E_LOG(LogCore, Fatal, "실행 파일 경로를 얻을 수 없습니다 (오류 코드 {})", GetLastError());
		}
		return std::filesystem::path(Buffer, Buffer + Length).parent_path();
	}

	std::filesystem::path EnsureDirectory(const std::filesystem::path& Directory)
	{
		std::error_code ErrorCode;
		std::filesystem::create_directories(Directory, ErrorCode);
		return Directory;
	}

	std::string ToUtf8(const std::filesystem::path& Path)
	{
		return FStringConv::ToUtf8(Path.wstring());
	}
} // namespace

void FPaths::Initialize()
{
	Initialize(FCommandLine::FromProcess());
}

void FPaths::Initialize(const FCommandLine& CommandLine)
{
	FPathsState& State = GetState();
	if (State.bInitialized)
	{
		E_LOG(LogCore, Warning, "FPaths가 이미 초기화되어 있습니다");
		return;
	}

	State.ExecutableDirectory = QueryExecutableDirectory();
	State.EngineDirectory     = FindEngineDirectory(State.ExecutableDirectory);
	if (State.EngineDirectory.empty())
	{
		E_LOG(LogCore, Fatal, "엔진 디렉터리를 찾을 수 없습니다. 실행 파일({}) 상위에 '{}'가 있어야 합니다",
		      ToUtf8(State.ExecutableDirectory), FStringConv::ToUtf8(GEngineMarker));
	}
	State.bInitialized = true;

	E_LOG(LogCore, Display, "엔진 디렉터리: {}", ToUtf8(State.EngineDirectory));

	// 프로젝트: 명령줄 우선, 없으면 예제 프로젝트
	const std::wstring ProjectArgument = CommandLine.GetValue(GProjectOption);
	if (!ProjectArgument.empty())
	{
		if (!SetProject(ProjectArgument))
		{
			E_LOG(LogCore, Error, "--project로 지정한 프로젝트를 열 수 없습니다: {}", FStringConv::ToUtf8(ProjectArgument));
		}
	}
	else
	{
		const std::filesystem::path DefaultProject = State.EngineDirectory / GDefaultProjectFile;
		std::error_code             ErrorCode;
		if (std::filesystem::exists(DefaultProject, ErrorCode))
		{
			SetProject(DefaultProject);
		}
	}

	if (!State.bHasProject)
	{
		E_LOG(LogCore, Warning, "열린 프로젝트가 없습니다 (--project <경로>로 지정)");
	}
}

bool FPaths::IsInitialized()
{
	return GetState().bInitialized;
}

const std::filesystem::path& FPaths::GetExecutableDirectory()
{
	E_CHECKF(GetState().bInitialized, "FPaths::Initialize()가 먼저 호출되어야 합니다");
	return GetState().ExecutableDirectory;
}

const std::filesystem::path& FPaths::GetEngineDirectory()
{
	E_CHECKF(GetState().bInitialized, "FPaths::Initialize()가 먼저 호출되어야 합니다");
	return GetState().EngineDirectory;
}

std::filesystem::path FPaths::GetEngineShaderDirectory()
{
	return GetEngineDirectory() / L"Engine" / L"Shaders";
}

bool FPaths::SetProject(const std::filesystem::path& ProjectFileOrDirectory)
{
	const std::filesystem::path ProjectFile = ResolveProjectFile(ProjectFileOrDirectory);
	if (ProjectFile.empty())
	{
		E_LOG(LogCore, Error, "프로젝트 파일을 찾을 수 없습니다: {}", ToUtf8(ProjectFileOrDirectory));
		return false;
	}

	FProjectDescriptor Descriptor;
	if (!Descriptor.LoadFromFile(ProjectFile))
	{
		return false;
	}

	FPathsState& State = GetState();
	std::error_code ErrorCode;
	State.ProjectFile = std::filesystem::weakly_canonical(ProjectFile, ErrorCode);
	if (ErrorCode)
	{
		State.ProjectFile = std::filesystem::absolute(ProjectFile);
	}
	State.ProjectDescriptor = std::move(Descriptor);
	State.bHasProject       = true;

	E_LOG(LogCore, Display, "프로젝트 열림: {} ({})", State.ProjectDescriptor.Name, ToUtf8(State.ProjectFile));
	return true;
}

bool FPaths::HasProject()
{
	return GetState().bHasProject;
}

const std::string& FPaths::GetProjectName()
{
	return GetState().ProjectDescriptor.Name;
}

const FProjectDescriptor& FPaths::GetProjectDescriptor()
{
	return GetState().ProjectDescriptor;
}

const std::filesystem::path& FPaths::GetProjectFile()
{
	return GetState().ProjectFile;
}

std::filesystem::path FPaths::GetProjectDirectory()
{
	E_CHECKF(GetState().bHasProject, "열린 프로젝트가 없습니다");
	return GetState().ProjectFile.parent_path();
}

std::filesystem::path FPaths::GetProjectContentDirectory()
{
	return GetProjectDirectory() / L"Content";
}

std::filesystem::path FPaths::GetProjectConfigDirectory()
{
	return GetProjectDirectory() / L"Config";
}

std::filesystem::path FPaths::GetProjectSavedDirectory()
{
	return EnsureDirectory(GetProjectDirectory() / L"Saved");
}

std::filesystem::path FPaths::GetSavedDirectory()
{
	if (HasProject())
	{
		return GetProjectSavedDirectory();
	}
	return EnsureDirectory(GetEngineDirectory() / L"Saved");
}

std::filesystem::path FPaths::FindEngineDirectory(const std::filesystem::path& StartDirectory)
{
	std::error_code       ErrorCode;
	std::filesystem::path Current = std::filesystem::absolute(StartDirectory, ErrorCode);
	while (!Current.empty())
	{
		if (std::filesystem::exists(Current / GEngineMarker, ErrorCode))
		{
			return Current;
		}
		const std::filesystem::path Parent = Current.parent_path();
		if (Parent == Current)
		{
			break;
		}
		Current = Parent;
	}
	return {};
}

std::filesystem::path FPaths::ResolveProjectFile(const std::filesystem::path& FileOrDirectory)
{
	std::error_code ErrorCode;
	if (std::filesystem::is_regular_file(FileOrDirectory, ErrorCode))
	{
		return FileOrDirectory;
	}
	if (std::filesystem::is_directory(FileOrDirectory, ErrorCode))
	{
		for (const std::filesystem::directory_entry& Entry : std::filesystem::directory_iterator(FileOrDirectory, ErrorCode))
		{
			if (Entry.is_regular_file(ErrorCode) && Entry.path().extension() == FProjectDescriptor::Extension)
			{
				return Entry.path();
			}
		}
	}
	return {};
}
