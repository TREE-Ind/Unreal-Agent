using UnrealBuildTool;

public class UnrealGPTEditor : ModuleRules
{
	public UnrealGPTEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		
		PublicIncludePaths.AddRange(
			new string[] {
			}
		);
				
		PrivateIncludePaths.AddRange(
			new string[] {
			}
		);
			
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"UnrealGPT",
				"DeveloperSettings",
				"Projects" // For IPluginManager
			}
		);
			
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Slate",
				"SlateCore",
				"EditorStyle",
				"EditorWidgets",
				"ToolMenus",
				"UnrealEd",
				"LevelEditor",
				"WorkspaceMenuStructure",
				"PropertyEditor",
				"Settings",
				"HTTP",
				"Json",
				"JsonUtilities",
				"PythonScriptPlugin",
				"EditorSubsystem",
				"ToolWidgets",
				"ApplicationCore",
				"InputCore",
				"AudioCapture",
				"AudioCaptureCore",
				"AudioMixer",
				"RHI",
				"RenderCore"
			}
		);
	}
}

