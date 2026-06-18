// Copyright (c) 2025 TREE Industries.

#include "UnrealGPTBlueprintGraph.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_Self.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/SavePackage.h"

namespace UnrealGPTBlueprintGraphPrivate
{
	static FString PinDirectionToString(const EEdGraphPinDirection Direction)
	{
		switch (Direction)
		{
		case EGPD_Input: return TEXT("input");
		case EGPD_Output: return TEXT("output");
		default: return TEXT("unknown");
		}
	}

	static FString BlueprintStatusToString(const EBlueprintStatus Status)
	{
		switch (Status)
		{
		case BS_Unknown: return TEXT("unknown");
		case BS_Dirty: return TEXT("dirty");
		case BS_Error: return TEXT("error");
		case BS_UpToDate: return TEXT("up_to_date");
		case BS_UpToDateWithWarnings: return TEXT("up_to_date_with_warnings");
		case BS_BeingCreated: return TEXT("being_created");
		default: return TEXT("unknown");
		}
	}

	static TSharedPtr<FJsonObject> SerializePin(const UEdGraphPin* Pin, bool bIncludeLinks)
	{
		TSharedPtr<FJsonObject> PinJson = MakeShared<FJsonObject>();
		PinJson->SetStringField(TEXT("name"), Pin->PinName.ToString());
		PinJson->SetStringField(TEXT("direction"), PinDirectionToString(Pin->Direction));
		PinJson->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
		if (!Pin->PinType.PinSubCategory.IsNone())
		{
			PinJson->SetStringField(TEXT("sub_category"), Pin->PinType.PinSubCategory.ToString());
		}
		if (Pin->PinType.PinSubCategoryObject.IsValid())
		{
			PinJson->SetStringField(TEXT("sub_category_object"), Pin->PinType.PinSubCategoryObject->GetPathName());
		}
		if (!Pin->DefaultValue.IsEmpty())
		{
			PinJson->SetStringField(TEXT("default_value"), Pin->DefaultValue);
		}
		PinJson->SetBoolField(TEXT("is_connected"), Pin->LinkedTo.Num() > 0);

		if (bIncludeLinks)
		{
			TArray<TSharedPtr<FJsonValue>> Links;
			for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				if (!LinkedPin || !LinkedPin->GetOwningNode())
				{
					continue;
				}
				TSharedPtr<FJsonObject> LinkJson = MakeShared<FJsonObject>();
				LinkJson->SetStringField(TEXT("node_guid"), LinkedPin->GetOwningNode()->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensInBraces));
				LinkJson->SetStringField(TEXT("pin_name"), LinkedPin->PinName.ToString());
				Links.Add(MakeShared<FJsonValueObject>(LinkJson));
			}
			if (Links.Num() > 0)
			{
				PinJson->SetArrayField(TEXT("linked_to"), Links);
			}
		}

		return PinJson;
	}

	static TSharedPtr<FJsonObject> SerializeNode(UEdGraphNode* Node, bool bIncludePins)
	{
		TSharedPtr<FJsonObject> NodeJson = MakeShared<FJsonObject>();
		NodeJson->SetStringField(TEXT("guid"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensInBraces));
		NodeJson->SetStringField(TEXT("class"), Node->GetClass()->GetName());
		NodeJson->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
		NodeJson->SetNumberField(TEXT("pos_x"), Node->NodePosX);
		NodeJson->SetNumberField(TEXT("pos_y"), Node->NodePosY);

		if (bIncludePins)
		{
			TArray<TSharedPtr<FJsonValue>> Pins;
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin)
				{
					continue;
				}
				Pins.Add(MakeShared<FJsonValueObject>(SerializePin(Pin, true)));
			}
			NodeJson->SetArrayField(TEXT("pins"), Pins);
		}

		return NodeJson;
	}

	static void CollectGraphs(UBlueprint* Blueprint, TArray<UEdGraph*>& OutGraphs)
	{
		OutGraphs.Reset();
		for (UEdGraph* Graph : Blueprint->UbergraphPages)
		{
			if (Graph)
			{
				OutGraphs.Add(Graph);
			}
		}
		for (UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			if (Graph)
			{
				OutGraphs.Add(Graph);
			}
		}
		for (UEdGraph* Graph : Blueprint->MacroGraphs)
		{
			if (Graph)
			{
				OutGraphs.Add(Graph);
			}
		}
	}

	static UFunction* ResolveEventFunction(const FString& EventName, UClass*& OutOwnerClass, FString& OutError)
	{
		const FName EventFName(*EventName);
		if (EventFName == TEXT("ReceiveBeginPlay") || EventFName == TEXT("BeginPlay"))
		{
			OutOwnerClass = AActor::StaticClass();
			return AActor::StaticClass()->FindFunctionByName(TEXT("ReceiveBeginPlay"));
		}
		if (EventFName == TEXT("ReceiveTick") || EventFName == TEXT("Tick"))
		{
			OutOwnerClass = AActor::StaticClass();
			return AActor::StaticClass()->FindFunctionByName(TEXT("ReceiveTick"));
		}
		if (EventFName == TEXT("ReceiveEndPlay") || EventFName == TEXT("EndPlay"))
		{
			OutOwnerClass = AActor::StaticClass();
			return AActor::StaticClass()->FindFunctionByName(TEXT("ReceiveEndPlay"));
		}

		OutError = FString::Printf(TEXT("Unsupported event name '%s'. Supported: ReceiveBeginPlay, ReceiveTick, ReceiveEndPlay."), *EventName);
		return nullptr;
	}

	static UFunction* ResolveCallFunction(const FString& FunctionName, const FString& TargetClassPath, FString& OutError)
	{
		UClass* TargetClass = nullptr;
		if (TargetClassPath.IsEmpty())
		{
			TargetClass = UKismetSystemLibrary::StaticClass();
		}
		else
		{
			TargetClass = LoadObject<UClass>(nullptr, *TargetClassPath);
			if (!TargetClass)
			{
				TargetClass = FindObject<UClass>(nullptr, *TargetClassPath);
			}
		}

		if (!TargetClass)
		{
			OutError = FString::Printf(TEXT("Could not resolve target_class '%s'"), *TargetClassPath);
			return nullptr;
		}

		UFunction* Function = TargetClass->FindFunctionByName(FName(*FunctionName));
		if (!Function)
		{
			OutError = FString::Printf(TEXT("Function '%s' not found on class '%s'"), *FunctionName, *TargetClass->GetPathName());
		}
		return Function;
	}
}

