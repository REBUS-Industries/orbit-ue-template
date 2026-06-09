// Copyright REBUS Industries.
//
// v1.0.130 -- portal-hosted floor-surface texture fetcher.
//
// The Python builder (`Content/Python/build_rebus_base_level.py`) calls
// `URebusSurfaceFetcher::EnsureSurfaceCached` for each of the four floor
// surfaces (Concrete / Grass / Sand / Tarmac). The fetcher synchronously
// downloads the surface's `manifest.json` + four-texture bundle from the
// portal (same `PortalUrl` + `x-api-key` auth scheme the rest of the
// plugin uses through `FRebusRestClient`), validates SHA256 + size on
// every file, and writes the result into a per-machine cache under
// `<UserSettings>/Rebus/SurfaceCache/<name>/`.
//
// Why the v1.0.130 mid-flight pivot from "pull direct from Fab" to
// "pull from the portal":
//   * Fab's UE editor plug-in is gated by the editor's `Window > Fab`
//     interactive login flow; there's no token-based / unattended path
//     for an offline commandlet bake.
//   * Fab exposes ZERO Python-callable methods (verified across UE 5.7's
//     `unreal` bindings -- no `unreal.FabSubsystem`, no
//     `BlueprintCallable` UFUNCTIONs in `Engine/Plugins/Fab/Source/Fab/
//     Public/`, only `Fab.{ShowSettings,Login,Logout,ClearCache,
//     SetEnvironment,TEDS.MyFolderIntegration}` console commands -- none
//     of which can drive a per-listing automated download workflow).
//   * Operating against the Fab REST API directly would leak the user's
//     Fab credentials per-machine and pin the build to a per-developer
//     Fab account.
// Routing the surface bundles through the portal lets the v1.0.130 build
// reuse the SAME `x-api-key` auth header / `PortalUrl` base config the
// rest of the plugin already wires; the portal team uploads the four
// Megascans surface bundles ONCE (see the v1.0.130 README release block
// for the exact endpoint contract) and every UE client that runs the
// commandlet bake gets them without any per-developer login.
//
// The fetcher degrades gracefully when the portal isn't yet hosting the
// surfaces: a previous bake's cached files are reused verbatim; if no
// cache exists either, the four child MIs at /Game/REBUS/Materials/
// {Concrete,Grass,Sand,Tarmac}.uasset stay parented to whatever they
// pointed at in v1.0.129 (the build does NOT fail) and a single LOUD
// warning logs the portal-side action item the operator should
// escalate.
#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "RebusSurfaceFetcher.generated.h"

UENUM(BlueprintType)
enum class ERebusSurfaceFetchStatus : uint8
{
	// Every file in the manifest is present in the cache + matches its
	// declared SHA256/size. The on-disk bundle is current.
	Ok                = 0,
	// Cache had every file but the portal manifest declared a different
	// revision number; the new files were downloaded + the cache replaced.
	Refreshed         = 1,
	// Portal was unreachable (manifest fetch failed) but a previous
	// bake's cache exists locally and was used verbatim.
	OfflineCached     = 2,
	// Portal was unreachable AND no cache exists locally. The caller
	// MUST treat this as "no surface bundle available"; the floor stays
	// at the v1.0.129 fallback (legacy MI_RebusGround_<preset>).
	OfflineNoCache    = 3,
	// Manifest was fetched + parsed, but a per-file fetch failed (either
	// HTTP error or SHA256/size mismatch). Caller logged the per-file
	// detail; the surface bundle is INCOMPLETE.
	FetchFailed       = 4,
	// Portal config (PortalUrl OR ApiKey) is missing entirely. No
	// download was attempted.
	NotConfigured     = 5,
};

