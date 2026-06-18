// Copyright (c) 2025 TREE Industries.

#include "UnrealGPTBlueprintContext.h"

#include "Editor.h"
#include "UnrealGPTBlueprintGraph.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/SavePackage.h"

namespace UnrealGPTBlueprintContextPrivate
{
	static FString SerializeJson(const TSharedPtr<FJsonObject>& Root)
	{
		return FUnrealGPTBlueprintGraph::SerializeJson(Root);
	}

	static TSharedPtr<FJsonObject> ParseArgs(const FString& ArgumentsJson, FString& OutError)
	{
		TSharedPtr<FJsonObject> ArgsObj;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ArgumentsJson);
		if (!(FJsonSerializer::Deserialize(Reader, ArgsObj) && ArgsObj.IsValid()))
		{
			OutError = TEXT("Failed to parse arguments JSON");
		}
		return ArgsObj;
	}

	static FString MakeOk(const FString& Message, const TSharedPtr<FJsonObject>& Details = nullptr)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("status"), TEXT("ok"));
		Root->SetStringField(TEXT("message"), Message);
		if (Details.IsValid())
		{
			Root->SetObjectField(TEXT("details"), Details);
		}
		return SerializeJson(Root);
	}

	static FString ReturnErrorJson(const FString& Message)
	{
		return SerializeJson(FUnrealGPTBlueprintGraph::MakeError(Message));
	}

	static bool BeginWriteTransaction(const FString& Description)
	{
		if (GEditor)
		{
			GEditor->BeginTransaction(FText::FromString(Description));
			return true;
		}
		return false;
	}

	static void EndWriteTransaction()
	{
		if (GEditor)
		{
			GEditor->EndTransaction();
		}
	}

	static FString GetRequiredAssetPath(const TSharedPtr<FJsonObject>& Args, FString& OutError)
	{
		FString AssetPath;
		if (!Args->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		{
			OutError = TEXT("Missing required field: asset_path");
			return FString();
		}
		return FUnrealGPTBlueprintGraph::NormalizeAssetPath(AssetPath);
	}
}

FString UUnrealGPTBlueprintContext::Query(const FString& ArgumentsJson)
{
	using namespace UnrealGPTBlueprintContextPrivate;

	FString ParseError;
	const TSharedPtr<FJsonObject> Args = ParseArgs(ArgumentsJson, ParseError);
	if (!Args.IsValid())
	{
		return ReturnErrorJson(ParseError);
	}

	const FString AssetPath = GetRequiredAssetPath(Args, ParseError);
	if (AssetPath.IsEmpty())
	{
		return ReturnErrorJson(ParseError);
	}

	FString LoadError;
	UBlueprint* Blueprint = FUnrealGPTBlueprintGraph::LoadBlueprint(AssetPath, LoadError);
	if (!Blueprint)
	{
		return SerializeJson(FUnrealGPTBlueprintGraph::MakeError(LoadError));
	}

	FString GraphName;
	Args->TryGetStringField(TEXT("graph_name"), GraphName);

	FString NodeGuid;
	Args->TryGetStringField(TEXT("node_guid"), NodeGuid);

	bool bIncludePins = true;
	Args->TryGetBoolField(TEXT("include_pins"), bIncludePins);

	const TSharedPtr<FJsonObject> Details = FUnrealGPTBlueprintGraph::SerializeBlueprintSummary(
		Blueprint, AssetPath, GraphName, NodeGuid, bIncludePins);
	return MakeOk(TEXT("Blueprint query succeeded"), Details);
}

