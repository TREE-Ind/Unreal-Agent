// Copyright (c) 2025 TREE Industries.

#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraphPin.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;

/**
 * Internal helpers for blueprint asset loading, graph serialization, and K2 node manipulation.
 */
class UNREALGPTEDITOR_API FUnrealGPTBlueprintGraph
{
public:
	static FString SerializeJson(const TSharedPtr<FJsonObject>& Root);
	static TSharedPtr<FJsonObject> MakeError(const FString& Message, const TArray<FString>& Candidates = {});

	static FString NormalizeAssetPath(const FString& InPath);
	static UBlueprint* LoadBlueprint(const FString& AssetPath, FString& OutError);
	static UClass* ResolveParentClass(const FString& ParentClassName, FString& OutError);

	static UEdGraph* ResolveGraph(UBlueprint* Blueprint, const FString& GraphName, FString& OutError);
	static UEdGraphNode* FindNodeByGuid(UEdGraph* Graph, const FString& NodeGuidStr, FString& OutError);

	static TSharedPtr<FJsonObject> SerializeBlueprintSummary(
		UBlueprint* Blueprint,
		const FString& AssetPath,
		const FString& GraphNameFilter,
		const FString& NodeGuidFilter,
		bool bIncludePins);

	static bool ParsePinType(const FString& TypeName, const FString& SubTypeObjectPath, FEdGraphPinType& OutPinType, FString& OutError);
	static FString PinTypeToString(const FEdGraphPinType& PinType);

	static bool AddNode(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		const FString& NodeType,
		const FVector2D& Position,
		const TSharedPtr<FJsonObject>& Params,
		FString& OutNodeGuid,
		FString& OutError);

	static bool ConnectPins(
		UEdGraph* Graph,
		const FString& FromNodeGuid,
		const FString& FromPinName,
		const FString& ToNodeGuid,
		const FString& ToPinName,
		FString& OutError);

	static bool RemoveNode(UBlueprint* Blueprint, UEdGraph* Graph, const FString& NodeGuid, FString& OutError);

	static bool SetPinDefault(
		UEdGraph* Graph,
		const FString& NodeGuid,
		const FString& PinName,
		const FString& Value,
		FString& OutError);

	static bool AddMemberVariable(
		UBlueprint* Blueprint,
		const FString& VarName,
		const FEdGraphPinType& PinType,
		const FString& DefaultValue,
		bool bInstanceEditable,
		bool bExposeOnSpawn,
		FString& OutError);

	static bool CompileBlueprint(UBlueprint* Blueprint, TArray<FString>& OutErrors, TArray<FString>& OutWarnings, FString& OutStatus);

	static const TArray<FString>& GetSupportedNodeTypes();
};