FString FUnrealGPTBlueprintGraph::SerializeJson(const TSharedPtr<FJsonObject>& Root)
{
	FString OutJson;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutJson);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
	return OutJson;
}

TSharedPtr<FJsonObject> FUnrealGPTBlueprintGraph::MakeError(const FString& Message, const TArray<FString>& Candidates)
{
	TSharedPtr<FJsonObject> ErrorObj = MakeShared<FJsonObject>();
	ErrorObj->SetStringField(TEXT("status"), TEXT("error"));
	ErrorObj->SetStringField(TEXT("message"), Message);
	if (Candidates.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> CandidateValues;
		for (const FString& Candidate : Candidates)
		{
			CandidateValues.Add(MakeShared<FJsonValueString>(Candidate));
		}
		ErrorObj->SetArrayField(TEXT("candidates"), CandidateValues);
	}
	return ErrorObj;
}

FString FUnrealGPTBlueprintGraph::NormalizeAssetPath(const FString& InPath)
{
	FString Path = InPath.TrimStartAndEnd();
	if (Path.IsEmpty())
	{
		return Path;
	}

	if (!Path.StartsWith(TEXT("/")))
	{
		Path = TEXT("/Game/") + Path;
	}

	if (!Path.Contains(TEXT(".")))
	{
		const FString AssetName = FPaths::GetBaseFilename(Path);
		Path = FString::Printf(TEXT("%s.%s"), *Path, *AssetName);
	}

	return Path;
}

