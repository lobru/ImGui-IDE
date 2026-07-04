// ImGui-IDE source code accessor for Unreal Engine 5.

using UnrealBuildTool;

public class ImGuiIDESourceCodeAccess : ModuleRules
{
	public ImGuiIDESourceCodeAccess(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"SourceCodeAccess",
				"DesktopPlatform",
			}
		);

		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.Add("HotReload");
		}
	}
}
