// Copyright (c) 2025 TREE Industries.

#include "UnrealGPTToolSchemas.h"
#include "Serialization/JsonSerializer.h"

TSharedPtr<FJsonObject> FUnrealGPTToolSchemas::BuildToolObject(
	const FString& Name,
	const FString& Description,
	const TSharedPtr<FJsonObject>& Parameters,
	bool bUseResponsesApi)
{
	TSharedPtr<FJsonObject> Tool = MakeShareable(new FJsonObject);
	Tool->SetStringField(TEXT("type"), TEXT("function"));

	if (bUseResponsesApi)
	{
		Tool->SetStringField(TEXT("name"), Name);
		Tool->SetStringField(TEXT("description"), Description);
		Tool->SetObjectField(TEXT("parameters"), Parameters);
	}
	else
	{
		TSharedPtr<FJsonObject> FunctionObj = MakeShareable(new FJsonObject);
		FunctionObj->SetStringField(TEXT("name"), Name);
		FunctionObj->SetStringField(TEXT("description"), Description);
		FunctionObj->SetObjectField(TEXT("parameters"), Parameters);
		Tool->SetObjectField(TEXT("function"), FunctionObj);
	}

	return Tool;
}

TSharedPtr<FJsonObject> FUnrealGPTToolSchemas::BuildPythonExecuteTool(bool bUseResponsesApi)
{
	TSharedPtr<FJsonObject> PythonParams = MakeShareable(new FJsonObject);
	PythonParams->SetStringField(TEXT("type"), TEXT("object"));
	
	TSharedPtr<FJsonObject> CodeProperty = MakeShareable(new FJsonObject);
	CodeProperty->SetStringField(TEXT("type"), TEXT("string"));
	CodeProperty->SetStringField(TEXT("description"), TEXT("Python code to execute"));
	PythonParams->SetObjectField(TEXT("properties"), MakeShareable(new FJsonObject));
	PythonParams->GetObjectField(TEXT("properties"))->SetObjectField(TEXT("code"), CodeProperty);
	
	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShareable(new FJsonValueString(TEXT("code"))));
	PythonParams->SetArrayField(TEXT("required"), Required);
	
	return BuildToolObject(
		TEXT("python_execute"),
		TEXT("Execute Python code in Unreal Engine editor. Use this to manipulate actors, spawn objects, modify properties, automate Content Browser and asset/Blueprint operations, and perform other editor tasks not possible with other tools. ")
		TEXT("Code runs in the editor Python environment with access to the 'unreal' module and editor subsystems. ")
		TEXT("The execution is wrapped in an editor transaction for Undo support. ")
		TEXT("Returns a standard result envelope with status, message, details, logs, and transaction fields. ")
		TEXT("If you populate 'result[\"details\"][\"actor_label\"]' or 'result[\"details\"][\"actor_name\"]' with the name of a created or modified actor, the editor viewport will automatically focus on it."),
		PythonParams,
		bUseResponsesApi);
}

TSharedPtr<FJsonObject> FUnrealGPTToolSchemas::BuildViewportScreenshotTool(bool bUseResponsesApi)
{
	TSharedPtr<FJsonObject> ScreenshotParams = MakeShareable(new FJsonObject);
	ScreenshotParams->SetStringField(TEXT("type"), TEXT("object"));
	
	TSharedPtr<FJsonObject> Properties = MakeShareable(new FJsonObject);
	
	TSharedPtr<FJsonObject> FocusActorProp = MakeShareable(new FJsonObject);
	FocusActorProp->SetStringField(TEXT("type"), TEXT("string"));
	FocusActorProp->SetStringField(TEXT("description"), TEXT("Optional actor label to focus/frame in viewport before capturing. If provided, the viewport camera will auto-frame the specified actor."));
	Properties->SetObjectField(TEXT("focus_actor"), FocusActorProp);
	
	ScreenshotParams->SetObjectField(TEXT("properties"), Properties);

	return BuildToolObject(
		TEXT("viewport_screenshot"),
		TEXT("Capture a screenshot of the active viewport with metadata. ")
		TEXT("Returns JSON with base64-encoded PNG image data, camera transform, FOV, resolution, and list of selected actors. ")
		TEXT("Optionally specify focus_actor to auto-frame a specific actor before capture. ")
		TEXT("Use this to visually verify changes, show the user the current state of the scene, or before asking for visual feedback."),
		ScreenshotParams,
		bUseResponsesApi);
}