UBlueprint* FUnrealGPTBlueprintGraph::LoadBlueprint(const FString& AssetPath, FString& OutError)
{
	const FString NormalizedPath = NormalizeAssetPath(AssetPath);
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *NormalizedPath);
	if (!Blueprint)
	{
		OutError = FString::Printf(TEXT("Blueprint not found at '%s'"), *NormalizedPath);
	}
	return Blueprint;
}

UClass* FUnrealGPTBlueprintGraph::ResolveParentClass(const FString& ParentClassName, FString& OutError)
{
	if (ParentClassName.IsEmpty())
	{
		OutError = TEXT("parent_class is required");
		return nullptr;
	}

	UClass* ParentClass = LoadObject<UClass>(nullptr, *ParentClassName);
	if (!ParentClass)
	{
		ParentClass = FindObject<UClass>(nullptr, *ParentClassName);
	}
	if (!ParentClass)
	{
		ParentClass = FindFirstObject<UClass>(*ParentClassName, EFindFirstObjectOptions::ExactClass);
	}
	if (!ParentClass)
	{
		ParentClass = FindFirstObject<UClass>(*(TEXT("/Script/Engine.") + ParentClassName), EFindFirstObjectOptions::ExactClass);
	}
	if (!ParentClass)
	{
		OutError = FString::Printf(TEXT("Could not resolve parent_class '%s'"), *ParentClassName);
	}
	return ParentClass;
}

UEdGraph* FUnrealGPTBlueprintGraph::ResolveGraph(UBlueprint* Blueprint, const FString& GraphName, FString& OutError)
{
	if (!Blueprint)
	{
		OutError = TEXT("Blueprint is null");
		return nullptr;
	}

	if (GraphName.IsEmpty())
	{
		if (Blueprint->UbergraphPages.Num() > 0)
		{
			return Blueprint->UbergraphPages[0];
		}
		OutError = TEXT("Blueprint has no event graph");
		return nullptr;
	}

	const FName GraphFName(*GraphName);
	TArray<UEdGraph*> Graphs;
	UnrealGPTBlueprintGraphPrivate::CollectGraphs(Blueprint, Graphs);
	for (UEdGraph* Graph : Graphs)
	{
		if (Graph && Graph->GetFName() == GraphFName)
		{
			return Graph;
		}
	}

	OutError = FString::Printf(TEXT("Graph '%s' not found on blueprint"), *GraphName);
	return nullptr;
}

UEdGraphNode* FUnrealGPTBlueprintGraph::FindNodeByGuid(UEdGraph* Graph, const FString& NodeGuidStr, FString& OutError)
{
	if (!Graph)
	{
		OutError = TEXT("Graph is null");
		return nullptr;
	}

	FGuid NodeGuid;
	if (!FGuid::Parse(NodeGuidStr, NodeGuid))
	{
		OutError = FString::Printf(TEXT("Invalid node_guid '%s'"), *NodeGuidStr);
		return nullptr;
	}

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node && Node->NodeGuid == NodeGuid)
		{
			return Node;
		}
	}

	OutError = FString::Printf(TEXT("Node with guid '%s' not found in graph"), *NodeGuidStr);
	return nullptr;
}

