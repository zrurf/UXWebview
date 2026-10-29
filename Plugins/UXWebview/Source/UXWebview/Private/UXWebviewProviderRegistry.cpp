#include "Provider/UXWebviewProviderRegistry.h"

#include "Misc/ScopeLock.h"
#include "UXWebviewLog.h"
#include "UXWebviewSettings.h"

FUXWebviewProviderRegistry& FUXWebviewProviderRegistry::Get()
{
	static FUXWebviewProviderRegistry Registry;
	return Registry;
}

void FUXWebviewProviderRegistry::RegisterFactory(TSharedPtr<IUXWebviewProviderFactory> Factory)
{
	if (!Factory.IsValid())
	{
		UE_LOG(LogUXWebview, Warning, TEXT("ignoring a null provider factory"));
		return;
	}

	const FUXWebviewProviderInfo Info = Factory->GetInfo();
	if (Info.Name.IsEmpty())
	{
		UE_LOG(LogUXWebview, Warning, TEXT("provider factory without a name was ignored"));
		return;
	}

	FScopeLock ScopeLock(&Lock);
	if (Factories.Contains(Info.Name))
	{
		UE_LOG(LogUXWebview, Warning, TEXT("provider '%s' is already registered, replacing it"), *Info.Name);
	}
	Factories.Add(Info.Name, Factory);
}

void FUXWebviewProviderRegistry::UnregisterFactory(const FString& Name)
{
	FScopeLock ScopeLock(&Lock);
	Factories.Remove(Name);
}

TArray<FUXWebviewProviderInfo> FUXWebviewProviderRegistry::GetProviders() const
{
	TArray<FUXWebviewProviderInfo> Result;
	{
		FScopeLock ScopeLock(&Lock);
		Result.Reserve(Factories.Num());
		for (const TPair<FString, TSharedPtr<IUXWebviewProviderFactory>>& Pair : Factories)
		{
			Result.Add(Pair.Value->GetInfo());
		}
	}
	Result.Sort([](const FUXWebviewProviderInfo& A, const FUXWebviewProviderInfo& B)
	{
		return A.Name < B.Name;
	});
	return Result;
}

TSharedPtr<IUXWebviewProviderFactory> FUXWebviewProviderRegistry::FindFactory(const FString& Name) const
{
	FScopeLock ScopeLock(&Lock);
	if (const TSharedPtr<IUXWebviewProviderFactory>* Found = Factories.Find(Name))
	{
		return *Found;
	}
	for (const TPair<FString, TSharedPtr<IUXWebviewProviderFactory>>& Pair : Factories)
	{
		if (Pair.Key.Equals(Name, ESearchCase::IgnoreCase))
		{
			return Pair.Value;
		}
	}
	return nullptr;
}

bool FUXWebviewProviderRegistry::FindProvider(const FString& Name, FUXWebviewProviderInfo& OutInfo) const
{
	TSharedPtr<IUXWebviewProviderFactory> Factory = FindFactory(Name);
	if (!Factory.IsValid())
	{
		return false;
	}
	OutInfo = Factory->GetInfo();
	return true;
}

FString FUXWebviewProviderRegistry::BuiltInDefaultProvider()
{
	return TEXT("CEF");
}

FUXWebviewResult FUXWebviewProviderRegistry::CreateProvider(
	const FString& Name, TUniquePtr<IUXWebviewProvider>& OutProvider) const
{
	TSharedPtr<IUXWebviewProviderFactory> Factory = FindFactory(Name);
	if (!Factory.IsValid())
	{
		return FUXWebviewResult::Fail(EUXWebviewResultCode::NotFound,
			FString::Printf(TEXT("provider '%s' is not registered"), *Name));
	}
	OutProvider = Factory->Create();
	if (!OutProvider)
	{
		return FUXWebviewResult::Fail(EUXWebviewResultCode::Unknown,
			FString::Printf(TEXT("provider '%s' failed to create an instance"), *Name));
	}
	return FUXWebviewResult::Ok();
}

FUXWebviewResult FUXWebviewProviderRegistry::Resolve(
	const FUXWebviewCreateParams& Params, FString& OutName) const
{
	const UUXWebviewSettings& Settings = UUXWebviewSettings::Get();
	const bool bFallbackAllowed = Params.bAllowProviderFallback && Settings.bAllowProviderFallback;
	TArray<FString> Candidates;
	TArray<FString> Reasons;

	if (!Params.ProviderName.IsEmpty())
	{
		TSharedPtr<IUXWebviewProviderFactory> Factory = FindFactory(Params.ProviderName);
		if (Factory.IsValid())
		{
			const FUXWebviewProviderInfo Info = Factory->GetInfo();
			if (Info.bAvailable)
			{
				OutName = Info.Name;
				return FUXWebviewResult::Ok();
			}
			if (!bFallbackAllowed)
			{
				return FUXWebviewResult::Fail(EUXWebviewResultCode::NotSupported,
					FString::Printf(TEXT("provider '%s' is unavailable: %s"), *Info.Name,
						Info.UnavailableReason.IsEmpty() ? TEXT("no reason given") : *Info.UnavailableReason));
			}
		}
		else if (!bFallbackAllowed)
		{
			return FUXWebviewResult::Fail(EUXWebviewResultCode::NotFound,
				FString::Printf(TEXT("provider '%s' is not registered"), *Params.ProviderName));
		}
	}
	else
	{
		Candidates.Add(Settings.PlatformDefaultProvider.IsEmpty()
			? BuiltInDefaultProvider()
			: Settings.PlatformDefaultProvider);
	}

	Candidates.Append(Settings.FallbackProviders);

	for (const FString& Candidate : Candidates)
	{
		if (Params.ProviderName.Equals(Candidate, ESearchCase::IgnoreCase))
		{
			continue;
		}

		TSharedPtr<IUXWebviewProviderFactory> Factory = FindFactory(Candidate);
		if (!Factory.IsValid())
		{
			Reasons.Add(FString::Printf(TEXT("'%s' is not registered"), *Candidate));
			continue;
		}

		const FUXWebviewProviderInfo Info = Factory->GetInfo();
		if (Info.bAvailable)
		{
			OutName = Info.Name;
			return FUXWebviewResult::Ok();
		}

		Reasons.Add(FString::Printf(TEXT("'%s' is unavailable: %s"), *Info.Name,
			Info.UnavailableReason.IsEmpty() ? TEXT("no reason given") : *Info.UnavailableReason));
	}

	return FUXWebviewResult::Fail(EUXWebviewResultCode::NotFound,
		FString::Join(Reasons, TEXT("; ")));
}
