// Copyright REBUS Industries.
//
// Async HTTP client for the portal's /api/ue/* contract (ue-plugin-build-guide.md §4).
//
//   * Every request sends the x-api-key header.
//   * Base URL ("PortalUrl") + key come from config / command line (see RebusConfig).
//   * /meshes, /ies and /wheel-image are 307 redirects to time-bounded storage URLs; the
//     UE HTTP stack (libcurl) follows redirects automatically, so the byte fetch lands the
//     storage bytes directly.
//   * Responses are not HTTP-cached (Cache-Control: no-store); callers cache by fixture id.
#pragma once

#include "CoreMinimal.h"
#include "RebusSceneTypes.h"

DECLARE_DELEGATE_TwoParams(FRebusSceneFetched, bool /*bOk*/, const FRebusScene& /*Scene*/);
DECLARE_DELEGATE_TwoParams(FRebusProfileFetched, bool /*bOk*/, const FRebusFixtureProfile& /*Profile*/);
DECLARE_DELEGATE_TwoParams(FRebusBytesFetched, bool /*bOk*/, const TArray<uint8>& /*Bytes*/);

class FRebusRestClient
{
public:
	void Configure(const FString& InBaseUrl, const FString& InApiKey);

	bool IsConfigured() const { return !BaseUrl.IsEmpty() && !ApiKey.IsEmpty(); }
	const FString& GetBaseUrl() const { return BaseUrl; }

	// GET /api/ue/scene?projectId=&modelId=&versionId=
	void FetchScene(const FString& ProjectId, const FString& ModelId, const FString& VersionId,
		FRebusSceneFetched OnComplete);

	// GET /api/ue/fixtures/{libraryFixtureId}
	void FetchFixtureProfile(const FString& LibraryFixtureId, FRebusProfileFetched OnComplete);

	// GET a relative ("/api/ue/...") or absolute URL, returning the raw response bytes.
	// Used for /meshes, /ies and /wheel-image (follows the 307 redirect).
	void FetchBytes(const FString& UrlOrPath, FRebusBytesFetched OnComplete);

	// Resolve a relative path against the configured base URL (absolute URLs pass through).
	FString ResolveUrl(const FString& UrlOrPath) const;

private:
	FString BaseUrl;
	FString ApiKey;
};