TSharedPtr<FJsonObject> FUnrealGPTBlueprintGraph::SerializeBlueprintSummary(
	UBlueprint* Blueprint,
	const FString& AssetPath,
	const FString& GraphNameFilter,
	const FString& NodeGuidFilter,
	bool bIncludePins)
{
	using namespace UnrealGPTBlueprintGraphPrivate;

	TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
	Details->SetStringField(TEXT("asset_path"), AssetPath);
	Details->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
	if (Blueprint->ParentClass)
	{
		Details->SetStringField(TEXT("parent_class"), Blueprint->ParentClass->GetPathName());
	}
	Details->SetStringField(TEXT("compile_status"), BlueprintStatusToString(Blueprint->Status));

	TArray<TSharedPtr<FJsonValue>> Variables;
	for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
	{
		TSharedPtr<FJsonObject> VarJson = MakeShared<FJsonObject>();
		VarJson->SetStringField(TEXT("name"), Variable.VarName.ToString());
		VarJson->SetStringField(TEXT("type"), PinTypeToString(Variable.VarType));
		VarJson->SetStringField(TEXT("default_value"), Variable.DefaultValue);
		VarJson->SetBoolField(TEXT("instance_editable"), (Variable.PropertyFlags & CPF_Edit) != 0);
		VarJson->SetBoolField(TEXT("expose_on_spawn"), (Variable.PropertyFlags & CPF_ExposeOnSpawn) != 0);
		Variables.Add(MakeShared<FJsonValueObject>(VarJson));
	}
	Details->SetArrayField(TEXT("variables"), Variables);

	TArray<TSharedPtr<FJsonValue>> Components;
	if (Blueprint->SimpleConstructionScript)
	{
		const TArray<USCS_Node*>& AllNodes = Blueprint->SimpleConstructionScript->GetAllNodes();
		for (USCS_Node* Node : AllNodes)
		{
			if (!Node || !Node->ComponentClass)
			{
				continue;
			}
			TSharedPtr<FJsonObject> CompJson = MakeShared<FJsonObject>();
			CompJson->SetStringField(TEXT("name"), Node->GetVariableName().ToString());
			CompJson->SetStringField(TEXT("class"), Node->ComponentClass->GetPathName());
			Components.Add(MakeShared<FJsonValueObject>(CompJson));
		}
	}
	Details->SetArrayField(TEXT("components"), Components);

	TArray<UEdGraph*> Graphs;
	UnrealGPTBlueprintGraphPrivate::CollectGraphs(Blueprint, Graphs);

	TArray<TSharedPtr<FJsonValue>> GraphSummaries;
	TArray<TSharedPtr<FJsonValue>> NodeSummaries;

	for (UEdGraph* Graph : Graphs)
	{
		if (!Graph)
		{
			continue;
		}

		if (!GraphNameFilter.IsEmpty() && !Graph->GetName().Equals(GraphNameFilter, ESearchCase::IgnoreCase))
		{
			continue;
		}

		TSharedPtr<FJsonObject> GraphJson = MakeShared<FJsonObject>();
		GraphJson->SetStringField(TEXT("name"), Graph->GetName());
		GraphJson->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
		GraphSummaries.Add(MakeShared<FJsonValueObject>(GraphJson));

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}

			if (!NodeGuidFilter.IsEmpty() && Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensInBraces) != NodeGuidFilter)
			{
				continue;
			}

			TSharedPtr<FJsonObject> NodeJson = SerializeNode(Node, bIncludePins);
			NodeJson->SetStringField(TEXT("graph_name"), Graph->GetName());
			NodeSummaries.Add(MakeShared<FJsonValueObject>(NodeJson));
		}
	}

	Details->SetArrayField(TEXT("graphs"), GraphSummaries);
	Details->SetArrayField(TEXT("nodes"), NodeSummaries);
	return Details;
}

