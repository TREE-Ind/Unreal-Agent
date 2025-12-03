#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "Dom/JsonObject.h"
#include "UnrealGPTComputerUse.generated.h"

UCLASS()
class UNREALGPTEDITOR_API UUnrealGPTComputerUse : public UObject
{
	GENERATED_BODY()

public:
	/** Execute a computer use action based on JSON description */
	static FString ExecuteAction(const FString& ActionJson);

private:
	/** Handle file operations */
	static FString HandleFileOperation(const TSharedPtr<FJsonObject>& ActionObj);

	/** Handle widget interactions */
	static FString HandleWidgetInteraction(const TSharedPtr<FJsonObject>& ActionObj);

	/** Handle OS commands */
	static FString HandleOSCommand(const TSharedPtr<FJsonObject>& ActionObj);
};