TSharedPtr<FJsonObject> FUnrealGPTToolSchemas::BuildSceneQueryTool(bool bUseResponsesApi)
{
	TSharedPtr<FJsonObject> SceneQueryParams = MakeShareable(new FJsonObject);
	SceneQueryParams->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShareable(new FJsonObject);

	TSharedPtr<FJsonObject> ClassContainsProp = MakeShareable(new FJsonObject);
	ClassContainsProp->SetStringField(TEXT("type"), TEXT("string"));
	ClassContainsProp->SetStringField(TEXT("description"), TEXT("Optional substring to match in actor class names, e.g., 'DirectionalLight', 'StaticMeshActor'."));
	Properties->SetObjectField(TEXT("class_contains"), ClassContainsProp);

	TSharedPtr<FJsonObject> LabelContainsProp = MakeShareable(new FJsonObject);
	LabelContainsProp->SetStringField(TEXT("type"), TEXT("string"));
	LabelContainsProp->SetStringField(TEXT("description"), TEXT("Optional substring to match in actor labels as shown in the Outliner."));
	Properties->SetObjectField(TEXT("label_contains"), LabelContainsProp);

	TSharedPtr<FJsonObject> NameContainsProp = MakeShareable(new FJsonObject);
	NameContainsProp->SetStringField(TEXT("type"), TEXT("string"));
	NameContainsProp->SetStringField(TEXT("description"), TEXT("Optional substring to match in actor object names."));
	Properties->SetObjectField(TEXT("name_contains"), NameContainsProp);

	TSharedPtr<FJsonObject> ComponentClassContainsProp = MakeShareable(new FJsonObject);
	ComponentClassContainsProp->SetStringField(TEXT("type"), TEXT("string"));
	ComponentClassContainsProp->SetStringField(TEXT("description"), TEXT("Optional substring to match in component class names, e.g., 'DirectionalLightComponent'."));
	Properties->SetObjectField(TEXT("component_class_contains"), ComponentClassContainsProp);

	TSharedPtr<FJsonObject> MaxResultsProp = MakeShareable(new FJsonObject);
	MaxResultsProp->SetStringField(TEXT("type"), TEXT("integer"));
	MaxResultsProp->SetStringField(TEXT("description"), TEXT("Maximum number of matching actors to return (default 20)."));
	MaxResultsProp->SetNumberField(TEXT("default"), 20);
	Properties->SetObjectField(TEXT("max_results"), MaxResultsProp);

	// New detail flags to reduce payload size
	TSharedPtr<FJsonObject> IncludeTransformProp = MakeShareable(new FJsonObject);
	IncludeTransformProp->SetStringField(TEXT("type"), TEXT("boolean"));
	IncludeTransformProp->SetStringField(TEXT("description"), TEXT("Include rotation and scale in results (default true). Location is always included."));
	Properties->SetObjectField(TEXT("include_transform"), IncludeTransformProp);

	TSharedPtr<FJsonObject> IncludeBoundsProp = MakeShareable(new FJsonObject);
	IncludeBoundsProp->SetStringField(TEXT("type"), TEXT("boolean"));
	IncludeBoundsProp->SetStringField(TEXT("description"), TEXT("Include bounds origin and extent in results (default false)."));
	Properties->SetObjectField(TEXT("include_bounds"), IncludeBoundsProp);

	TSharedPtr<FJsonObject> IncludeComponentsProp = MakeShareable(new FJsonObject);
	IncludeComponentsProp->SetStringField(TEXT("type"), TEXT("boolean"));
	IncludeComponentsProp->SetStringField(TEXT("description"), TEXT("Include root component, mobility, and static_mesh_path in results (default false)."));
	Properties->SetObjectField(TEXT("include_components"), IncludeComponentsProp);

	TSharedPtr<FJsonObject> IncludeMetadataProp = MakeShareable(new FJsonObject);
	IncludeMetadataProp->SetStringField(TEXT("type"), TEXT("boolean"));
	IncludeMetadataProp->SetStringField(TEXT("description"), TEXT("Include tags, folder_path, and parent_actor in results (default false)."));
	Properties->SetObjectField(TEXT("include_metadata"), IncludeMetadataProp);

	SceneQueryParams->SetObjectField(TEXT("properties"), Properties);

	return BuildToolObject(
		TEXT("scene_query"),
		TEXT("Search the current level for actors matching simple filters. ")
		TEXT("Returns a JSON array of matching actors with their locations (always), plus optional rotation/scale (include_transform), ")
		TEXT("bounds (include_bounds), component info (include_components), and metadata (include_metadata). ")
		TEXT("Use detail flags to control payload size. ")
		TEXT("You can filter by class_contains, label_contains, name_contains, component_class_contains, and control max_results."),
		SceneQueryParams,
		bUseResponsesApi);
}

TSharedPtr<FJsonObject> FUnrealGPTToolSchemas::BuildReflectionQueryTool(bool bUseResponsesApi)
{
	TSharedPtr<FJsonObject> ReflectionParams = MakeShareable(new FJsonObject);
	ReflectionParams->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShareable(new FJsonObject);

	TSharedPtr<FJsonObject> ClassNameProp = MakeShareable(new FJsonObject);
	ClassNameProp->SetStringField(TEXT("type"), TEXT("string"));
	ClassNameProp->SetStringField(
		TEXT("description"),
		TEXT("Name or path of the UClass to inspect. Short name, fully qualified path, or Blueprint generated class path."));
	Properties->SetObjectField(TEXT("class_name"), ClassNameProp);

	TSharedPtr<FJsonObject> MemberContainsProp = MakeShareable(new FJsonObject);
	MemberContainsProp->SetStringField(TEXT("type"), TEXT("string"));
	MemberContainsProp->SetStringField(TEXT("description"), TEXT("Optional substring filter on member names."));
	Properties->SetObjectField(TEXT("member_contains"), MemberContainsProp);

	TSharedPtr<FJsonObject> IncludeSuperProp = MakeShareable(new FJsonObject);
	IncludeSuperProp->SetStringField(TEXT("type"), TEXT("boolean"));
	IncludeSuperProp->SetStringField(TEXT("description"), TEXT("Include inherited members (default false)."));
	IncludeSuperProp->SetBoolField(TEXT("default"), false);
	Properties->SetObjectField(TEXT("include_super"), IncludeSuperProp);

	TSharedPtr<FJsonObject> ScriptableOnlyProp = MakeShareable(new FJsonObject);
	ScriptableOnlyProp->SetStringField(TEXT("type"), TEXT("boolean"));
	ScriptableOnlyProp->SetStringField(TEXT("description"), TEXT("Return only scriptable members (default true)."));
	ScriptableOnlyProp->SetBoolField(TEXT("default"), true);
	Properties->SetObjectField(TEXT("scriptable_only"), ScriptableOnlyProp);

	TSharedPtr<FJsonObject> MaxResultsProp = MakeShareable(new FJsonObject);
	MaxResultsProp->SetStringField(TEXT("type"), TEXT("integer"));
	MaxResultsProp->SetStringField(TEXT("description"), TEXT("Maximum members per category (default 40)."));
	MaxResultsProp->SetNumberField(TEXT("default"), 40);
	Properties->SetObjectField(TEXT("max_results"), MaxResultsProp);

	ReflectionParams->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShareable(new FJsonValueString(TEXT("class_name"))));
	ReflectionParams->SetArrayField(TEXT("required"), Required);

	return BuildToolObject(
		TEXT("reflection_query"),
		TEXT("Inspect an Unreal UClass via reflection. Returns a compact schema of scriptable properties and functions. ")
		TEXT("Use member_contains to target specific members; set include_super=true only when inherited members are needed."),
		ReflectionParams,
		bUseResponsesApi);
}