FString UUnrealGPTBlueprintContext::Create(const FString& ArgumentsJson)
{
	using namespace UnrealGPTBlueprintContextPrivate;

	FString ParseError;
	const TSharedPtr<FJsonObject> Args = ParseArgs(ArgumentsJson, ParseError);
	if (!Args.IsValid())
	{
		return ReturnErrorJson(ParseError);
	}

	FString AssetPath;
	if (!Args->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return ReturnErrorJson(TEXT("Missing required field: asset_path"));
	}
	AssetPath = FUnrealGPTBlueprintGraph::NormalizeAssetPath(AssetPath);

	FString ParentClassName;
	if (!Args->TryGetStringField(TEXT("parent_class"), ParentClassName) || ParentClassName.IsEmpty())
	{
		ParentClassName = TEXT("/Script/Engine.Actor");
	}

	FString ClassError;
	UClass* ParentClass = FUnrealGPTBlueprintGraph::ResolveParentClass(ParentClassName, ClassError);
	if (!ParentClass)
	{
		return ReturnErrorJson(ClassError);
	}

	const FString PackageName = FPackageName::ObjectPathToPackageName(AssetPath);
	const FName AssetName(*FPaths::GetBaseFilename(AssetPath));

	if (LoadObject<UBlueprint>(nullptr, *AssetPath))
	{
		return ReturnErrorJson(FString::Printf(TEXT("Blueprint already exists at '%s'"), *AssetPath));
	}

	const bool bTransaction = BeginWriteTransaction(TEXT("Create Blueprint"));
	UPackage* Package = CreatePackage(*PackageName);
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		ParentClass,
		Package,
		AssetName,
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass(),
		FName("CreateBlueprint"));

	if (!Blueprint)
	{
		if (bTransaction)
		{
			EndWriteTransaction();
		}
		return ReturnErrorJson(TEXT("Failed to create blueprint asset"));
	}

	FAssetRegistryModule::AssetCreated(Blueprint);
	Package->MarkPackageDirty();

	if (bTransaction)
	{
		EndWriteTransaction();
	}

	TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
	Details->SetStringField(TEXT("asset_path"), AssetPath);
	Details->SetStringField(TEXT("parent_class"), ParentClass->GetPathName());
	return MakeOk(TEXT("Blueprint created"), Details);
}

FString UUnrealGPTBlueprintContext::AddVariable(const FString& ArgumentsJson)
{
	using namespace UnrealGPTBlueprintContextPrivate;

	FString ParseError;
	const TSharedPtr<FJsonObject> Args = ParseArgs(ArgumentsJson, ParseError);
	if (!Args.IsValid())
	{
		return ReturnErrorJson(ParseError);
	}

	const FString AssetPath = GetRequiredAssetPath(Args, ParseError);
	if (AssetPath.IsEmpty())
	{
		return ReturnErrorJson(ParseError);
	}

	FString VarName;
	if (!Args->TryGetStringField(TEXT("name"), VarName) || VarName.IsEmpty())
	{
		return ReturnErrorJson(TEXT("Missing required field: name"));
	}

	FString TypeName = TEXT("bool");
	Args->TryGetStringField(TEXT("type"), TypeName);

	FString SubTypeObject;
	Args->TryGetStringField(TEXT("sub_type_object"), SubTypeObject);

	FEdGraphPinType PinType;
	FString TypeError;
	if (!FUnrealGPTBlueprintGraph::ParsePinType(TypeName, SubTypeObject, PinType, TypeError))
	{
		return ReturnErrorJson(TypeError);
	}

	FString DefaultValue;
	Args->TryGetStringField(TEXT("default_value"), DefaultValue);

	bool bInstanceEditable = false;
	bool bExposeOnSpawn = false;
	Args->TryGetBoolField(TEXT("instance_editable"), bInstanceEditable);
	Args->TryGetBoolField(TEXT("expose_on_spawn"), bExposeOnSpawn);

	FString LoadError;
	UBlueprint* Blueprint = FUnrealGPTBlueprintGraph::LoadBlueprint(AssetPath, LoadError);
	if (!Blueprint)
	{
		return SerializeJson(FUnrealGPTBlueprintGraph::MakeError(LoadError));
	}

	const bool bTransaction = BeginWriteTransaction(TEXT("Add Blueprint Variable"));
	FString VarError;
	const bool bSuccess = FUnrealGPTBlueprintGraph::AddMemberVariable(
		Blueprint, VarName, PinType, DefaultValue, bInstanceEditable, bExposeOnSpawn, VarError);
	if (bTransaction)
	{
		EndWriteTransaction();
	}

	if (!bSuccess)
	{
		return ReturnErrorJson(VarError);
	}

	TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
	Details->SetStringField(TEXT("asset_path"), AssetPath);
	Details->SetStringField(TEXT("name"), VarName);
	Details->SetStringField(TEXT("type"), TypeName);
	return MakeOk(TEXT("Blueprint variable added"), Details);
}

