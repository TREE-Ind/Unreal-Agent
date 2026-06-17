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
		TEXT("Name or path of the UClass to inspect. You can pass a short name like 'StaticMeshActor' or a fully qualified path like '/Script/Engine.StaticMeshActor'."));
	Properties->SetObjectField(TEXT("class_name"), ClassNameProp);

	ReflectionParams->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShareable(new FJsonValueString(TEXT("class_name"))));
	ReflectionParams->SetArrayField(TEXT("required"), Required);

	return BuildToolObject(
		TEXT("reflection_query"),
		TEXT("Inspect an Unreal UClass via the reflection system at runtime. ")
		TEXT("Given a class_name (C++ or Blueprint), this returns a JSON schema describing its reflected properties and functions, ")
		TEXT("including names, C++ types, and high-signal flags that matter for Python/Blueprint access."),
		ReflectionParams,
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