TSharedPtr<FJsonObject> FUnrealGPTToolSchemas::BuildReadLogTool(bool bUseResponsesApi)
{
	TSharedPtr<FJsonObject> ReadLogParams = MakeShareable(new FJsonObject);
	ReadLogParams->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShareable(new FJsonObject);

	auto AddIntProp = [&Properties](const TCHAR* Name, const TCHAR* Description, int32 DefaultValue)
	{
		TSharedPtr<FJsonObject> Prop = MakeShareable(new FJsonObject);
		Prop->SetStringField(TEXT("type"), TEXT("integer"));
		Prop->SetStringField(TEXT("description"), Description);
		Prop->SetNumberField(TEXT("default"), DefaultValue);
		Properties->SetObjectField(Name, Prop);
	};

	auto AddStringProp = [&Properties](const TCHAR* Name, const TCHAR* Description, const TCHAR* DefaultValue = nullptr)
	{
		TSharedPtr<FJsonObject> Prop = MakeShareable(new FJsonObject);
		Prop->SetStringField(TEXT("type"), TEXT("string"));
		Prop->SetStringField(TEXT("description"), Description);
		if (DefaultValue)
		{
			Prop->SetStringField(TEXT("default"), DefaultValue);
		}
		Properties->SetObjectField(Name, Prop);
	};

	AddIntProp(TEXT("max_lines"), TEXT("Maximum log lines to return (default 40, max 150)."), 40);
	AddIntProp(TEXT("max_chars"), TEXT("Character budget for the response (default 8000)."), 8000);
	AddStringProp(TEXT("min_verbosity"), TEXT("Minimum verbosity: error, warning, display, log, or all (default warning)."), TEXT("warning"));
	AddStringProp(TEXT("category"), TEXT("Optional case-insensitive category substring filter (e.g. LogPython)."));
	AddStringProp(TEXT("contains"), TEXT("Optional case-insensitive message substring filter (e.g. Traceback)."));
	AddStringProp(TEXT("mode"), TEXT("Read mode: tail, since_last_read, or file (default tail)."), TEXT("tail"));
	AddStringProp(TEXT("source"), TEXT("Log source: auto, memory, or file (default auto)."), TEXT("auto"));

	ReadLogParams->SetObjectField(TEXT("properties"), Properties);

	return BuildToolObject(
		TEXT("read_log"),
		TEXT("Read recent Unreal Editor log output in a token-efficient, filtered way. ")
		TEXT("Use after failed or ambiguous python_execute calls. Prefer category='LogPython' or contains='Traceback' for Python errors."),
		ReadLogParams,
		bUseResponsesApi);
}

TSharedPtr<FJsonObject> FUnrealGPTToolSchemas::BuildReplicateGenerateTool(bool bUseResponsesApi)
{
	TSharedPtr<FJsonObject> ReplicateParams = MakeShareable(new FJsonObject);
	ReplicateParams->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShareable(new FJsonObject);

	TSharedPtr<FJsonObject> PromptProp = MakeShareable(new FJsonObject);
	PromptProp->SetStringField(TEXT("type"), TEXT("string"));
	PromptProp->SetStringField(
		TEXT("description"),
		TEXT("Text prompt describing what to generate (image, video, audio, or 3D asset). For example: ")
		TEXT("'seamless square floral rock wall texture' or 'short ambient forest soundscape'."));
	Properties->SetObjectField(TEXT("prompt"), PromptProp);

	TSharedPtr<FJsonObject> VersionProp = MakeShareable(new FJsonObject);
	VersionProp->SetStringField(TEXT("type"), TEXT("string"));
	VersionProp->SetStringField(
		TEXT("description"),
		TEXT("Optional Replicate model identifier. You can pass either a full version id or an 'owner/model' slug for official models (for example 'black-forest-labs/flux-dev'). "));
	Properties->SetObjectField(TEXT("version"), VersionProp);

	TSharedPtr<FJsonObject> KindProp = MakeShareable(new FJsonObject);
	KindProp->SetStringField(TEXT("type"), TEXT("string"));
	KindProp->SetStringField(
		TEXT("description"),
		TEXT("Optional output kind hint: 'image', 'video', 'audio', or '3d'. Used to pick a staging folder, downstream Unreal import helper, ")
		TEXT("and default Replicate model from plugin settings when no explicit model version is provided."));
	Properties->SetObjectField(TEXT("output_kind"), KindProp);

	TSharedPtr<FJsonObject> SubkindProp = MakeShareable(new FJsonObject);
	SubkindProp->SetStringField(TEXT("type"), TEXT("string"));
	SubkindProp->SetStringField(
		TEXT("description"),
		TEXT("Optional sub-kind for audio or other outputs, e.g. 'sfx', 'music', or 'speech'. ")
		TEXT("This is used to choose between SFX, music, and speech Replicate models configured in settings when 'version' is omitted."));
	Properties->SetObjectField(TEXT("output_subkind"), SubkindProp);

	TSharedPtr<FJsonObject> InputImageProp = MakeShareable(new FJsonObject);
	InputImageProp->SetStringField(TEXT("type"), TEXT("string"));
	InputImageProp->SetStringField(
		TEXT("description"),
		TEXT("Optional input image for image-to-image models. Can be a project asset path (e.g. '/Game/Textures/MyTexture') or a file path on disk. ")
		TEXT("The image will be converted to PNG and sent to Replicate as a base64 data URI."));
	Properties->SetObjectField(TEXT("input_image"), InputImageProp);

	TSharedPtr<FJsonObject> InputImageParamProp = MakeShareable(new FJsonObject);
	InputImageParamProp->SetStringField(TEXT("type"), TEXT("string"));
	InputImageParamProp->SetStringField(
		TEXT("description"),
		TEXT("Optional name of the input parameter for the image. Defaults to 'image'. Some models use 'init_image', 'control_image', 'input_image', etc."));
	Properties->SetObjectField(TEXT("input_image_param"), InputImageParamProp);

	ReplicateParams->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShareable(new FJsonValueString(TEXT("prompt"))));
	ReplicateParams->SetArrayField(TEXT("required"), Required);

	return BuildToolObject(
		TEXT("replicate_generate"),
		TEXT("Generate content using Replicate (images, video, audio, or 3D files) via the Replicate HTTP API. ")
		TEXT("Supports image-to-image models by providing input_image (project texture asset path or file path). ")
		TEXT("Returns JSON with 'status', 'message', and 'details.files' containing local file paths for any downloaded outputs. ")
		TEXT("After calling this, use python_execute (for example with the 'unrealgpt_mcp_import' helpers) to import the files as Unreal assets, ")
		TEXT("then verify placement with scene_query and/or viewport_screenshot."),
		ReplicateParams,
		bUseResponsesApi);
}

// New atomic editor tools

TSharedPtr<FJsonObject> FUnrealGPTToolSchemas::BuildGetActorTool(bool bUseResponsesApi)
{
	TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
	Params->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShareable(new FJsonObject);

	TSharedPtr<FJsonObject> LabelProp = MakeShareable(new FJsonObject);
	LabelProp->SetStringField(TEXT("type"), TEXT("string"));
	LabelProp->SetStringField(TEXT("description"), TEXT("Actor label as shown in the World Outliner (preferred)."));
	Properties->SetObjectField(TEXT("label"), LabelProp);

	TSharedPtr<FJsonObject> NameProp = MakeShareable(new FJsonObject);
	NameProp->SetStringField(TEXT("type"), TEXT("string"));
	NameProp->SetStringField(TEXT("description"), TEXT("Actor object name (alternative to label)."));
	Properties->SetObjectField(TEXT("name"), NameProp);

	Params->SetObjectField(TEXT("properties"), Properties);

	return BuildToolObject(
		TEXT("get_actor"),
		TEXT("Get detailed information about a specific actor by label or name. ")
		TEXT("Returns JSON with actor details including transform, class, components, bounds, tags, and more. ")
		TEXT("Use this to inspect an actor before modifying it."),
		Params,
		bUseResponsesApi);
}

