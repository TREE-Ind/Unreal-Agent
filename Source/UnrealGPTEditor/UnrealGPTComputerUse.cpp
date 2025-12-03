#include "UnrealGPTComputerUse.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformFilemanager.h"
#include "Misc/Paths.h"

FString UUnrealGPTComputerUse::ExecuteAction(const FString& ActionJson)
{
	TSharedPtr<FJsonObject> ActionObj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ActionJson);
	
	if (!FJsonSerializer::Deserialize(Reader, ActionObj) || !ActionObj.IsValid())
	{
		return TEXT("Error: Invalid JSON action format");
	}

	FString ActionType;
	if (!ActionObj->TryGetStringField(TEXT("type"), ActionType))
	{
		return TEXT("Error: Missing 'type' field in action");
	}

	if (ActionType == TEXT("file_operation"))
	{
		return HandleFileOperation(ActionObj);
	}
	else if (ActionType == TEXT("widget_interaction"))
	{
		return HandleWidgetInteraction(ActionObj);
	}
	else if (ActionType == TEXT("os_command"))
	{
		return HandleOSCommand(ActionObj);
	}
	else
	{
		return FString::Printf(TEXT("Error: Unknown action type '%s'"), *ActionType);
	}
}

FString UUnrealGPTComputerUse::HandleFileOperation(const TSharedPtr<FJsonObject>& ActionObj)
{
	FString Operation;
	if (!ActionObj->TryGetStringField(TEXT("operation"), Operation))
	{
		return TEXT("Error: Missing 'operation' field for file_operation");
	}

	FString FilePath;
	if (!ActionObj->TryGetStringField(TEXT("path"), FilePath))
	{
		return TEXT("Error: Missing 'path' field for file_operation");
	}

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

	if (Operation == TEXT("read"))
	{
		FString FileContents;
		if (FFileHelper::LoadFileToString(FileContents, *FilePath))
		{
			return FString::Printf(TEXT("Success: Read %d bytes from %s"), FileContents.Len(), *FilePath);
		}
		else
		{
			return FString::Printf(TEXT("Error: Failed to read file %s"), *FilePath);
		}
	}
	else if (Operation == TEXT("write"))
	{
		FString Content;
		if (!ActionObj->TryGetStringField(TEXT("content"), Content))
		{
			return TEXT("Error: Missing 'content' field for write operation");
		}

		if (FFileHelper::SaveStringToFile(Content, *FilePath))
		{
			return FString::Printf(TEXT("Success: Wrote %d bytes to %s"), Content.Len(), *FilePath);
		}
		else
		{
			return FString::Printf(TEXT("Error: Failed to write file %s"), *FilePath);
		}
	}
	else if (Operation == TEXT("delete"))
	{
		if (PlatformFile.DeleteFile(*FilePath))
		{
			return FString::Printf(TEXT("Success: Deleted %s"), *FilePath);
		}
		else
		{
			return FString::Printf(TEXT("Error: Failed to delete %s"), *FilePath);
		}
	}
	else if (Operation == TEXT("exists"))
	{
		bool bExists = PlatformFile.FileExists(*FilePath);
		return FString::Printf(TEXT("File exists: %s"), bExists ? TEXT("true") : TEXT("false"));
	}
	else
	{
		return FString::Printf(TEXT("Error: Unknown file operation '%s'"), *Operation);
	}
}

FString UUnrealGPTComputerUse::HandleWidgetInteraction(const TSharedPtr<FJsonObject>& ActionObj)
{
	// Widget interaction would require more complex implementation
	// For now, return placeholder
	return TEXT("Widget interaction not yet implemented");
}

FString UUnrealGPTComputerUse::HandleOSCommand(const TSharedPtr<FJsonObject>& ActionObj)
{
	FString Command;
	if (!ActionObj->TryGetStringField(TEXT("command"), Command))
	{
		return TEXT("Error: Missing 'command' field for os_command");
	}

	// Security: Only allow safe commands
	// In production, this should have a whitelist of allowed commands
	FString LowerCommand = Command.ToLower();
	if (LowerCommand.Contains(TEXT("rm ")) || LowerCommand.Contains(TEXT("del ")) || 
		LowerCommand.Contains(TEXT("format")) || LowerCommand.Contains(TEXT("shutdown")))
	{
		return TEXT("Error: Command not allowed for security reasons");
	}

	// Execute command (platform-specific)
	FString Output;
	int32 ReturnCode = 0;
	
	// Note: In production, use proper process execution with timeouts
	// For now, return placeholder
	return FString::Printf(TEXT("OS command execution not fully implemented. Command: %s"), *Command);
}

