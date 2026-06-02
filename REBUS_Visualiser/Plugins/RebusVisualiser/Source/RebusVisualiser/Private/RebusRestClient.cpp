// Copyright REBUS Industries.
#include "RebusRestClient.h"
#include "RebusJson.h"
#include "RebusVisualiserLog.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "GenericPlatform/GenericPlatformHttp.h"

void FRebusRestClient::Configure(const FString& InBaseUrl, const FString& InApiKey)
{
	// Strip a single trailing slash so ResolveUrl can always prepend "/path".
	BaseUrl = InBaseUrl;
	while (BaseUrl.EndsWith(TEXT("/")))
	{
		BaseUrl.LeftChopInline(1);
	}
	ApiKey = InApiKey;

	UE_LOG(LogRebusVisualiser, Log, TEXT("REST configured: base='%s' key=%s"),
		*BaseUrl, ApiKey.IsEmpty() ? TEXT("<missing>") : TEXT("<set>"));
}

FString FRebusRestClient::ResolveUrl(const FString& UrlOrPath) const
{
	if (UrlOrPath.StartsWith(TEXT("http://")) || UrlOrPath.StartsWith(TEXT("https://")))
	{
		return UrlOrPath;
	}
	const FString Path = UrlOrPath.StartsWith(TEXT("/")) ? UrlOrPath : (TEXT("/") + UrlOrPath);
	return BaseUrl + Path;
}

namespace
{
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> MakeGet(const FString& Url, const FString& ApiKey)
	{
		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Req = FHttpModule::Get().CreateRequest();
		Req->SetURL(Url);
		Req->SetVerb(TEXT("GET"));
		Req->SetHeader(TEXT("x-api-key"), ApiKey);
		Req->SetHeader(TEXT("Accept"), TEXT("application/json"));
		return Req;
	}

	bool IsSuccess(FHttpResponsePtr Response, bool bConnectedSuccessfully)
	{
		if (!bConnectedSuccessfully || !Response.IsValid()) return false;
		const int32 Code = Response->GetResponseCode();
		return Code >= 200 && Code < 300;
	}
}

void FRebusRestClient::FetchScene(const FString& ProjectId, const FString& ModelId,
	const FString& VersionId, FRebusSceneFetched OnComplete)
{
	FString Url = ResolveUrl(TEXT("/api/ue/scene"));
	Url += FString::Printf(TEXT("?projectId=%s&modelId=%s"),
		*FGenericPlatformHttp::UrlEncode(ProjectId),
		*FGenericPlatformHttp::UrlEncode(ModelId));
	if (!VersionId.IsEmpty())
	{
		Url += FString::Printf(TEXT("&versionId=%s"), *FGenericPlatformHttp::UrlEncode(VersionId));
	}

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Req = MakeGet(Url, ApiKey);
	Req->OnProcessRequestComplete().BindLambda(
		[OnComplete](FHttpRequestPtr, FHttpResponsePtr Response, bool bOk)
		{
			FRebusScene Scene;
			if (!IsSuccess(Response, bOk))
			{
				UE_LOG(LogRebusVisualiser, Error, TEXT("FetchScene failed (code=%d)"),
					Response.IsValid() ? Response->GetResponseCode() : -1);
				OnComplete.ExecuteIfBound(false, Scene);
				return;
			}
			const bool bParsed = RebusJson::ParseScene(Response->GetContentAsString(), Scene);
			UE_LOG(LogRebusVisualiser, Log, TEXT("FetchScene ok: %d fixtures (parsed=%d)"),
				Scene.Fixtures.Num(), bParsed ? 1 : 0);
			OnComplete.ExecuteIfBound(bParsed, Scene);
		});
	Req->ProcessRequest();
}

void FRebusRestClient::FetchFixtureProfile(const FString& LibraryFixtureId, FRebusProfileFetched OnComplete)
{
	const FString Url = ResolveUrl(FString::Printf(TEXT("/api/ue/fixtures/%s"),
		*FGenericPlatformHttp::UrlEncode(LibraryFixtureId)));

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Req = MakeGet(Url, ApiKey);
	Req->OnProcessRequestComplete().BindLambda(
		[OnComplete, LibraryFixtureId](FHttpRequestPtr, FHttpResponsePtr Response, bool bOk)
		{
			FRebusFixtureProfile Profile;
			if (!IsSuccess(Response, bOk))
			{
				UE_LOG(LogRebusVisualiser, Error, TEXT("FetchFixtureProfile(%s) failed (code=%d)"),
					*LibraryFixtureId, Response.IsValid() ? Response->GetResponseCode() : -1);
				OnComplete.ExecuteIfBound(false, Profile);
				return;
			}
			const bool bParsed = RebusJson::ParseFixtureProfile(Response->GetContentAsString(), Profile);
			OnComplete.ExecuteIfBound(bParsed, Profile);
		});
	Req->ProcessRequest();
}

void FRebusRestClient::FetchBytes(const FString& UrlOrPath, FRebusBytesFetched OnComplete)
{
	const FString Url = ResolveUrl(UrlOrPath);
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Req = MakeGet(Url, ApiKey);
	Req->SetHeader(TEXT("Accept"), TEXT("*/*"));
	Req->OnProcessRequestComplete().BindLambda(
		[OnComplete, Url](FHttpRequestPtr, FHttpResponsePtr Response, bool bOk)
		{
			if (!IsSuccess(Response, bOk))
			{
				UE_LOG(LogRebusVisualiser, Warning, TEXT("FetchBytes(%s) failed (code=%d)"),
					*Url, Response.IsValid() ? Response->GetResponseCode() : -1);
				static const TArray<uint8> Empty;
				OnComplete.ExecuteIfBound(false, Empty);
				return;
			}
			OnComplete.ExecuteIfBound(true, Response->GetContent());
		});
	Req->ProcessRequest();
}