TSharedPtr<FJsonObject> FUnrealGPTToolSchemas::BuildSetActorTransformTool(bool bUseResponsesApi)
{
	TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
	Params->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShareable(new FJsonObject);

	TSharedPtr<FJsonObject> LabelProp = MakeShareable(new FJsonObject);
	LabelProp->SetStringField(TEXT("type"), TEXT("string"));
	LabelProp->SetStringField(TEXT("description"), TEXT("Actor label as shown in the World Outliner."));
	Properties->SetObjectField(TEXT("label"), LabelProp);

	TSharedPtr<FJsonObject> LocationProp = MakeShareable(new FJsonObject);
	LocationProp->SetStringField(TEXT("type"), TEXT("object"));
	LocationProp->SetStringField(TEXT("description"), TEXT("New location {x, y, z}. Omit to keep current."));
	Properties->SetObjectField(TEXT("location"), LocationProp);

	TSharedPtr<FJsonObject> RotationProp = MakeShareable(new FJsonObject);
	RotationProp->SetStringField(TEXT("type"), TEXT("object"));
	RotationProp->SetStringField(TEXT("description"), TEXT("New rotation {pitch, yaw, roll} in degrees. Omit to keep current."));
	Properties->SetObjectField(TEXT("rotation"), RotationProp);

	TSharedPtr<FJsonObject> ScaleProp = MakeShareable(new FJsonObject);
	ScaleProp->SetStringField(TEXT("type"), TEXT("object"));
	ScaleProp->SetStringField(TEXT("description"), TEXT("New scale {x, y, z}. Omit to keep current."));
	Properties->SetObjectField(TEXT("scale"), ScaleProp);

	Params->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShareable(new FJsonValueString(TEXT("label"))));
	Params->SetArrayField(TEXT("required"), Required);

	return BuildToolObject(
		TEXT("set_actor_transform"),
		TEXT("Set the transform (location, rotation, scale) of an actor by label. ")
		TEXT("Wrapped in an editor transaction for Undo support. ")
		TEXT("Only specify the components you want to change; others will remain unchanged."),
		Params,
		bUseResponsesApi);
}

TSharedPtr<FJsonObject> FUnrealGPTToolSchemas::BuildSelectActorsTool(bool bUseResponsesApi)
{
	TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
	Params->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShareable(new FJsonObject);

	TSharedPtr<FJsonObject> LabelsProp = MakeShareable(new FJsonObject);
	LabelsProp->SetStringField(TEXT("type"), TEXT("array"));
	LabelsProp->SetStringField(TEXT("description"), TEXT("Array of actor labels to select."));
	TSharedPtr<FJsonObject> ItemsProp = MakeShareable(new FJsonObject);
	ItemsProp->SetStringField(TEXT("type"), TEXT("string"));
	LabelsProp->SetObjectField(TEXT("items"), ItemsProp);
	Properties->SetObjectField(TEXT("labels"), LabelsProp);

	TSharedPtr<FJsonObject> AddToSelectionProp = MakeShareable(new FJsonObject);
	AddToSelectionProp->SetStringField(TEXT("type"), TEXT("boolean"));
	AddToSelectionProp->SetStringField(TEXT("description"), TEXT("If true, add to existing selection. If false (default), replace selection."));
	Properties->SetObjectField(TEXT("add_to_selection"), AddToSelectionProp);

	Params->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShareable(new FJsonValueString(TEXT("labels"))));
	Params->SetArrayField(TEXT("required"), Required);

	return BuildToolObject(
		TEXT("select_actors"),
		TEXT("Select one or more actors in the editor by their labels. ")
		TEXT("Useful for highlighting actors or preparing for batch operations."),
		Params,
		bUseResponsesApi);
}

TSharedPtr<FJsonObject> FUnrealGPTToolSchemas::BuildDuplicateActorTool(bool bUseResponsesApi)
{
	TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
	Params->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShareable(new FJsonObject);

	TSharedPtr<FJsonObject> LabelProp = MakeShareable(new FJsonObject);
	LabelProp->SetStringField(TEXT("type"), TEXT("string"));
	LabelProp->SetStringField(TEXT("description"), TEXT("Label of the actor to duplicate."));
	Properties->SetObjectField(TEXT("label"), LabelProp);

	TSharedPtr<FJsonObject> OffsetProp = MakeShareable(new FJsonObject);
	OffsetProp->SetStringField(TEXT("type"), TEXT("object"));
	OffsetProp->SetStringField(TEXT("description"), TEXT("Position offset {x, y, z} for the duplicate relative to original. Default is no offset."));
	Properties->SetObjectField(TEXT("offset"), OffsetProp);

	TSharedPtr<FJsonObject> NewLabelProp = MakeShareable(new FJsonObject);
	NewLabelProp->SetStringField(TEXT("type"), TEXT("string"));
	NewLabelProp->SetStringField(TEXT("description"), TEXT("Optional new label for the duplicated actor."));
	Properties->SetObjectField(TEXT("new_label"), NewLabelProp);

	Params->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShareable(new FJsonValueString(TEXT("label"))));
	Params->SetArrayField(TEXT("required"), Required);

	return BuildToolObject(
		TEXT("duplicate_actor"),
		TEXT("Duplicate an actor in the level. ")
		TEXT("Wrapped in an editor transaction for Undo support. ")
		TEXT("Returns the new actor's label and transform."),
		Params,
		bUseResponsesApi);
}

TSharedPtr<FJsonObject> FUnrealGPTToolSchemas::BuildSnapActorToGroundTool(bool bUseResponsesApi)
{
	TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
	Params->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShareable(new FJsonObject);

	TSharedPtr<FJsonObject> LabelProp = MakeShareable(new FJsonObject);
	LabelProp->SetStringField(TEXT("type"), TEXT("string"));
	LabelProp->SetStringField(TEXT("description"), TEXT("Label of the actor to snap to ground."));
	Properties->SetObjectField(TEXT("label"), LabelProp);

	TSharedPtr<FJsonObject> AlignToNormalProp = MakeShareable(new FJsonObject);
	AlignToNormalProp->SetStringField(TEXT("type"), TEXT("boolean"));
	AlignToNormalProp->SetStringField(TEXT("description"), TEXT("If true, align the actor's up vector to the surface normal. Default is false."));
	Properties->SetObjectField(TEXT("align_to_normal"), AlignToNormalProp);

	TSharedPtr<FJsonObject> OffsetProp = MakeShareable(new FJsonObject);
	OffsetProp->SetStringField(TEXT("type"), TEXT("number"));
	OffsetProp->SetStringField(TEXT("description"), TEXT("Vertical offset from the hit point. Use to prevent clipping or raise the actor above ground."));
	Properties->SetObjectField(TEXT("offset"), OffsetProp);

	Params->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShareable(new FJsonValueString(TEXT("label"))));
	Params->SetArrayField(TEXT("required"), Required);

	return BuildToolObject(
		TEXT("snap_actor_to_ground"),
		TEXT("Snap an actor to the ground using a line trace. ")
		TEXT("Wrapped in an editor transaction for Undo support. ")
		TEXT("Optionally align the actor to the surface normal."),
		Params,
		bUseResponsesApi);
}