UCLASS()
class REBUSVISUALISER_API URebusSurfaceFetcher : public UObject
{
	GENERATED_BODY()

public:
	// Fetch (or refresh) the per-surface bundle from the portal into the
	// per-machine cache directory. Synchronous: the call ticks the engine
	// HTTP manager until every file completes, so it's safe to drive from
	// a Python commandlet bake or from `Tools > Execute Python Script`
	// without writing async polling glue. Total wall-clock cost: typically
	// a few hundred milliseconds per surface for a healthy local-network
	// portal; up to ~30 s on a cold WAN link. The HTTP manager runs on
	// the game thread so calling this from a long-running editor session
	// will briefly block the UI -- intentional for an offline bake step,
	// not a per-frame runtime call.
	//
	// Inputs:
	//   * Surface           -- one of "Concrete" / "Grass" / "Sand" / "Tarmac"
	//                          (matches the keys in the Python builder's
	//                          PORTAL_SURFACE_MANIFEST). Used to compose
	//                          `<PortalUrl>/assets/surfaces/<name>/manifest.json`
	//                          for the manifest fetch + the per-file URLs.
	//   * RelativePath      -- the value from PORTAL_SURFACE_MANIFEST,
	//                          e.g. "surfaces/concrete". Joined with
	//                          `<PortalUrl>/assets/` to compose the
	//                          manifest URL. Letting Python supply this
	//                          rather than hard-coding "surfaces/<lower-
	//                          name>" inside C++ keeps the layout entirely
	//                          configurable from the builder constants
	//                          without a C++ rebuild.
	//
	// Outputs (return + out-params):
	//   * Returns the ERebusSurfaceFetchStatus enum value above so Python
	//     can branch on the four "OK / Refreshed / OfflineCached /
	//     OfflineNoCache / FetchFailed / NotConfigured" outcomes.
	//   * OutCacheDir   -- absolute filesystem path to the on-disk cache
	//                      folder for this surface (e.g.
	//                      `C:/Users/<u>/AppData/Local/Rebus/SurfaceCache/
	//                      Concrete`). Populated regardless of status so
	//                      Python can still try to import any partial
	//                      content on FetchFailed.
	//   * OutFiles      -- the file basenames the bundle is expected to
	//                      carry (e.g. `T_Concrete_B.png`,
	//                      `T_Concrete_N.png`, `T_Concrete_ORM.png`,
	//                      `T_Concrete_H.png`). On Ok / Refreshed every
	//                      entry is guaranteed to exist + match its
	//                      manifest SHA256. On OfflineCached the entries
	//                      are the file names actually found in the
	//                      cache (manifest is unknown). Empty on
	//                      OfflineNoCache / FetchFailed / NotConfigured.
	//   * OutRevision   -- manifest revision integer (-1 if no manifest
	//                      was fetched). The Python builder logs this so
	//                      the operator can confirm a cache invalidation
	//                      happened on a portal revision bump.
	//
	// Side effects:
	//   * Creates the cache directory if missing (idempotent).
	//   * Writes a `manifest.json` sidecar in the cache directory after
	//     a successful fetch so subsequent bakes can short-circuit on a
	//     matching revision.
	//   * Logs every per-file fetch + every cache-hit short-circuit at
	//     the LogRebusVisualiser channel (Verbose for hits, Log for
	//     misses, Error for failures + offline-no-cache).
	UFUNCTION(BlueprintCallable, Category = "Rebus|Surface",
		meta = (DisplayName = "Ensure Floor Surface Cached"))
	static ERebusSurfaceFetchStatus EnsureSurfaceCached(
		const FString& Surface,
		const FString& RelativePath,
		FString& OutCacheDir,
		TArray<FString>& OutFiles,
		int32& OutRevision);

	// Dump the cache root + the four supported surface subfolders + each
	// surface's most recent manifest revision (or "no cache" if absent)
	// to the OutputLog. Pure diagnostic helper -- the operator runs
	// `py unreal.RebusSurfaceFetcher.dump_cache_status()` from the
	// editor's Python REPL when the floor surfaces aren't appearing as
	// expected. No HTTP IO; reads disk only.
	UFUNCTION(BlueprintCallable, Category = "Rebus|Surface",
		meta = (DisplayName = "Dump Surface Cache Status"))
	static void DumpCacheStatus();

	// Returns the per-machine cache root directory
	// (`<UserSettings>/Rebus/SurfaceCache`). Pure path helper exposed so
	// Python can compose absolute paths without re-deriving
	// `FPlatformProcess::UserSettingsDir()`.
	UFUNCTION(BlueprintCallable, Category = "Rebus|Surface",
		meta = (DisplayName = "Get Surface Cache Root"))
	static FString GetCacheRoot();

	// Returns the per-machine cache directory for ONE surface (e.g.
	// `<UserSettings>/Rebus/SurfaceCache/Concrete`). Creates it if
	// missing so callers never have to handle the non-existent path
	// case.
	UFUNCTION(BlueprintCallable, Category = "Rebus|Surface",
		meta = (DisplayName = "Get Surface Cache Dir"))
	static FString GetSurfaceCacheDir(const FString& Surface);
};
