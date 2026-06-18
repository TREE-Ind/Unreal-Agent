// Copyright (c) 2025 TREE Industries.

#include "UnrealGPTClarifyTypes.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

bool FUnrealGPTClarifyTypes::ParseRequest(const FString& ArgumentsJson, FClarifyRequest& OutRequest)
{
	OutRequest = FClarifyRequest();

	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ArgumentsJson);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		OutRequest.ParseError = TEXT("Failed to parse clarify arguments as JSON.");
		return false;
	}

	Root->TryGetStringField(TEXT("title"), OutRequest.Title);

	const TArray<TSharedPtr<FJsonValue>>* QuestionsArray = nullptr;
	if (!Root->TryGetArrayField(TEXT("questions"), QuestionsArray) || !QuestionsArray || QuestionsArray->Num() == 0)
	{
		OutRequest.ParseError = TEXT("clarify requires at least one question.");
		return false;
	}

	if (QuestionsArray->Num() > 5)
	{
		OutRequest.ParseError = TEXT("clarify supports at most 5 questions per call.");
		return false;
	}

	for (const TSharedPtr<FJsonValue>& QuestionValue : *QuestionsArray)
	{
		const TSharedPtr<FJsonObject> QuestionObj = QuestionValue.IsValid() ? QuestionValue->AsObject() : nullptr;
		if (!QuestionObj.IsValid())
		{
			OutRequest.ParseError = TEXT("Each question must be a JSON object.");
			return false;
		}

		FClarifyQuestion Question;
		if (!QuestionObj->TryGetStringField(TEXT("id"), Question.Id) || Question.Id.IsEmpty())
		{
			OutRequest.ParseError = TEXT("Each question requires a non-empty id.");
			return false;
		}

		if (!QuestionObj->TryGetStringField(TEXT("prompt"), Question.Prompt) || Question.Prompt.IsEmpty())
		{
			OutRequest.ParseError = FString::Printf(TEXT("Question '%s' requires a non-empty prompt."), *Question.Id);
			return false;
		}

		QuestionObj->TryGetBoolField(TEXT("allow_multiple"), Question.bAllowMultiple);

		const TArray<TSharedPtr<FJsonValue>>* OptionsArray = nullptr;
		if (!QuestionObj->TryGetArrayField(TEXT("options"), OptionsArray) || !OptionsArray || OptionsArray->Num() == 0)
		{
			OutRequest.ParseError = FString::Printf(TEXT("Question '%s' requires at least one option."), *Question.Id);
			return false;
		}

		for (const TSharedPtr<FJsonValue>& OptionValue : *OptionsArray)
		{
			const TSharedPtr<FJsonObject> OptionObj = OptionValue.IsValid() ? OptionValue->AsObject() : nullptr;
			if (!OptionObj.IsValid())
			{
				OutRequest.ParseError = FString::Printf(TEXT("Question '%s' has an invalid option entry."), *Question.Id);
				return false;
			}

			FClarifyOption Option;
			if (!OptionObj->TryGetStringField(TEXT("id"), Option.Id) || Option.Id.IsEmpty())
			{
				OutRequest.ParseError = FString::Printf(TEXT("Question '%s' has an option without id."), *Question.Id);
				return false;
			}

			if (!OptionObj->TryGetStringField(TEXT("label"), Option.Label) || Option.Label.IsEmpty())
			{
				OutRequest.ParseError = FString::Printf(TEXT("Question '%s' option '%s' requires a label."), *Question.Id, *Option.Id);
				return false;
			}

			Question.Options.Add(Option);
		}

		OutRequest.Questions.Add(Question);
	}

	return true;
}

static FString SerializeClarifyStatusObject(const FString& Status, const TSharedPtr<FJsonObject>& AnswersObj = nullptr)
{
	TSharedPtr<FJsonObject> Root = MakeShareable(new FJsonObject);
	Root->SetStringField(TEXT("status"), Status);
	if (AnswersObj.IsValid())
	{
		Root->SetObjectField(TEXT("answers"), AnswersObj);
	}

	FString Output;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
	return Output;
}