TSharedPtr<FJsonObject> FUnrealGPTToolSchemas::BuildMcpListToolsTool(bool bUseResponsesApi)
{
	TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
	Params->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShareable(new FJsonObject);
	TSharedPtr<FJsonObject> ServerProp = MakeShareable(new FJsonObject);
	ServerProp->SetStringField(TEXT("type"), TEXT("string"));
	ServerProp->SetStringField(TEXT("description"), TEXT("Optional MCP server name filter. Omit to list tools from all configured servers."));
	Properties->SetObjectField(TEXT("server"), ServerProp);
	Params->SetObjectField(TEXT("properties"), Properties);

	return BuildToolObject(
		TEXT("mcp_list_tools"),
		TEXT("List tools exposed by configured MCP servers in Project Settings."),
		Params,
		bUseResponsesApi);
}

TSharedPtr<FJsonObject> FUnrealGPTToolSchemas::BuildMcpCallTool(bool bUseResponsesApi)
{
	TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
	Params->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShareable(new FJsonObject);

	TSharedPtr<FJsonObject> ServerProp = MakeShareable(new FJsonObject);
	ServerProp->SetStringField(TEXT("type"), TEXT("string"));
	ServerProp->SetStringField(TEXT("description"), TEXT("Configured MCP server name."));
	Properties->SetObjectField(TEXT("server"), ServerProp);

	TSharedPtr<FJsonObject> ToolProp = MakeShareable(new FJsonObject);
	ToolProp->SetStringField(TEXT("type"), TEXT("string"));
	ToolProp->SetStringField(TEXT("description"), TEXT("Remote MCP tool name to invoke."));
	Properties->SetObjectField(TEXT("tool"), ToolProp);

	TSharedPtr<FJsonObject> ArgsProp = MakeShareable(new FJsonObject);
	ArgsProp->SetStringField(TEXT("type"), TEXT("object"));
	ArgsProp->SetStringField(TEXT("description"), TEXT("Arguments object passed to the remote MCP tool."));
	Properties->SetObjectField(TEXT("arguments"), ArgsProp);

	TSharedPtr<FJsonObject> KindProp = MakeShareable(new FJsonObject);
	KindProp->SetStringField(TEXT("type"), TEXT("string"));
	KindProp->SetStringField(TEXT("description"), TEXT("Optional output kind hint for staging/import: image, audio, video, 3d."));
	Properties->SetObjectField(TEXT("output_kind"), KindProp);

	Params->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShareable(new FJsonValueString(TEXT("server"))));
	Required.Add(MakeShareable(new FJsonValueString(TEXT("tool"))));
	Params->SetArrayField(TEXT("required"), Required);

	return BuildToolObject(
		TEXT("mcp_call"),
		TEXT("Call a tool on a configured MCP server. Returns JSON with status, message, and details.files containing local paths for downloaded outputs. ")
		TEXT("Use python_execute with unrealgpt_mcp_import helpers to import returned files into Unreal assets."),
		Params,
		bUseResponsesApi);
}

TSharedPtr<FJsonObject> FUnrealGPTToolSchemas::BuildMcpReadResourceTool(bool bUseResponsesApi)
{
	TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
	Params->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShareable(new FJsonObject);

	TSharedPtr<FJsonObject> ServerProp = MakeShareable(new FJsonObject);
	ServerProp->SetStringField(TEXT("type"), TEXT("string"));
	Properties->SetObjectField(TEXT("server"), ServerProp);

	TSharedPtr<FJsonObject> UriProp = MakeShareable(new FJsonObject);
	UriProp->SetStringField(TEXT("type"), TEXT("string"));
	UriProp->SetStringField(TEXT("description"), TEXT("MCP resource URI to read."));
	Properties->SetObjectField(TEXT("uri"), UriProp);

	Params->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShareable(new FJsonValueString(TEXT("server"))));
	Required.Add(MakeShareable(new FJsonValueString(TEXT("uri"))));
	Params->SetArrayField(TEXT("required"), Required);

	return BuildToolObject(
		TEXT("mcp_read_resource"),
		TEXT("Read a resource from a configured MCP server."),
		Params,
		bUseResponsesApi);
}

TSharedPtr<FJsonObject> FUnrealGPTToolSchemas::BuildMcpGetPromptTool(bool bUseResponsesApi)
{
	TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
	Params->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShareable(new FJsonObject);

	TSharedPtr<FJsonObject> ServerProp = MakeShareable(new FJsonObject);
	ServerProp->SetStringField(TEXT("type"), TEXT("string"));
	Properties->SetObjectField(TEXT("server"), ServerProp);

	TSharedPtr<FJsonObject> NameProp = MakeShareable(new FJsonObject);
	NameProp->SetStringField(TEXT("type"), TEXT("string"));
	Properties->SetObjectField(TEXT("name"), NameProp);

	TSharedPtr<FJsonObject> ArgsProp = MakeShareable(new FJsonObject);
	ArgsProp->SetStringField(TEXT("type"), TEXT("object"));
	Properties->SetObjectField(TEXT("arguments"), ArgsProp);

	Params->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShareable(new FJsonValueString(TEXT("server"))));
	Required.Add(MakeShareable(new FJsonValueString(TEXT("name"))));
	Params->SetArrayField(TEXT("required"), Required);

	return BuildToolObject(
		TEXT("mcp_get_prompt"),
		TEXT("Fetch a prompt template from a configured MCP server."),
		Params,
		bUseResponsesApi);
}

