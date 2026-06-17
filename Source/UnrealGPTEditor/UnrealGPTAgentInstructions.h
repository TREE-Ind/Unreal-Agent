// Copyright (c) 2025 TREE Industries.

#pragma once

#include "CoreMinimal.h"

/**
 * Centralized agent instructions for UnrealGPT.
 * Provides the system prompt that guides agent behavior.
 */
class UNREALGPTEDITOR_API UnrealGPTAgentInstructions
{
public:
	/**
	 * Get the full agent instructions with engine version interpolated.
	 * @param EngineVersion The target Unreal Engine version string (e.g., "5.6")
	 * @return The complete instructions string for the agent
	 */
	static FString GetInstructions(const FString& EngineVersion);
};

