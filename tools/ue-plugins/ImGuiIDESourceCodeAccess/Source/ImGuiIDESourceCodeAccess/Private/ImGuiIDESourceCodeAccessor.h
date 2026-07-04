// ImGui-IDE source code accessor: implements UE's ISourceCodeAccessor so the
// engine's "Open Source Code" / class double-click routes to ImGui-IDE.exe.

#pragma once

#include "CoreMinimal.h"
#include "ISourceCodeAccessor.h"

class FImGuiIDESourceCodeAccessor : public ISourceCodeAccessor
{
public:
	/** ISourceCodeAccessor */
	virtual void RefreshAvailability() override;
	virtual bool CanAccessSourceCode() const override;
	virtual FName GetFName() const override;
	virtual FText GetNameText() const override;
	virtual FText GetDescriptionText() const override;
	virtual bool OpenSolution() override;
	virtual bool OpenSolutionAtPath(const FString& InSolutionPath) override;
	virtual bool DoesSolutionExist() const override;
	virtual bool OpenFileAtLine(const FString& FullPath, int32 LineNumber, int32 ColumnNumber = 0) override;
	virtual bool OpenSourceFiles(const TArray<FString>& AbsoluteSourcePaths) override;
	virtual bool AddSourceFiles(const TArray<FString>& AbsoluteSourcePaths, const TArray<FString>& AvailableModules) override;
	virtual bool SaveAllOpenDocuments() const override;
	virtual void Tick(const float DeltaTime) override;

private:
	/** Locate ImGui-IDE.exe (installed location, then PATH). Cached by RefreshAvailability. */
	FString IDEPath;

	bool Launch(const TArray<FString>& Args) const;
};