TSharedPtr<FJsonObject> FUnrealGPTToolSchemas::BuildClarifyTool(bool bUseResponsesApi)
{
	TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
	Params->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShareable(new FJsonObject);

	TSharedPtr<FJsonObject> TitleProp = MakeShareable(new FJsonObject);
	TitleProp->SetStringField(TEXT("type"), TEXT("string"));
	TitleProp->SetStringField(TEXT("description"), TEXT("Optional heading shown above the clarification form."));
	Properties->SetObjectField(TEXT("title"), TitleProp);

	TSharedPtr<FJsonObject> OptionItem = MakeShareable(new FJsonObject);
	OptionItem->SetStringField(TEXT("type"), TEXT("object"));
	{
		TSharedPtr<FJsonObject> OptionProps = MakeShareable(new FJsonObject);
		TSharedPtr<FJsonObject> OptionIdProp = MakeShareable(new FJsonObject);
		OptionIdProp->SetStringField(TEXT("type"), TEXT("string"));
		OptionProps->SetObjectField(TEXT("id"), OptionIdProp);
		TSharedPtr<FJsonObject> OptionLabelProp = MakeShareable(new FJsonObject);
		OptionLabelProp->SetStringField(TEXT("type"), TEXT("string"));
		OptionProps->SetObjectField(TEXT("label"), OptionLabelProp);
		OptionItem->SetObjectField(TEXT("properties"), OptionProps);
		TArray<TSharedPtr<FJsonValue>> OptionRequired;
		OptionRequired.Add(MakeShareable(new FJsonValueString(TEXT("id"))));
		OptionRequired.Add(MakeShareable(new FJsonValueString(TEXT("label"))));
		OptionItem->SetArrayField(TEXT("required"), OptionRequired);
	}

	TSharedPtr<FJsonObject> QuestionItem = MakeShareable(new FJsonObject);
	QuestionItem->SetStringField(TEXT("type"), TEXT("object"));
	{
		TSharedPtr<FJsonObject> QuestionProps = MakeShareable(new FJsonObject);
		TSharedPtr<FJsonObject> QuestionIdProp = MakeShareable(new FJsonObject);
		QuestionIdProp->SetStringField(TEXT("type"), TEXT("string"));
		QuestionProps->SetObjectField(TEXT("id"), QuestionIdProp);
		TSharedPtr<FJsonObject> PromptProp = MakeShareable(new FJsonObject);
		PromptProp->SetStringField(TEXT("type"), TEXT("string"));
		QuestionProps->SetObjectField(TEXT("prompt"), PromptProp);
		TSharedPtr<FJsonObject> OptionsProp = MakeShareable(new FJsonObject);
		OptionsProp->SetStringField(TEXT("type"), TEXT("array"));
		OptionsProp->SetObjectField(TEXT("items"), OptionItem);
		QuestionProps->SetObjectField(TEXT("options"), OptionsProp);
		TSharedPtr<FJsonObject> AllowMultipleProp = MakeShareable(new FJsonObject);
		AllowMultipleProp->SetStringField(TEXT("type"), TEXT("boolean"));
		AllowMultipleProp->SetStringField(TEXT("description"), TEXT("If true, the user may select multiple options. Default is false."));
		QuestionProps->SetObjectField(TEXT("allow_multiple"), AllowMultipleProp);
		QuestionItem->SetObjectField(TEXT("properties"), QuestionProps);
		TArray<TSharedPtr<FJsonValue>> QuestionRequired;
		QuestionRequired.Add(MakeShareable(new FJsonValueString(TEXT("id"))));
		QuestionRequired.Add(MakeShareable(new FJsonValueString(TEXT("prompt"))));
		QuestionRequired.Add(MakeShareable(new FJsonValueString(TEXT("options"))));
		QuestionItem->SetArrayField(TEXT("required"), QuestionRequired);
	}

	TSharedPtr<FJsonObject> QuestionsProp = MakeShareable(new FJsonObject);
	QuestionsProp->SetStringField(TEXT("type"), TEXT("array"));
	QuestionsProp->SetStringField(TEXT("description"), TEXT("One to five questions to present to the user."));
	QuestionsProp->SetObjectField(TEXT("items"), QuestionItem);
	Properties->SetObjectField(TEXT("questions"), QuestionsProp);

	Params->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShareable(new FJsonValueString(TEXT("questions"))));
	Params->SetArrayField(TEXT("required"), Required);

	return BuildToolObject(
		TEXT("clarify"),
		TEXT("Ask the user to choose between concrete options when you need a decision before proceeding. ")
		TEXT("Renders an interactive form in the chat UI. Use when 2+ distinct paths exist; provide 2-4 concise options per question. ")
		TEXT("Prefer one clarify call with multiple questions over multiple round-trips. Do NOT use for open-ended questions—ask those in plain text instead. ")
		TEXT("An automatic 'Other' option with free-text is always available to the user."),
		Params,
		bUseResponsesApi);
}

namespace UnrealGPTToolSchemasPrivate
{
	static void AddAssetPathProperty(const TSharedPtr<FJsonObject>& Properties)
	{
		TSharedPtr<FJsonObject> AssetPathProp = MakeShareable(new FJsonObject);
		AssetPathProp->SetStringField(TEXT("type"), TEXT("string"));
		AssetPathProp->SetStringField(TEXT("description"), TEXT("Blueprint asset path (e.g. /Game/Folder/BP_MyActor or /Game/Folder/BP_MyActor.BP_MyActor)."));
		Properties->SetObjectField(TEXT("asset_path"), AssetPathProp);
	}

	static void AddGraphNameProperty(const TSharedPtr<FJsonObject>& Properties)
	{
		TSharedPtr<FJsonObject> GraphNameProp = MakeShareable(new FJsonObject);
		GraphNameProp->SetStringField(TEXT("type"), TEXT("string"));
		GraphNameProp->SetStringField(TEXT("description"), TEXT("Optional graph name. Defaults to the primary event graph."));
		Properties->SetObjectField(TEXT("graph_name"), GraphNameProp);
	}
}

TSharedPtr<FJsonObject> FUnrealGPTToolSchemas::BuildBlueprintQueryTool(bool bUseResponsesApi)
{
	using namespace UnrealGPTToolSchemasPrivate;

	TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
	Params->SetStringField(TEXT("type"), TEXT("object"));
	TSharedPtr<FJsonObject> Properties = MakeShareable(new FJsonObject);
	AddAssetPathProperty(Properties);
	AddGraphNameProperty(Properties);

	TSharedPtr<FJsonObject> NodeGuidProp = MakeShareable(new FJsonObject);
	NodeGuidProp->SetStringField(TEXT("type"), TEXT("string"));
	NodeGuidProp->SetStringField(TEXT("description"), TEXT("Optional node GUID filter."));
	Properties->SetObjectField(TEXT("node_guid"), NodeGuidProp);

	TSharedPtr<FJsonObject> IncludePinsProp = MakeShareable(new FJsonObject);
	IncludePinsProp->SetStringField(TEXT("type"), TEXT("boolean"));
	IncludePinsProp->SetStringField(TEXT("description"), TEXT("Include pin details and connections (default true)."));
	IncludePinsProp->SetBoolField(TEXT("default"), true);
	Properties->SetObjectField(TEXT("include_pins"), IncludePinsProp);

	Params->SetObjectField(TEXT("properties"), Properties);
	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShareable(new FJsonValueString(TEXT("asset_path"))));
	Params->SetArrayField(TEXT("required"), Required);

	return BuildToolObject(
		TEXT("blueprint_query"),
		TEXT("Read a Blueprint asset: graphs, nodes, pins, connections, variables, components, and compile status. ")
		TEXT("Always call this before editing a Blueprint graph."),
		Params,
		bUseResponsesApi);
}

