#include "ImGuiIDESourceCodeAccessor.h"

#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "Misc/App.h"
#include "HAL/FileManager.h"
#include "Internationalization/Internationalization.h"

#define LOCTEXT_NAMESPACE "ImGuiIDESourceCodeAccessor"

void FImGuiIDESourceCodeAccessor::RefreshAvailability()
{
	IDEPath.Empty();

	// 1) Installed location (Inno Setup default).
	const TCHAR* Candidates[] = {
		TEXT("C:/Program Files/ImGui-IDE/ImGui-IDE.exe"),
		TEXT("C:/Program Files (x86)/ImGui-IDE/ImGui-IDE.exe"),
	};
	for (const TCHAR* Candidate : Candidates)
	{
		if (IFileManager::Get().FileExists(Candidate))
		{
			IDEPath = Candidate;
			return;
		}
	}

	// 2) PATH lookup — resolved lazily at launch time; assume available so the
	//    accessor stays selectable on dev machines that run it from a build tree.
	IDEPath = TEXT("ImGui-IDE.exe");
}

bool FImGuiIDESourceCodeAccessor::CanAccessSourceCode() const
{
	return !IDEPath.IsEmpty();
}

FName FImGuiIDESourceCodeAccessor::GetFName() const
{
	return FName("ImGuiIDE");
}

FText FImGuiIDESourceCodeAccessor::GetNameText() const
{
	return LOCTEXT("ImGuiIDEDisplayName", "ImGui-IDE");
}

FText FImGuiIDESourceCodeAccessor::GetDescriptionText() const
{
	return LOCTEXT("ImGuiIDEDisplayDesc", "Open source files in ImGui-IDE");
}

bool FImGuiIDESourceCodeAccessor::Launch(const TArray<FString>& Args) const
{
	if (IDEPath.IsEmpty())
	{
		return false;
	}
	FString ArgLine;
	for (const FString& Arg : Args)
	{
		ArgLine += TEXT("\"") + Arg + TEXT("\" ");
	}
	FProcHandle Proc = FPlatformProcess::CreateProc(*IDEPath, *ArgLine,
		/*bLaunchDetached*/ true, /*bLaunchHidden*/ false, /*bLaunchReallyHidden*/ false,
		nullptr, 0, nullptr, nullptr);
	if (Proc.IsValid())
	{
		FPlatformProcess::CloseProc(Proc);
		return true;
	}
	return false;
}

bool FImGuiIDESourceCodeAccessor::OpenSolution()
{
	// "Solution" for us is the game project root (nav tree + index root there).
	return OpenSolutionAtPath(FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
}

bool FImGuiIDESourceCodeAccessor::OpenSolutionAtPath(const FString& InSolutionPath)
{
	return Launch({ TEXT("--project"), InSolutionPath });
}

bool FImGuiIDESourceCodeAccessor::DoesSolutionExist() const
{
	return IFileManager::Get().DirectoryExists(*FPaths::ProjectDir());
}

bool FImGuiIDESourceCodeAccessor::OpenFileAtLine(const FString& FullPath, int32 LineNumber, int32 ColumnNumber)
{
	// v1 opens the file (the app restores per-file cursor state itself); line/column
	// routing can ride a --goto flag once the app grows one.
	(void)LineNumber;
	(void)ColumnNumber;
	return Launch({ FullPath });
}

bool FImGuiIDESourceCodeAccessor::OpenSourceFiles(const TArray<FString>& AbsoluteSourcePaths)
{
	bool bOk = true;
	for (const FString& Path : AbsoluteSourcePaths)
	{
		bOk &= Launch({ Path });
	}
	return bOk;
}

bool FImGuiIDESourceCodeAccessor::AddSourceFiles(const TArray<FString>& AbsoluteSourcePaths, const TArray<FString>& AvailableModules)
{
	// Nothing to add to (no solution file to maintain) — files are picked up by
	// the IDE's project tree automatically.
	(void)AbsoluteSourcePaths;
	(void)AvailableModules;
	return true;
}

bool FImGuiIDESourceCodeAccessor::SaveAllOpenDocuments() const
{
	// No IPC channel for this yet (the IDE autosaves); report success so hot
	// reload flows don't abort.
	return true;
}

void FImGuiIDESourceCodeAccessor::Tick(const float DeltaTime)
{
	(void)DeltaTime;
}

#undef LOCTEXT_NAMESPACE
