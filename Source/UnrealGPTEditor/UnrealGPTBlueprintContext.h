// Copyright (c) 2025 TREE Industries.

#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "UnrealGPTBlueprintContext.generated.h"

UCLASS()
class UNREALGPTEDITOR_API UUnrealGPTBlueprintContext : public UObject
{
	GENERATED_BODY()

public:
	static FString Query(const FString& ArgumentsJson);
	static FString Create(const FString& ArgumentsJson);
	static FString AddVariable(const FString& ArgumentsJson);
	static FString Compile(const FString& ArgumentsJson);
	static FString AddNode(const FString& ArgumentsJson);
	static FString ConnectPins(const FString& ArgumentsJson);
	static FString RemoveNode(const FString& ArgumentsJson);
	static FString SetPinDefault(const FString& ArgumentsJson);
};