bool FUnrealGPTBlueprintGraph::ParsePinType(const FString& TypeName, const FString& SubTypeObjectPath, FEdGraphPinType& OutPinType, FString& OutError)
{
	const FName TypeFName(*TypeName);
	if (TypeFName == TEXT("bool"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
		return true;
	}
	if (TypeFName == TEXT("byte"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
		return true;
	}
	if (TypeFName == TEXT("int") || TypeFName == TEXT("int32"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int;
		return true;
	}
	if (TypeFName == TEXT("int64"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int64;
		return true;
	}
	if (TypeFName == TEXT("float") || TypeFName == TEXT("real"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Real;
		OutPinType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
		return true;
	}
	if (TypeFName == TEXT("double"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Real;
		OutPinType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
		return true;
	}
	if (TypeFName == TEXT("string"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_String;
		return true;
	}
	if (TypeFName == TEXT("name"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Name;
		return true;
	}
	if (TypeFName == TEXT("text"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Text;
		return true;
	}
	if (TypeFName == TEXT("vector"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		OutPinType.PinSubCategoryObject = TBaseStructure<FVector>::Get();
		return true;
	}
	if (TypeFName == TEXT("rotator"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		OutPinType.PinSubCategoryObject = TBaseStructure<FRotator>::Get();
		return true;
	}
	if (TypeFName == TEXT("transform"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		OutPinType.PinSubCategoryObject = TBaseStructure<FTransform>::Get();
		return true;
	}
	if (TypeFName == TEXT("object") || TypeFName == TEXT("class"))
	{
		if (SubTypeObjectPath.IsEmpty())
		{
			OutError = TEXT("object/class pin types require sub_type_object (e.g. /Script/Engine.Actor)");
			return false;
		}
		UObject* SubType = LoadObject<UObject>(nullptr, *SubTypeObjectPath);
		if (!SubType)
		{
			SubType = FindObject<UObject>(nullptr, *SubTypeObjectPath);
		}
		if (!SubType)
		{
			OutError = FString::Printf(TEXT("Could not resolve sub_type_object '%s'"), *SubTypeObjectPath);
			return false;
		}
		OutPinType.PinCategory = (TypeFName == TEXT("class")) ? UEdGraphSchema_K2::PC_Class : UEdGraphSchema_K2::PC_Object;
		OutPinType.PinSubCategoryObject = SubType;
		return true;
	}

	OutError = FString::Printf(TEXT("Unsupported variable type '%s'"), *TypeName);
	return false;
}

FString FUnrealGPTBlueprintGraph::PinTypeToString(const FEdGraphPinType& PinType)
{
	if (PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean) return TEXT("bool");
	if (PinType.PinCategory == UEdGraphSchema_K2::PC_Byte) return TEXT("byte");
	if (PinType.PinCategory == UEdGraphSchema_K2::PC_Int) return TEXT("int");
	if (PinType.PinCategory == UEdGraphSchema_K2::PC_Int64) return TEXT("int64");
	if (PinType.PinCategory == UEdGraphSchema_K2::PC_Real)
	{
		return PinType.PinSubCategory == UEdGraphSchema_K2::PC_Double ? TEXT("double") : TEXT("float");
	}
	if (PinType.PinCategory == UEdGraphSchema_K2::PC_String) return TEXT("string");
	if (PinType.PinCategory == UEdGraphSchema_K2::PC_Name) return TEXT("name");
	if (PinType.PinCategory == UEdGraphSchema_K2::PC_Text) return TEXT("text");
	if (PinType.PinCategory == UEdGraphSchema_K2::PC_Struct && PinType.PinSubCategoryObject == TBaseStructure<FVector>::Get()) return TEXT("vector");
	if (PinType.PinCategory == UEdGraphSchema_K2::PC_Struct && PinType.PinSubCategoryObject == TBaseStructure<FRotator>::Get()) return TEXT("rotator");
	if (PinType.PinCategory == UEdGraphSchema_K2::PC_Struct && PinType.PinSubCategoryObject == TBaseStructure<FTransform>::Get()) return TEXT("transform");
	if (PinType.PinCategory == UEdGraphSchema_K2::PC_Object) return TEXT("object");
	if (PinType.PinCategory == UEdGraphSchema_K2::PC_Class) return TEXT("class");
	return PinType.PinCategory.ToString();
}

const TArray<FString>& FUnrealGPTBlueprintGraph::GetSupportedNodeTypes()
{
	static const TArray<FString> Supported = {
		TEXT("Event"),
		TEXT("CallFunction"),
		TEXT("VariableGet"),
		TEXT("VariableSet"),
		TEXT("CustomEvent"),
		TEXT("Branch"),
		TEXT("Sequence"),
		TEXT("Self"),
		TEXT("PrintString")
	};
	return Supported;
}

bool FUnrealGPTBlueprintGraph::AddNode(
	UBlueprint* Blueprint,
	UEdGraph* Graph,
	const FString& NodeType,
	const FVector2D& Position,
	const TSharedPtr<FJsonObject>& Params,
	FString& OutNodeGuid,
	FString& OutError)
{
	if (!Blueprint || !Graph)
	{
		OutError = TEXT("Blueprint or graph is null");
		return false;
	}

	const FString NormalizedType = NodeType.Equals(TEXT("PrintString"), ESearchCase::IgnoreCase) ? TEXT("CallFunction") : NodeType;
	if (!GetSupportedNodeTypes().ContainsByPredicate([&NormalizedType, &NodeType](const FString& Supported)
	{
		return Supported.Equals(NormalizedType, ESearchCase::IgnoreCase) || NodeType.Equals(Supported, ESearchCase::IgnoreCase);
	}))
	{
		OutError = FString::Printf(TEXT("Unsupported node_type '%s'. Supported: %s"), *NodeType, *FString::Join(GetSupportedNodeTypes(), TEXT(", ")));
		return false;
	}

	UEdGraphNode* NewNode = nullptr;

	if (NormalizedType.Equals(TEXT("Event"), ESearchCase::IgnoreCase))
	{
		FString EventName = TEXT("ReceiveBeginPlay");
		if (Params.IsValid())
		{
			Params->TryGetStringField(TEXT("event_name"), EventName);
		}

		UClass* EventOwnerClass = nullptr;
		UFunction* EventFunction = UnrealGPTBlueprintGraphPrivate::ResolveEventFunction(EventName, EventOwnerClass, OutError);
		if (!EventFunction)
		{
			return false;
		}

		FGraphNodeCreator<UK2Node_Event> Creator(*Graph);
		UK2Node_Event* EventNode = Creator.CreateNode();
		EventNode->EventReference.SetExternalMember(EventFunction->GetFName(), EventOwnerClass);
		Creator.Finalize();
		EventNode->AllocateDefaultPins();
		NewNode = EventNode;
	}
	else if (NormalizedType.Equals(TEXT("CallFunction"), ESearchCase::IgnoreCase) || NodeType.Equals(TEXT("PrintString"), ESearchCase::IgnoreCase))
	{
		FString FunctionName = TEXT("PrintString");
		FString TargetClassPath = TEXT("/Script/Engine.KismetSystemLibrary");
		if (NodeType.Equals(TEXT("PrintString"), ESearchCase::IgnoreCase))
		{
			FunctionName = TEXT("PrintString");
			TargetClassPath = TEXT("/Script/Engine.KismetSystemLibrary");
		}
		else if (Params.IsValid())
		{
			Params->TryGetStringField(TEXT("function_name"), FunctionName);
			Params->TryGetStringField(TEXT("target_class"), TargetClassPath);
		}

		UFunction* Function = UnrealGPTBlueprintGraphPrivate::ResolveCallFunction(FunctionName, TargetClassPath, OutError);
		if (!Function)
		{
			return false;
		}

		FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
		UK2Node_CallFunction* CallNode = Creator.CreateNode();
		CallNode->SetFromFunction(Function);
		Creator.Finalize();
		CallNode->AllocateDefaultPins();
		NewNode = CallNode;
	}
	else if (NormalizedType.Equals(TEXT("VariableGet"), ESearchCase::IgnoreCase))
	{
		FString VariableName;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("variable_name"), VariableName) || VariableName.IsEmpty())
		{
			OutError = TEXT("VariableGet requires variable_name");
			return false;
		}

		FGraphNodeCreator<UK2Node_VariableGet> Creator(*Graph);
		UK2Node_VariableGet* VarNode = Creator.CreateNode();
		VarNode->VariableReference.SetSelfMember(FName(*VariableName));
		Creator.Finalize();
		VarNode->AllocateDefaultPins();
		NewNode = VarNode;
	}
	else if (NormalizedType.Equals(TEXT("VariableSet"), ESearchCase::IgnoreCase))
	{
		FString VariableName;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("variable_name"), VariableName) || VariableName.IsEmpty())
		{
			OutError = TEXT("VariableSet requires variable_name");
			return false;
		}

		FGraphNodeCreator<UK2Node_VariableSet> Creator(*Graph);
		UK2Node_VariableSet* VarNode = Creator.CreateNode();
		VarNode->VariableReference.SetSelfMember(FName(*VariableName));
		Creator.Finalize();
		VarNode->AllocateDefaultPins();
		NewNode = VarNode;
	}
	else if (NormalizedType.Equals(TEXT("CustomEvent"), ESearchCase::IgnoreCase))
	{
		FString EventName = TEXT("CustomEvent");
		if (Params.IsValid())
		{
			Params->TryGetStringField(TEXT("event_name"), EventName);
		}

		FGraphNodeCreator<UK2Node_CustomEvent> Creator(*Graph);
		UK2Node_CustomEvent* CustomEventNode = Creator.CreateNode();
		CustomEventNode->CustomFunctionName = FName(*EventName);
		Creator.Finalize();
		CustomEventNode->AllocateDefaultPins();
		NewNode = CustomEventNode;
	}
	else if (NormalizedType.Equals(TEXT("Branch"), ESearchCase::IgnoreCase))
	{
		FGraphNodeCreator<UK2Node_IfThenElse> Creator(*Graph);
		UK2Node_IfThenElse* BranchNode = Creator.CreateNode();
		Creator.Finalize();
		BranchNode->AllocateDefaultPins();
		NewNode = BranchNode;
	}
	else if (NormalizedType.Equals(TEXT("Sequence"), ESearchCase::IgnoreCase))
	{
		FGraphNodeCreator<UK2Node_ExecutionSequence> Creator(*Graph);
		UK2Node_ExecutionSequence* SequenceNode = Creator.CreateNode();
		Creator.Finalize();
		SequenceNode->AllocateDefaultPins();
		NewNode = SequenceNode;
	}
	else if (NormalizedType.Equals(TEXT("Self"), ESearchCase::IgnoreCase))
	{
		FGraphNodeCreator<UK2Node_Self> Creator(*Graph);
		UK2Node_Self* SelfNode = Creator.CreateNode();
		Creator.Finalize();
		SelfNode->AllocateDefaultPins();
		NewNode = SelfNode;
	}

	if (!NewNode)
	{
		OutError = FString::Printf(TEXT("Failed to create node_type '%s'"), *NodeType);
		return false;
	}

	NewNode->NodePosX = FMath::RoundToInt(Position.X);
	NewNode->NodePosY = FMath::RoundToInt(Position.Y);
	OutNodeGuid = NewNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensInBraces);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	return true;
}

bool FUnrealGPTBlueprintGraph::ConnectPins(
	UEdGraph* Graph,
	const FString& FromNodeGuid,
	const FString& FromPinName,
	const FString& ToNodeGuid,
	const FString& ToPinName,
	FString& OutError)
{
	UEdGraphNode* FromNode = FindNodeByGuid(Graph, FromNodeGuid, OutError);
	if (!FromNode)
	{
		return false;
	}

	UEdGraphNode* ToNode = FindNodeByGuid(Graph, ToNodeGuid, OutError);
	if (!ToNode)
	{
		return false;
	}

	UEdGraphPin* FromPin = FromNode->FindPin(FName(*FromPinName));
	UEdGraphPin* ToPin = ToNode->FindPin(FName(*ToPinName));
	if (!FromPin || !ToPin)
	{
		OutError = FString::Printf(TEXT("Could not find pins '%s' or '%s'"), *FromPinName, *ToPinName);
		return false;
	}

	const UEdGraphSchema_K2* Schema = Cast<const UEdGraphSchema_K2>(Graph->GetSchema());
	if (!Schema)
	{
		OutError = TEXT("Graph schema is not K2");
		return false;
	}

	if (!Schema->TryCreateConnection(FromPin, ToPin))
	{
		OutError = FString::Printf(TEXT("Failed to connect '%s.%s' -> '%s.%s'"), *FromNodeGuid, *FromPinName, *ToNodeGuid, *ToPinName);
		return false;
	}

	FromNode->NodeConnectionListChanged();
	ToNode->NodeConnectionListChanged();
	return true;
}

bool FUnrealGPTBlueprintGraph::RemoveNode(UBlueprint* Blueprint, UEdGraph* Graph, const FString& NodeGuid, FString& OutError)
{
	UEdGraphNode* Node = FindNodeByGuid(Graph, NodeGuid, OutError);
	if (!Node)
	{
		return false;
	}

	FBlueprintEditorUtils::RemoveNode(Blueprint, Node, true);
	return true;
}

bool FUnrealGPTBlueprintGraph::SetPinDefault(
	UEdGraph* Graph,
	const FString& NodeGuid,
	const FString& PinName,
	const FString& Value,
	FString& OutError)
{
	UEdGraphNode* Node = FindNodeByGuid(Graph, NodeGuid, OutError);
	if (!Node)
	{
		return false;
	}

	UEdGraphPin* Pin = Node->FindPin(FName(*PinName));
	if (!Pin)
	{
		OutError = FString::Printf(TEXT("Pin '%s' not found on node"), *PinName);
		return false;
	}

	const UEdGraphSchema_K2* Schema = Cast<const UEdGraphSchema_K2>(Graph->GetSchema());
	if (!Schema)
	{
		OutError = TEXT("Graph schema is not K2");
		return false;
	}

	Schema->TrySetDefaultValue(*Pin, Value);
	if (Pin->DefaultValue.IsEmpty())
	{
		Pin->DefaultValue = Value;
	}

	return true;
}

bool FUnrealGPTBlueprintGraph::AddMemberVariable(
	UBlueprint* Blueprint,
	const FString& VarName,
	const FEdGraphPinType& PinType,
	const FString& DefaultValue,
	bool bInstanceEditable,
	bool bExposeOnSpawn,
	FString& OutError)
{
	if (VarName.IsEmpty())
	{
		OutError = TEXT("Variable name is required");
		return false;
	}

	if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, FName(*VarName), PinType, DefaultValue))
	{
		OutError = FString::Printf(TEXT("Failed to add variable '%s'"), *VarName);
		return false;
	}

	for (FBPVariableDescription& Variable : Blueprint->NewVariables)
	{
		if (Variable.VarName == FName(*VarName))
		{
			if (bInstanceEditable)
			{
				Variable.PropertyFlags |= CPF_Edit;
			}
			if (bExposeOnSpawn)
			{
				Variable.PropertyFlags |= CPF_ExposeOnSpawn;
			}
			break;
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	return true;
}

bool FUnrealGPTBlueprintGraph::CompileBlueprint(UBlueprint* Blueprint, TArray<FString>& OutErrors, TArray<FString>& OutWarnings, FString& OutStatus)
{
	OutErrors.Reset();
	OutWarnings.Reset();

	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None);
	OutStatus = UnrealGPTBlueprintGraphPrivate::BlueprintStatusToString(Blueprint->Status);

	if (Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings)
	{
		return true;
	}

	OutErrors.Add(FString::Printf(TEXT("Blueprint compile status: %s"), *OutStatus));
	return Blueprint->Status != BS_Error;
}