FString FUnrealGPTClarifyTypes::BuildAnsweredResult(const TMap<FString, TPair<TArray<FString>, FString>>& AnswersByQuestionId)
{
	TSharedPtr<FJsonObject> AnswersObj = MakeShareable(new FJsonObject);

	for (const TPair<FString, TPair<TArray<FString>, FString>>& Pair : AnswersByQuestionId)
	{
		TSharedPtr<FJsonObject> AnswerObj = MakeShareable(new FJsonObject);

		TArray<TSharedPtr<FJsonValue>> SelectedValues;
		for (const FString& SelectedId : Pair.Value.Key)
		{
			SelectedValues.Add(MakeShareable(new FJsonValueString(SelectedId)));
		}
		AnswerObj->SetArrayField(TEXT("selected"), SelectedValues);

		if (!Pair.Value.Value.IsEmpty())
		{
			AnswerObj->SetStringField(TEXT("other_text"), Pair.Value.Value);
		}
		else
		{
			AnswerObj->SetField(TEXT("other_text"), MakeShareable(new FJsonValueNull));
		}

		AnswersObj->SetObjectField(Pair.Key, AnswerObj);
	}

	return SerializeClarifyStatusObject(TEXT("answered"), AnswersObj);
}

FString FUnrealGPTClarifyTypes::BuildCancelledResult()
{
	return SerializeClarifyStatusObject(TEXT("cancelled"));
}

FString FUnrealGPTClarifyTypes::BuildErrorResult(const FString& Message)
{
	TSharedPtr<FJsonObject> Root = MakeShareable(new FJsonObject);
	Root->SetStringField(TEXT("status"), TEXT("error"));
	Root->SetStringField(TEXT("message"), Message);

	FString Output;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
	return Output;
}

FString FUnrealGPTClarifyTypes::FormatAnswerSummary(const FString& ResultJson, const FClarifyRequest& Request)
{
	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResultJson);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return ResultJson;
	}

	FString Status;
	if (!Root->TryGetStringField(TEXT("status"), Status))
	{
		return ResultJson;
	}

	if (Status == TEXT("cancelled"))
	{
		return TEXT("Clarification skipped.");
	}

	if (Status == TEXT("error"))
	{
		FString Message;
		Root->TryGetStringField(TEXT("message"), Message);
		return Message.IsEmpty() ? TEXT("Clarification failed.") : Message;
	}

	if (Status != TEXT("answered"))
	{
		return ResultJson;
	}

	const TSharedPtr<FJsonObject>* AnswersObjPtr = nullptr;
	if (!Root->TryGetObjectField(TEXT("answers"), AnswersObjPtr) || !AnswersObjPtr || !AnswersObjPtr->IsValid())
	{
		return TEXT("Clarification answered.");
	}

	FString Summary;
	for (const FClarifyQuestion& Question : Request.Questions)
	{
		const TSharedPtr<FJsonObject>* AnswerObjPtr = nullptr;
		if (!(*AnswersObjPtr)->TryGetObjectField(Question.Id, AnswerObjPtr) || !AnswerObjPtr || !AnswerObjPtr->IsValid())
		{
			continue;
		}

		TArray<FString> Labels;
		const TArray<TSharedPtr<FJsonValue>>* SelectedArray = nullptr;
		if ((*AnswerObjPtr)->TryGetArrayField(TEXT("selected"), SelectedArray) && SelectedArray)
		{
			for (const TSharedPtr<FJsonValue>& SelectedValue : *SelectedArray)
			{
				FString SelectedId;
				if (!SelectedValue.IsValid() || !SelectedValue->TryGetString(SelectedId))
				{
					continue;
				}

				if (SelectedId == ClarifyOtherOptionId)
				{
					FString OtherText;
					(*AnswerObjPtr)->TryGetStringField(TEXT("other_text"), OtherText);
					Labels.Add(OtherText.IsEmpty() ? TEXT("Other") : OtherText);
					continue;
				}

				for (const FClarifyOption& Option : Question.Options)
				{
					if (Option.Id == SelectedId)
					{
						Labels.Add(Option.Label);
						break;
					}
				}
			}
		}

		if (Labels.Num() > 0)
		{
			if (!Summary.IsEmpty())
			{
				Summary += TEXT("\n");
			}
			Summary += FString::Printf(TEXT("%s: %s"), *Question.Prompt, *FString::Join(Labels, TEXT(", ")));
		}
	}

	return Summary.IsEmpty() ? TEXT("Clarification answered.") : Summary;
}