FString UUnrealGPTBlueprintContext::Compile(const FString& ArgumentsJson)
{
	using namespace UnrealGPTBlueprintContextPrivate;

	FString ParseError;
	const TSharedPtr<FJsonObject> Args = ParseArgs(ArgumentsJson, ParseError);
	if (!Args.IsValid())
	{
		return ReturnErrorJson(ParseError);
	}

	const FString AssetPath = GetRequiredAssetPath(Args, ParseError);
	if (AssetPath.IsEmpty())
	{
		return ReturnErrorJson(ParseError);
	}

	FString LoadError;
	UBlueprint* Blueprint = FUnrealGPTBlueprintGraph::LoadBlueprint(AssetPath, LoadError);
	if (!Blueprint)
	{
		return SerializeJson(FUnrealGPTBlueprintGraph::MakeError(LoadError));
	}

	TArray<FString> Errors;
	TArray<FString> Warnings;
	FString Status;
	const bool bSuccess = FUnrealGPTBlueprintGraph::CompileBlueprint(Blueprint, Errors, Warnings, Status);

	TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
	Details->SetStringField(TEXT("asset_path"), AssetPath);
	Details->SetStringField(TEXT("compile_status"), Status);
	Details->SetBoolField(TEXT("success"), bSuccess);

	TArray<TSharedPtr<FJsonValue>> ErrorValues;
	for (const FString& Error : Errors)
	{
		ErrorValues.Add(MakeShared<FJsonValueString>(Error));
	}
	Details->SetArrayField(TEXT("compile_errors"), ErrorValues);

	TArray<TSharedPtr<FJsonValue>> WarningValues;
	for (const FString& Warning : Warnings)
	{
		WarningValues.Add(MakeShared<FJsonValueString>(Warning));
	}
	Details->SetArrayField(TEXT("compile_warnings"), WarningValues);

	if (!bSuccess)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("status"), TEXT("error"));
		Root->SetStringField(TEXT("message"), TEXT("Blueprint compile failed"));
		Root->SetObjectField(TEXT("details"), Details);
		return SerializeJson(Root);
	}

	return MakeOk(TEXT("Blueprint compiled successfully"), Details);
}

FString UUnrealGPTBlueprintContext::AddNode(const FString& ArgumentsJson)
{
	using namespace UnrealGPTBlueprintContextPrivate;

	FString ParseError;
	const TSharedPtr<FJsonObject> Args = ParseArgs(ArgumentsJson, ParseError);
	if (!Args.IsValid())
	{
		return ReturnErrorJson(ParseError);
	}

	const FString AssetPath = GetRequiredAssetPath(Args, ParseError);
	if (AssetPath.IsEmpty())
	{
		return ReturnErrorJson(ParseError);
	}

	FString NodeType;
	if (!Args->TryGetStringField(TEXT("node_type"), NodeType) || NodeType.IsEmpty())
	{
		return ReturnErrorJson(TEXT("Missing required field: node_type"));
	}

	FString GraphName;
	Args->TryGetStringField(TEXT("graph_name"), GraphName);

	double PosX = 0.0;
	double PosY = 0.0;
	Args->TryGetNumberField(TEXT("pos_x"), PosX);
	Args->TryGetNumberField(TEXT("pos_y"), PosY);

	const TSharedPtr<FJsonObject>* ParamsObj = nullptr;
	Args->TryGetObjectField(TEXT("params"), ParamsObj);

	FString LoadError;
	UBlueprint* Blueprint = FUnrealGPTBlueprintGraph::LoadBlueprint(AssetPath, LoadError);
	if (!Blueprint)
	{
		return SerializeJson(FUnrealGPTBlueprintGraph::MakeError(LoadError));
	}

	FString GraphError;
	UEdGraph* Graph = FUnrealGPTBlueprintGraph::ResolveGraph(Blueprint, GraphName, GraphError);
	if (!Graph)
	{
		return ReturnErrorJson(GraphError);
	}

	const bool bTransaction = BeginWriteTransaction(TEXT("Add Blueprint Node"));
	FString NodeGuid;
	FString NodeError;
	const bool bSuccess = FUnrealGPTBlueprintGraph::AddNode(
		Blueprint,
		Graph,
		NodeType,
		FVector2D(static_cast<float>(PosX), static_cast<float>(PosY)),
		ParamsObj ? *ParamsObj : nullptr,
		NodeGuid,
		NodeError);
	if (bTransaction)
	{
		EndWriteTransaction();
	}

	if (!bSuccess)
	{
		return ReturnErrorJson(NodeError);
	}

	TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
	Details->SetStringField(TEXT("asset_path"), AssetPath);
	Details->SetStringField(TEXT("graph_name"), Graph->GetName());
	Details->SetStringField(TEXT("node_guid"), NodeGuid);
	Details->SetStringField(TEXT("node_type"), NodeType);
	return MakeOk(TEXT("Blueprint node added"), Details);
}

