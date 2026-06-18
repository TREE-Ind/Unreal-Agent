// Copyright (c) 2025 TREE Industries.

#include "UnrealGPTReflectionQuery.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"

namespace UnrealGPTReflectionQueryPrivate
{
	struct FOptions
	{
		FString ClassName;
		FString MemberContains;
		bool bIncludeSuper = false;
		bool bIncludeProperties = true;
		bool bIncludeFunctions = true;
		bool bScriptableOnly = true;
		bool bIncludeDeprecated = false;
		int32 MaxResults = 40;
	};

	static FString SerializeJson(const TSharedPtr<FJsonObject>& Root)
	{
		FString OutJson;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutJson);
		FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
		return OutJson;
	}

	static TSharedPtr<FJsonObject> MakeReflectionError(const FString& Message, const TArray<FString>& Candidates = {})
	{
		TSharedPtr<FJsonObject> ErrorObj = MakeShareable(new FJsonObject);
		ErrorObj->SetStringField(TEXT("status"), TEXT("error"));
		ErrorObj->SetStringField(TEXT("message"), Message);

		if (Candidates.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> CandidateValues;
			for (const FString& Candidate : Candidates)
			{
				CandidateValues.Add(MakeShareable(new FJsonValueString(Candidate)));
			}
			ErrorObj->SetArrayField(TEXT("candidates"), CandidateValues);
		}

		return ErrorObj;
	}

	static bool MatchesMemberFilter(const FString& MemberName, const FString& MemberContains)
	{
		return MemberContains.IsEmpty()
			|| MemberName.Contains(MemberContains, ESearchCase::IgnoreCase);
	}

	static bool IsScriptableProperty(const FProperty* Property, bool bIncludeDeprecated)
	{
		if (!Property)
		{
			return false;
		}

		if (!bIncludeDeprecated && Property->HasAnyPropertyFlags(CPF_Deprecated))
		{
			return false;
		}

		if (Property->HasAnyPropertyFlags(CPF_DuplicateTransient))
		{
			return false;
		}

		return Property->HasAnyPropertyFlags(
			CPF_Edit | CPF_BlueprintVisible | CPF_BlueprintReadOnly | CPF_BlueprintAssignable);
	}

	static bool IsScriptableFunction(const UFunction* Function, bool bIncludeDeprecated)
	{
		if (!Function)
		{
			return false;
		}

		if (!bIncludeDeprecated && Function->HasMetaData(TEXT("DeprecatedFunction")))
		{
			return false;
		}

		if (Function->HasAnyFunctionFlags(FUNC_Delegate | FUNC_Private | FUNC_Protected))
		{
			return false;
		}

		return Function->HasAnyFunctionFlags(
			FUNC_BlueprintCallable | FUNC_BlueprintPure | FUNC_BlueprintEvent | FUNC_BlueprintAuthorityOnly);
	}

	static FString GetClassCppType(UClass* Class)
	{
		if (!Class)
		{
			return FString();
		}

		const FString Prefix = Class->GetPrefixCPP();
		const FString Name = Class->GetName();
		return Prefix.IsEmpty() ? Name : Prefix + Name;
	}

	static void AppendPropertyFlags(const FProperty* Property, TArray<FString>& Flags)
	{
		if (Property->HasAnyPropertyFlags(CPF_Edit))
		{
			Flags.Add(TEXT("Edit"));
		}
		if (Property->HasAnyPropertyFlags(CPF_BlueprintVisible))
		{
			Flags.Add(TEXT("BlueprintVisible"));
		}
		if (Property->HasAnyPropertyFlags(CPF_BlueprintReadOnly))
		{
			Flags.Add(TEXT("BlueprintReadOnly"));
		}
		if (Property->HasAnyPropertyFlags(CPF_BlueprintAssignable))
		{
			Flags.Add(TEXT("BlueprintAssignable"));
		}
		if (Property->HasAnyPropertyFlags(CPF_Config))
		{
			Flags.Add(TEXT("Config"));
		}
	}

	static void AppendFunctionFlags(const UFunction* Function, TArray<FString>& Flags)
	{
		if (Function->HasAnyFunctionFlags(FUNC_BlueprintCallable))
		{
			Flags.Add(TEXT("BlueprintCallable"));
		}
		if (Function->HasAnyFunctionFlags(FUNC_BlueprintPure))
		{
			Flags.Add(TEXT("BlueprintPure"));
		}
		if (Function->HasAnyFunctionFlags(FUNC_BlueprintEvent))
		{
			Flags.Add(TEXT("BlueprintEvent"));
		}
		if (Function->HasAnyFunctionFlags(FUNC_Static))
		{
			Flags.Add(TEXT("Static"));
		}
		if (Function->HasAnyFunctionFlags(FUNC_Net))
		{
			Flags.Add(TEXT("Net"));
		}
	}

	static TSharedPtr<FJsonObject> BuildPropertyJson(FProperty* Property, bool bIncludeDeclaredOn, UClass* OwnerClass)
	{
		if (!Property)
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> PropJson = MakeShareable(new FJsonObject);
		PropJson->SetStringField(TEXT("name"), Property->GetName());
		PropJson->SetStringField(TEXT("cpp_type"), Property->GetCPPType(nullptr, 0));

		const FString UeType = Property->GetClass() ? Property->GetClass()->GetName() : TEXT("Unknown");
		if (!UeType.Equals(TEXT("ObjectProperty")) && !UeType.Equals(TEXT("StructProperty")))
		{
			PropJson->SetStringField(TEXT("ue_type"), UeType);
		}

		TArray<FString> Flags;
		AppendPropertyFlags(Property, Flags);
		if (Flags.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> FlagValues;
			for (const FString& Flag : Flags)
			{
				FlagValues.Add(MakeShareable(new FJsonValueString(Flag)));
			}
			PropJson->SetArrayField(TEXT("flags"), FlagValues);
		}

		if (bIncludeDeclaredOn && OwnerClass)
		{
			PropJson->SetStringField(TEXT("declared_on"), OwnerClass->GetName());
		}

		return PropJson;
	}

	static TSharedPtr<FJsonObject> BuildFunctionJson(UFunction* Function, bool bIncludeDeclaredOn, UClass* OwnerClass)
	{
		if (!Function)
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> FuncJson = MakeShareable(new FJsonObject);
		FuncJson->SetStringField(TEXT("name"), Function->GetName());

		TArray<FString> Flags;
		AppendFunctionFlags(Function, Flags);
		if (Flags.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> FlagValues;
			for (const FString& Flag : Flags)
			{
				FlagValues.Add(MakeShareable(new FJsonValueString(Flag)));
			}
			FuncJson->SetArrayField(TEXT("flags"), FlagValues);
		}

		TArray<TSharedPtr<FJsonValue>> ParamsJson;
		TSharedPtr<FJsonObject> ReturnJson;

		for (TFieldIterator<FProperty> ParamIt(Function); ParamIt; ++ParamIt)
		{
			FProperty* ParamProp = *ParamIt;
			if (!ParamProp)
			{
				continue;
			}

			if (ParamProp->HasAnyPropertyFlags(CPF_ReturnParm))
			{
				ReturnJson = MakeShareable(new FJsonObject);
				ReturnJson->SetStringField(TEXT("name"), ParamProp->GetName());
				ReturnJson->SetStringField(TEXT("cpp_type"), ParamProp->GetCPPType(nullptr, 0));
				continue;
			}

			if (!ParamProp->HasAnyPropertyFlags(CPF_Parm))
			{
				continue;
			}

			TSharedPtr<FJsonObject> ParamJson = MakeShareable(new FJsonObject);
			ParamJson->SetStringField(TEXT("name"), ParamProp->GetName());
			ParamJson->SetStringField(TEXT("cpp_type"), ParamProp->GetCPPType(nullptr, 0));
			if (ParamProp->HasAnyPropertyFlags(CPF_OutParm | CPF_ReferenceParm))
			{
				ParamJson->SetBoolField(TEXT("is_out"), true);
			}

			ParamsJson.Add(MakeShareable(new FJsonValueObject(ParamJson)));
		}

		if (ParamsJson.Num() > 0)
		{
			FuncJson->SetArrayField(TEXT("parameters"), ParamsJson);
		}
		if (ReturnJson.IsValid())
		{
			FuncJson->SetObjectField(TEXT("return"), ReturnJson);
		}

		if (bIncludeDeclaredOn && OwnerClass)
		{
			FuncJson->SetStringField(TEXT("declared_on"), OwnerClass->GetName());
		}

		return FuncJson;
	}

	static UClass* TryLoadClassPath(const FString& ClassPath)
	{
		if (ClassPath.IsEmpty())
		{
			return nullptr;
		}

		if (UClass* LoadedClass = LoadObject<UClass>(nullptr, *ClassPath))
		{
			return LoadedClass;
		}

		if (!ClassPath.EndsWith(TEXT("_C"), ESearchCase::CaseSensitive))
		{
			return LoadObject<UClass>(nullptr, *(ClassPath + TEXT("_C")));
		}

		return nullptr;
	}

	static void CollectExactNameMatches(const FString& ClassName, TArray<UClass*>& OutMatches)
	{
		for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
		{
			UClass* Candidate = *ClassIt;
			if (!Candidate || Candidate->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists))
			{
				continue;
			}

			if (Candidate->GetName().Equals(ClassName, ESearchCase::IgnoreCase))
			{
				OutMatches.AddUnique(Candidate);
			}
		}
	}

	static bool ParseOptions(const FString& ArgumentsJson, FOptions& OutOptions, FString& OutError)
	{
		TSharedPtr<FJsonObject> ArgsObj;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ArgumentsJson);
		if (!(FJsonSerializer::Deserialize(Reader, ArgsObj) && ArgsObj.IsValid()))
		{
			OutError = TEXT("Failed to parse reflection_query arguments");
			return false;
		}

		if (!ArgsObj->TryGetStringField(TEXT("class_name"), OutOptions.ClassName) || OutOptions.ClassName.IsEmpty())
		{
			OutError = TEXT("Missing required field: class_name");
			return false;
		}

		ArgsObj->TryGetStringField(TEXT("member_contains"), OutOptions.MemberContains);
		ArgsObj->TryGetBoolField(TEXT("include_super"), OutOptions.bIncludeSuper);
		ArgsObj->TryGetBoolField(TEXT("include_properties"), OutOptions.bIncludeProperties);
		ArgsObj->TryGetBoolField(TEXT("include_functions"), OutOptions.bIncludeFunctions);
		ArgsObj->TryGetBoolField(TEXT("scriptable_only"), OutOptions.bScriptableOnly);
		ArgsObj->TryGetBoolField(TEXT("include_deprecated"), OutOptions.bIncludeDeprecated);

		double MaxResultsValue = OutOptions.MaxResults;
		if (ArgsObj->TryGetNumberField(TEXT("max_results"), MaxResultsValue))
		{
			OutOptions.MaxResults = FMath::Clamp(static_cast<int32>(MaxResultsValue), 1, 200);
		}

		return true;
	}

	static void CollectNameCandidates(const FString& ClassName, TArray<FString>& OutCandidates, int32 MaxCandidates = 8)
	{
		for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
		{
			UClass* Candidate = *ClassIt;
			if (!Candidate || Candidate->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists))
			{
				continue;
			}

			const FString CandidateName = Candidate->GetName();
			if (CandidateName.Contains(ClassName, ESearchCase::IgnoreCase))
			{
				OutCandidates.AddUnique(Candidate->GetPathName());
				if (OutCandidates.Num() >= MaxCandidates)
				{
					break;
				}
			}
		}
	}

	static UClass* ResolveClass(const FString& ClassName, TArray<FString>& OutAmbiguousCandidates)
	{
		OutAmbiguousCandidates.Reset();

		const bool bLooksLikePath = ClassName.Contains(TEXT("/")) || ClassName.Contains(TEXT("."));
		if (bLooksLikePath)
		{
			if (UClass* LoadedClass = TryLoadClassPath(ClassName))
			{
				return LoadedClass;
			}
		}

		TArray<UClass*> ExactMatches;
		CollectExactNameMatches(ClassName, ExactMatches);

		if (ExactMatches.Num() == 1)
		{
			return ExactMatches[0];
		}

		if (ExactMatches.Num() > 1)
		{
			for (UClass* Match : ExactMatches)
			{
				OutAmbiguousCandidates.Add(Match->GetPathName());
			}
			return nullptr;
		}

		if (UClass* LoadedClass = TryLoadClassPath(ClassName))
		{
			return LoadedClass;
		}

		if (!ClassName.EndsWith(TEXT("_C"), ESearchCase::CaseSensitive))
		{
			CollectExactNameMatches(ClassName + TEXT("_C"), ExactMatches);
			if (ExactMatches.Num() == 1)
			{
				return ExactMatches[0];
			}
			if (ExactMatches.Num() > 1)
			{
				for (UClass* Match : ExactMatches)
				{
					OutAmbiguousCandidates.Add(Match->GetPathName());
				}
				return nullptr;
			}
		}

#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7)
		if (UClass* FirstMatch = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst))
		{
			return FirstMatch;
		}