TSharedPtr<FJsonObject> FUnrealGPTToolSchemas::BuildBlueprintCreateTool(bool bUseResponsesApi)
{
	TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
	Params->SetStringField(TEXT("type"), TEXT("object"));
	TSharedPtr<FJsonObject> Properties = MakeShareable(new FJsonObject);

	TSharedPtr<FJsonObject> AssetPathProp = MakeShareable(new FJsonObject);
	AssetPathProp->SetStringField(TEXT("type"), TEXT("string"));
	AssetPathProp->SetStringField(TEXT("description"), TEXT("Full asset path for the new Blueprint (e.g. /Game/Folder/BP_MyActor)."));
	Properties->SetObjectField(TEXT("asset_path"), AssetPathProp);

	TSharedPtr<FJsonObject> ParentClassProp = MakeShareable(new FJsonObject);
	ParentClassProp->SetStringField(TEXT("type"), TEXT("string"));
	ParentClassProp->SetStringField(TEXT("description"), TEXT("Parent class path (default /Script/Engine.Actor)."));
	ParentClassProp->SetStringField(TEXT("default"), TEXT("/Script/Engine.Actor"));
	Properties->SetObjectField(TEXT("parent_class"), ParentClassProp);

	Params->SetObjectField(TEXT("properties"), Properties);
	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShareable(new FJsonValueString(TEXT("asset_path"))));
	Params->SetArrayField(TEXT("required"), Required);

	return BuildToolObject(
		TEXT("blueprint_create"),
		TEXT("Create a new Actor Blueprint asset at the given path."),
		Params,
		bUseResponsesApi);
}

TSharedPtr<FJsonObject> FUnrealGPTToolSchemas::BuildBlueprintAddVariableTool(bool bUseResponsesApi)
{
	using namespace UnrealGPTToolSchemasPrivate;

	TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
	Params->SetStringField(TEXT("type"), TEXT("object"));
	TSharedPtr<FJsonObject> Properties = MakeShareable(new FJsonObject);
	AddAssetPathProperty(Properties);

	TSharedPtr<FJsonObject> NameProp = MakeShareable(new FJsonObject);
	NameProp->SetStringField(TEXT("type"), TEXT("string"));
	Properties->SetObjectField(TEXT("name"), NameProp);

	TSharedPtr<FJsonObject> TypeProp = MakeShareable(new FJsonObject);
	TypeProp->SetStringField(TEXT("type"), TEXT("string"));
	TypeProp->SetStringField(TEXT("description"), TEXT("Variable type: bool, int, float, string, name, text, vector, rotator, transform, object, class."));
	TypeProp->SetStringField(TEXT("default"), TEXT("bool"));
	Properties->SetObjectField(TEXT("type"), TypeProp);

	TSharedPtr<FJsonObject> SubTypeProp = MakeShareable(new FJsonObject);
	SubTypeProp->SetStringField(TEXT("type"), TEXT("string"));
	SubTypeProp->SetStringField(TEXT("description"), TEXT("For object/class types, the sub-type path (e.g. /Script/Engine.Actor)."));
	Properties->SetObjectField(TEXT("sub_type_object"), SubTypeProp);

	TSharedPtr<FJsonObject> DefaultValueProp = MakeShareable(new FJsonObject);
	DefaultValueProp->SetStringField(TEXT("type"), TEXT("string"));
	Properties->SetObjectField(TEXT("default_value"), DefaultValueProp);

	TSharedPtr<FJsonObject> InstanceEditableProp = MakeShareable(new FJsonObject);
	InstanceEditableProp->SetStringField(TEXT("type"), TEXT("boolean"));
	Properties->SetObjectField(TEXT("instance_editable"), InstanceEditableProp);

	TSharedPtr<FJsonObject> ExposeOnSpawnProp = MakeShareable(new FJsonObject);
	ExposeOnSpawnProp->SetStringField(TEXT("type"), TEXT("boolean"));
	Properties->SetObjectField(TEXT("expose_on_spawn"), ExposeOnSpawnProp);

	Params->SetObjectField(TEXT("properties"), Properties);
	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShareable(new FJsonValueString(TEXT("asset_path"))));
	Required.Add(MakeShareable(new FJsonValueString(TEXT("name"))));
	Params->SetArrayField(TEXT("required"), Required);

	return BuildToolObject(
		TEXT("blueprint_add_variable"),
		TEXT("Add a member variable to a Blueprint."),
		Params,
		bUseResponsesApi);
}

TSharedPtr<FJsonObject> FUnrealGPTToolSchemas::BuildBlueprintCompileTool(bool bUseResponsesApi)
{
	using namespace UnrealGPTToolSchemasPrivate;

	TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
	Params->SetStringField(TEXT("type"), TEXT("object"));
	TSharedPtr<FJsonObject> Properties = MakeShareable(new FJsonObject);
	AddAssetPathProperty(Properties);
	Params->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShareable(new FJsonValueString(TEXT("asset_path"))));
	Params->SetArrayField(TEXT("required"), Required);

	return BuildToolObject(
		TEXT("blueprint_compile"),
		TEXT("Compile a Blueprint and return compile status/errors."),
		Params,
		bUseResponsesApi);
}

