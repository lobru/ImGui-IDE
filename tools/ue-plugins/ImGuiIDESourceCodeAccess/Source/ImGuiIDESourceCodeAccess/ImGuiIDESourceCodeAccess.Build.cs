// ImGui-IDE source code accessor for Unreal Engine 5.

using UnrealBuildTool;

public class ImGuiIDESourceCodeAccess : ModuleRules
{
	public ImGuiIDESourceCodeAccess(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Deliberately minimal: the accessor only spawns a process and registers a
		// modular feature. Extra deps are extra ways for the plugin build to break.
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"SourceCodeAccess",
			}
		);
	}
}