FString UUnrealGPTBlueprintContext::ConnectPins(const FString& ArgumentsJson)
{
	using namespace UnrealGPTBlueprintContextPrivate;

	FString ParseError;
	const TSharedPtr<FJsonObject> Args = ParseArgs(ArgumentsJson, ParseError);
	if (!Args.IsValid())
	{
		return ReturnErrorJson(ParseError);
	}

	const FString AssetPath = GetRequiredAssetPath(Args, ParseError);
	if (AssetPath.IsEmpty())
	{
		return ReturnErrorJson(ParseError);
	}

	FString GraphName;
	Args->TryGetStringField(TEXT("graph_name"), GraphName);

	FString FromNodeGuid;
	FString FromPin;
	FString ToNodeGuid;
	FString ToPin;
	if (!Args->TryGetStringField(TEXT("from_node_guid"), FromNodeGuid) || FromNodeGuid.IsEmpty()
		|| !Args->TryGetStringField(TEXT("from_pin"), FromPin) || FromPin.IsEmpty()
		|| !Args->TryGetStringField(TEXT("to_node_guid"), ToNodeGuid) || ToNodeGuid.IsEmpty()
		|| !Args->TryGetStringField(TEXT("to_pin"), ToPin) || ToPin.IsEmpty())
	{
		return ReturnErrorJson(TEXT("Missing required pin connection fields: from_node_guid, from_pin, to_node_guid, to_pin"));
	}

	FString LoadError;
	UBlueprint* Blueprint = FUnrealGPTBlueprintGraph::LoadBlueprint(AssetPath, LoadError);
	if (!Blueprint)
	{
		return SerializeJson(FUnrealGPTBlueprintGraph::MakeError(LoadError));
	}

	FString GraphError;
	UEdGraph* Graph = FUnrealGPTBlueprintGraph::ResolveGraph(Blueprint, GraphName, GraphError);
	if (!Graph)
	{
		return ReturnErrorJson(GraphError);
	}

	const bool bTransaction = BeginWriteTransaction(TEXT("Connect Blueprint Pins"));
	FString ConnectError;
	const bool bSuccess = FUnrealGPTBlueprintGraph::ConnectPins(
		Graph, FromNodeGuid, FromPin, ToNodeGuid, ToPin, ConnectError);
	if (bSuccess)
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	}
	if (bTransaction)
	{
		EndWriteTransaction();
	}

	if (!bSuccess)
	{
		return ReturnErrorJson(ConnectError);
	}

	TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
	Details->SetStringField(TEXT("asset_path"), AssetPath);
	Details->SetStringField(TEXT("graph_name"), Graph->GetName());
	Details->SetStringField(TEXT("from_node_guid"), FromNodeGuid);
	Details->SetStringField(TEXT("from_pin"), FromPin);
	Details->SetStringField(TEXT("to_node_guid"), ToNodeGuid);
	Details->SetStringField(TEXT("to_pin"), ToPin);
	return MakeOk(TEXT("Blueprint pins connected"), Details);
}