TSharedPtr<FJsonObject> FUnrealGPTToolSchemas::BuildBlueprintAddNodeTool(bool bUseResponsesApi)
{
	using namespace UnrealGPTToolSchemasPrivate;

	TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
	Params->SetStringField(TEXT("type"), TEXT("object"));
	TSharedPtr<FJsonObject> Properties = MakeShareable(new FJsonObject);
	AddAssetPathProperty(Properties);
	AddGraphNameProperty(Properties);

	TSharedPtr<FJsonObject> NodeTypeProp = MakeShareable(new FJsonObject);
	NodeTypeProp->SetStringField(TEXT("type"), TEXT("string"));
	NodeTypeProp->SetStringField(TEXT("description"), TEXT("Node type: Event, CallFunction, VariableGet, VariableSet, CustomEvent, Branch, Sequence, Self, PrintString."));
	Properties->SetObjectField(TEXT("node_type"), NodeTypeProp);

	TSharedPtr<FJsonObject> PosXProp = MakeShareable(new FJsonObject);
	PosXProp->SetStringField(TEXT("type"), TEXT("number"));
	Properties->SetObjectField(TEXT("pos_x"), PosXProp);

	TSharedPtr<FJsonObject> PosYProp = MakeShareable(new FJsonObject);
	PosYProp->SetStringField(TEXT("type"), TEXT("number"));
	Properties->SetObjectField(TEXT("pos_y"), PosYProp);

	TSharedPtr<FJsonObject> ParamsProp = MakeShareable(new FJsonObject);
	ParamsProp->SetStringField(TEXT("type"), TEXT("object"));
	ParamsProp->SetStringField(TEXT("description"), TEXT("Type-specific params: event_name, function_name, target_class, variable_name."));
	Properties->SetObjectField(TEXT("params"), ParamsProp);

	Params->SetObjectField(TEXT("properties"), Properties);
	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShareable(new FJsonValueString(TEXT("asset_path"))));
	Required.Add(MakeShareable(new FJsonValueString(TEXT("node_type"))));
	Params->SetArrayField(TEXT("required"), Required);

	return BuildToolObject(
		TEXT("blueprint_add_node"),
		TEXT("Add a K2 node to a Blueprint graph. Returns node_guid for subsequent pin connections."),
		Params,
		bUseResponsesApi);
}

TSharedPtr<FJsonObject> FUnrealGPTToolSchemas::BuildBlueprintConnectPinsTool(bool bUseResponsesApi)
{
	using namespace UnrealGPTToolSchemasPrivate;

	TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
	Params->SetStringField(TEXT("type"), TEXT("object"));
	TSharedPtr<FJsonObject> Properties = MakeShareable(new FJsonObject);
	AddAssetPathProperty(Properties);
	AddGraphNameProperty(Properties);

	auto AddStringProp = [&Properties](const TCHAR* Name, const TCHAR* Description)
	{
		TSharedPtr<FJsonObject> Prop = MakeShareable(new FJsonObject);
		Prop->SetStringField(TEXT("type"), TEXT("string"));
		Prop->SetStringField(TEXT("description"), Description);
		Properties->SetObjectField(Name, Prop);
	};

	AddStringProp(TEXT("from_node_guid"), TEXT("Source node GUID from blueprint_query."));
	AddStringProp(TEXT("from_pin"), TEXT("Source pin name (e.g. then, ReturnValue)."));
	AddStringProp(TEXT("to_node_guid"), TEXT("Target node GUID from blueprint_query."));
	AddStringProp(TEXT("to_pin"), TEXT("Target pin name (e.g. execute, Condition)."));

	Params->SetObjectField(TEXT("properties"), Properties);
	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShareable(new FJsonValueString(TEXT("asset_path"))));
	Required.Add(MakeShareable(new FJsonValueString(TEXT("from_node_guid"))));
	Required.Add(MakeShareable(new FJsonValueString(TEXT("from_pin"))));
	Required.Add(MakeShareable(new FJsonValueString(TEXT("to_node_guid"))));
	Required.Add(MakeShareable(new FJsonValueString(TEXT("to_pin"))));
	Params->SetArrayField(TEXT("required"), Required);

	return BuildToolObject(
		TEXT("blueprint_connect_pins"),
		TEXT("Connect two Blueprint pins by node GUID and pin name."),
		Params,
		bUseResponsesApi);
}

TSharedPtr<FJsonObject> FUnrealGPTToolSchemas::BuildBlueprintRemoveNodeTool(bool bUseResponsesApi)
{
	using namespace UnrealGPTToolSchemasPrivate;

	TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
	Params->SetStringField(TEXT("type"), TEXT("object"));
	TSharedPtr<FJsonObject> Properties = MakeShareable(new FJsonObject);
	AddAssetPathProperty(Properties);
	AddGraphNameProperty(Properties);

	TSharedPtr<FJsonObject> NodeGuidProp = MakeShareable(new FJsonObject);
	NodeGuidProp->SetStringField(TEXT("type"), TEXT("string"));
	Properties->SetObjectField(TEXT("node_guid"), NodeGuidProp);

	Params->SetObjectField(TEXT("properties"), Properties);
	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShareable(new FJsonValueString(TEXT("asset_path"))));
	Required.Add(MakeShareable(new FJsonValueString(TEXT("node_guid"))));
	Params->SetArrayField(TEXT("required"), Required);

	return BuildToolObject(
		TEXT("blueprint_remove_node"),
		TEXT("Remove a node from a Blueprint graph by node GUID."),
		Params,
		bUseResponsesApi);
}

TSharedPtr<FJsonObject> FUnrealGPTToolSchemas::BuildBlueprintSetPinDefaultTool(bool bUseResponsesApi)
{
	using namespace UnrealGPTToolSchemasPrivate;

	TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
	Params->SetStringField(TEXT("type"), TEXT("object"));
	TSharedPtr<FJsonObject> Properties = MakeShareable(new FJsonObject);
	AddAssetPathProperty(Properties);
	AddGraphNameProperty(Properties);

	TSharedPtr<FJsonObject> NodeGuidProp = MakeShareable(new FJsonObject);
	NodeGuidProp->SetStringField(TEXT("type"), TEXT("string"));
	Properties->SetObjectField(TEXT("node_guid"), NodeGuidProp);

	TSharedPtr<FJsonObject> PinNameProp = MakeShareable(new FJsonObject);
	PinNameProp->SetStringField(TEXT("type"), TEXT("string"));
	Properties->SetObjectField(TEXT("pin_name"), PinNameProp);

	TSharedPtr<FJsonObject> ValueProp = MakeShareable(new FJsonObject);
	ValueProp->SetStringField(TEXT("type"), TEXT("string"));
	Properties->SetObjectField(TEXT("value"), ValueProp);

	Params->SetObjectField(TEXT("properties"), Properties);
	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShareable(new FJsonValueString(TEXT("asset_path"))));
	Required.Add(MakeShareable(new FJsonValueString(TEXT("node_guid"))));
	Required.Add(MakeShareable(new FJsonValueString(TEXT("pin_name"))));
	Required.Add(MakeShareable(new FJsonValueString(TEXT("value"))));
	Params->SetArrayField(TEXT("required"), Required);

	return BuildToolObject(
		TEXT("blueprint_set_pin_default"),
		TEXT("Set the default/literal value on a Blueprint input pin."),
		Params,
		bUseResponsesApi);
}

