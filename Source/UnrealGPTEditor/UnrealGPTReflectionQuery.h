// Copyright (c) 2025 TREE Industries.

#pragma once

#include "CoreMinimal.h"

/**
 * Reflection introspection for the reflection_query agent tool.
 * Produces compact, filterable UClass schemas for Python/Blueprint scripting.
 */
class UNREALGPTEDITOR_API FUnrealGPTReflectionQuery
{
public:
	/** Execute reflection_query from a JSON arguments object. */
	static FString Query(const FString& ArgumentsJson);
};