FString UUnrealGPTBlueprintContext::RemoveNode(const FString& ArgumentsJson)
{
	using namespace UnrealGPTBlueprintContextPrivate;

	FString ParseError;
	const TSharedPtr<FJsonObject> Args = ParseArgs(ArgumentsJson, ParseError);
	if (!Args.IsValid())
	{
		return ReturnErrorJson(ParseError);
	}

	const FString AssetPath = GetRequiredAssetPath(Args, ParseError);
	if (AssetPath.IsEmpty())
	{
		return ReturnErrorJson(ParseError);
	}

	FString NodeGuid;
	if (!Args->TryGetStringField(TEXT("node_guid"), NodeGuid) || NodeGuid.IsEmpty())
	{
		return ReturnErrorJson(TEXT("Missing required field: node_guid"));
	}

	FString GraphName;
	Args->TryGetStringField(TEXT("graph_name"), GraphName);

	FString LoadError;
	UBlueprint* Blueprint = FUnrealGPTBlueprintGraph::LoadBlueprint(AssetPath, LoadError);
	if (!Blueprint)
	{
		return SerializeJson(FUnrealGPTBlueprintGraph::MakeError(LoadError));
	}

	FString GraphError;
	UEdGraph* Graph = FUnrealGPTBlueprintGraph::ResolveGraph(Blueprint, GraphName, GraphError);
	if (!Graph)
	{
		return ReturnErrorJson(GraphError);
	}

	const bool bTransaction = BeginWriteTransaction(TEXT("Remove Blueprint Node"));
	FString RemoveError;
	const bool bSuccess = FUnrealGPTBlueprintGraph::RemoveNode(Blueprint, Graph, NodeGuid, RemoveError);
	if (bTransaction)
	{
		EndWriteTransaction();
	}

	if (!bSuccess)
	{
		return ReturnErrorJson(RemoveError);
	}

	TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
	Details->SetStringField(TEXT("asset_path"), AssetPath);
	Details->SetStringField(TEXT("graph_name"), Graph->GetName());
	Details->SetStringField(TEXT("node_guid"), NodeGuid);
	return MakeOk(TEXT("Blueprint node removed"), Details);
}

FString UUnrealGPTBlueprintContext::SetPinDefault(const FString& ArgumentsJson)
{
	using namespace UnrealGPTBlueprintContextPrivate;

	FString ParseError;
	const TSharedPtr<FJsonObject> Args = ParseArgs(ArgumentsJson, ParseError);
	if (!Args.IsValid())
	{
		return ReturnErrorJson(ParseError);
	}

	const FString AssetPath = GetRequiredAssetPath(Args, ParseError);
	if (AssetPath.IsEmpty())
	{
		return ReturnErrorJson(ParseError);
	}

	FString NodeGuid;
	FString PinName;
	FString Value;
	if (!Args->TryGetStringField(TEXT("node_guid"), NodeGuid) || NodeGuid.IsEmpty()
		|| !Args->TryGetStringField(TEXT("pin_name"), PinName) || PinName.IsEmpty()
		|| !Args->TryGetStringField(TEXT("value"), Value))
	{
		return ReturnErrorJson(TEXT("Missing required fields: node_guid, pin_name, value"));
	}

	FString GraphName;
	Args->TryGetStringField(TEXT("graph_name"), GraphName);

	FString LoadError;
	UBlueprint* Blueprint = FUnrealGPTBlueprintGraph::LoadBlueprint(AssetPath, LoadError);
	if (!Blueprint)
	{
		return SerializeJson(FUnrealGPTBlueprintGraph::MakeError(LoadError));
	}

	FString GraphError;
	UEdGraph* Graph = FUnrealGPTBlueprintGraph::ResolveGraph(Blueprint, GraphName, GraphError);
	if (!Graph)
	{
		return ReturnErrorJson(GraphError);
	}

	const bool bTransaction = BeginWriteTransaction(TEXT("Set Blueprint Pin Default"));
	FString PinError;
	const bool bSuccess = FUnrealGPTBlueprintGraph::SetPinDefault(Graph, NodeGuid, PinName, Value, PinError);
	if (bSuccess)
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	}
	if (bTransaction)
	{
		EndWriteTransaction();
	}

	if (!bSuccess)
	{
		return ReturnErrorJson(PinError);
	}

	TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
	Details->SetStringField(TEXT("asset_path"), AssetPath);
	Details->SetStringField(TEXT("graph_name"), Graph->GetName());
	Details->SetStringField(TEXT("node_guid"), NodeGuid);
	Details->SetStringField(TEXT("pin_name"), PinName);
	Details->SetStringField(TEXT("value"), Value);
	return MakeOk(TEXT("Blueprint pin default set"), Details);
}
