using UnrealBuildTool;
using System.IO;

public class UnrealGPTEditorTests : ModuleRules
{
	public UnrealGPTEditorTests(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"UnrealGPT",
				"UnrealGPTEditor"
			}
		);
			
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"AutomationController",
				"EditorStyle",
				"Slate",
				"SlateCore",
				"UnrealEd",
				"UnrealGPTEditor"
			}
		);

		PublicIncludePaths.AddRange(
			new string[]
			{
				Path.Combine(ModuleDirectory, "../UnrealGPTEditor")
			}
		);
	}
}