#else
		if (UClass* LegacyMatch = FindObject<UClass>(ANY_PACKAGE, *ClassName))
		{
			return LegacyMatch;
		}
#endif

		return nullptr;
	}

	static void GatherClassMembers(
		const FOptions& Options,
		UClass* TargetClass,
		EFieldIteratorFlags::SuperClassFlags IteratorFlags,
		TArray<TSharedPtr<FJsonValue>>& OutProperties,
		TArray<TSharedPtr<FJsonValue>>& OutFunctions,
		int32& OutMatchedPropertyCount,
		int32& OutMatchedFunctionCount,
		bool& bOutPropertiesTruncated,
		bool& bOutFunctionsTruncated)
	{
		OutMatchedPropertyCount = 0;
		OutMatchedFunctionCount = 0;
		bOutPropertiesTruncated = false;
		bOutFunctionsTruncated = false;

		if (Options.bIncludeProperties)
		{
			for (TFieldIterator<FProperty> PropIt(TargetClass, IteratorFlags); PropIt; ++PropIt)
			{
				FProperty* Property = *PropIt;
				if (!Property)
				{
					continue;
				}

				if (!MatchesMemberFilter(Property->GetName(), Options.MemberContains))
				{
					continue;
				}

				if (Options.bScriptableOnly && !IsScriptableProperty(Property, Options.bIncludeDeprecated))
				{
					continue;
				}

				++OutMatchedPropertyCount;

				if (OutProperties.Num() >= Options.MaxResults)
				{
					bOutPropertiesTruncated = true;
					continue;
				}

				UClass* DeclaringClass = Property->GetOwnerClass();
				TSharedPtr<FJsonObject> PropJson = BuildPropertyJson(Property, Options.bIncludeSuper, DeclaringClass);
				if (PropJson.IsValid())
				{
					OutProperties.Add(MakeShareable(new FJsonValueObject(PropJson)));
				}
			}
		}

		if (Options.bIncludeFunctions)
		{
			for (TFieldIterator<UFunction> FuncIt(TargetClass, IteratorFlags); FuncIt; ++FuncIt)
			{
				UFunction* Function = *FuncIt;
				if (!Function)
				{
					continue;
				}

				if (!MatchesMemberFilter(Function->GetName(), Options.MemberContains))
				{
					continue;
				}

				if (Options.bScriptableOnly && !IsScriptableFunction(Function, Options.bIncludeDeprecated))
				{
					continue;
				}

				++OutMatchedFunctionCount;

				if (OutFunctions.Num() >= Options.MaxResults)
				{
					bOutFunctionsTruncated = true;
					continue;
				}

				UClass* DeclaringClass = Function->GetOwnerClass();
				TSharedPtr<FJsonObject> FuncJson = BuildFunctionJson(Function, Options.bIncludeSuper, DeclaringClass);
				if (FuncJson.IsValid())
				{
					OutFunctions.Add(MakeShareable(new FJsonValueObject(FuncJson)));
				}
			}
		}
	}

	static FString BuildSchemaJson(UClass* TargetClass, const FOptions& Options)
	{
		TSharedPtr<FJsonObject> Root = MakeShareable(new FJsonObject);
		Root->SetStringField(TEXT("status"), TEXT("ok"));
		Root->SetStringField(TEXT("class_name"), TargetClass->GetName());
		Root->SetStringField(TEXT("path_name"), TargetClass->GetPathName());
		Root->SetStringField(TEXT("cpp_type"), GetClassCppType(TargetClass));

		if (UClass* SuperClass = TargetClass->GetSuperClass())
		{
			Root->SetStringField(TEXT("super_class"), SuperClass->GetName());
		}

		FOptions EffectiveOptions = Options;
		EFieldIteratorFlags::SuperClassFlags IteratorFlags = Options.bIncludeSuper
			? EFieldIteratorFlags::IncludeSuper
			: EFieldIteratorFlags::ExcludeSuper;

		TArray<TSharedPtr<FJsonValue>> PropertiesJson;
		TArray<TSharedPtr<FJsonValue>> FunctionsJson;
		int32 MatchedPropertyCount = 0;
		int32 MatchedFunctionCount = 0;
		bool bPropertiesTruncated = false;
		bool bFunctionsTruncated = false;

		GatherClassMembers(
			EffectiveOptions,
			TargetClass,
			IteratorFlags,
			PropertiesJson,
			FunctionsJson,
			MatchedPropertyCount,
			MatchedFunctionCount,
			bPropertiesTruncated,
			bFunctionsTruncated);

		if (!EffectiveOptions.bIncludeSuper
			&& !EffectiveOptions.MemberContains.IsEmpty()
			&& MatchedPropertyCount == 0
			&& MatchedFunctionCount == 0)
		{
			EffectiveOptions.bIncludeSuper = true;
			PropertiesJson.Reset();
			FunctionsJson.Reset();
			MatchedPropertyCount = 0;
			MatchedFunctionCount = 0;
			bPropertiesTruncated = false;
			bFunctionsTruncated = false;

			IteratorFlags = EFieldIteratorFlags::IncludeSuper;
			GatherClassMembers(
				EffectiveOptions,
				TargetClass,
				IteratorFlags,
				PropertiesJson,
				FunctionsJson,
				MatchedPropertyCount,
				MatchedFunctionCount,
				bPropertiesTruncated,
				bFunctionsTruncated);

			Root->SetBoolField(TEXT("searched_super"), true);
		}

		if (EffectiveOptions.bIncludeProperties)
		{
			Root->SetArrayField(TEXT("properties"), PropertiesJson);
			Root->SetNumberField(TEXT("properties_matched"), MatchedPropertyCount);
			if (bPropertiesTruncated)
			{
				Root->SetBoolField(TEXT("properties_truncated"), true);
			}
		}

		if (EffectiveOptions.bIncludeFunctions)
		{
			Root->SetArrayField(TEXT("functions"), FunctionsJson);
			Root->SetNumberField(TEXT("functions_matched"), MatchedFunctionCount);
			if (bFunctionsTruncated)
			{
				Root->SetBoolField(TEXT("functions_truncated"), true);
			}
		}

		if (MatchedPropertyCount == 0 && MatchedFunctionCount == 0)
		{
			Root->SetStringField(
				TEXT("hint"),
				TEXT("No members matched. Try a broader member_contains filter, set scriptable_only=false, or include_super=true."));
		}

		return SerializeJson(Root);
	}
}

FString FUnrealGPTReflectionQuery::Query(const FString& ArgumentsJson)
{
	using namespace UnrealGPTReflectionQueryPrivate;

	FOptions Options;
	FString ParseError;
	if (!ParseOptions(ArgumentsJson, Options, ParseError))
	{
		return SerializeJson(MakeReflectionError(ParseError));
	}

	TArray<FString> AmbiguousCandidates;
	UClass* TargetClass = ResolveClass(Options.ClassName, AmbiguousCandidates);
	if (!TargetClass)
	{
		if (AmbiguousCandidates.Num() > 0)
		{
			return SerializeJson(MakeReflectionError(
				FString::Printf(TEXT("Ambiguous class name '%s'. Use a fully qualified path."), *Options.ClassName),
				AmbiguousCandidates));
		}

		TArray<FString> NearbyCandidates;
		CollectNameCandidates(Options.ClassName, NearbyCandidates);
		return SerializeJson(MakeReflectionError(
			FString::Printf(TEXT("Class not found: %s"), *Options.ClassName),
			NearbyCandidates));
	}

	return BuildSchemaJson(TargetClass, Options);
}
