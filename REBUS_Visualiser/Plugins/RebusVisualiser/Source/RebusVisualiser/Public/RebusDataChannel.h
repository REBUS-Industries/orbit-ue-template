// Copyright REBUS Industries.
//
// Pixel Streaming 2 data-channel transport (ue-plugin-build-guide.md §5/§6/§10).
//
// Receive: registers a message handler for the "UIInteraction" opcode on the streamer's
// input handler (PS2 removed the C++ UPixelStreamingInput component; you register handlers on
// the streamer's IPixelStreaming2InputHandler instead). Each descriptor arrives as a string,
// is parsed to JSON, and routed by `type`.
//
// Send: UE->portal events go back via IPixelStreaming2Streamer::SendAllPlayerMessage with the
// "Response" message type, which the PS2 frontend delivers to the portal's response listener
// (parseUeToPortalEvent).
//
// The streamer is discovered by the id PRISM passes via -PixelStreamingID=orbit_<shortRunId>.
// We never hardcode DefaultStreamer (§10.2): if no id is configured we fall back to the
// module's default streamer id.
#pragma once

#include "CoreMinimal.h"
#include "RebusPropertyValue.h"

class URebusFixtureControlSubsystem;
class URebusSceneSettingsSubsystem;
class FJsonObject;

DECLARE_DELEGATE(FRebusOnChannelReady); // fired once when the data channel first opens
DECLARE_DELEGATE(FRebusOnViewerConnected); // fired each time a viewer's data track opens
// Portal pushes scene/fixture definitions over the channel (data-channel alternative to the
// /api/ue/scene REST contract). Carries the descriptor type + parsed JSON object.
DECLARE_DELEGATE_TwoParams(FRebusOnSceneDefinition, const FString& /*Type*/, const TSharedPtr<FJsonObject>& /*Msg*/);

class FRebusDataChannel : public TSharedFromThis<FRebusDataChannel>
{
public:
	~FRebusDataChannel();

	void Initialize(const FString& InStreamerId,
		URebusFixtureControlSubsystem* InControl,
		URebusSceneSettingsSubsystem* InScene);

	// The scene-settings subsystem is world-scoped and may not exist yet at Initialize time;
	// the session wires it in once a game world is live.
	void SetSceneSettings(URebusSceneSettingsSubsystem* InScene);

	// Poll for the streamer + input handler and bind the UIInteraction handler. Safe to call
	// repeatedly; returns true once bound.
	bool TryBind();
	bool IsBound() const { return bBound; }

	// The session wires this to know when to emit Ready + FixtureRegistered.
	FRebusOnChannelReady OnChannelReady;

	// Fired (game thread) whenever a viewer's data track opens. A viewer typically connects a
	// beat AFTER the channel binds, so the one-shot Ready broadcast can be missed; the session
	// re-broadcasts the handshake on this so every (re)connecting viewer becomes controllable.
	FRebusOnViewerConnected OnViewerConnected;

	// Fired (game thread) when the portal pushes a scene/fixture definition over the channel.
	FRebusOnSceneDefinition OnSceneDefinition;

	// ---- UE -> portal read-back (§6) ----
	void SendReady(const FString& UeVersion, const FString& ProjectVersion,
		const TArray<FString>& Capabilities,
		const FString& ProjectId, const FString& ModelId, const FString& CommitId,
		int32 FixtureCount, int32 TrussCount);
	void SendFixtureRegistered(const FString& FixtureId, const FString& LibraryFixtureId,
		const FString& DisplayName, bool bHasPanTilt, bool bHasGobo);
	void SendSceneState(const TMap<FString, FRebusPropertyValue>& Properties);
	void SendError(const FString& Code, const FString& Message, bool bFatal, const FString& FixtureId = FString());
	void SendNotice(const FString& Code, const FString& Message, const FString& FixtureId = FString());
	void SendFrameStats(float Fps, float BitrateKbps, float PacketLossPct, float LatencyMs);

private:
	void HandleDescriptor(const FString& Descriptor);
	void SendEvent(const TSharedRef<FJsonObject>& Event);
	void SendPong(double Ts);
	void OnViewerDataTrackOpen(FString InStreamerId, FString PlayerId);

private:
	FString StreamerId;
	TWeakObjectPtr<URebusFixtureControlSubsystem> Control;
	TWeakObjectPtr<URebusSceneSettingsSubsystem> Scene;
	FDelegateHandle DataTrackOpenHandle;
	bool bBound = false;
	bool bReadyFired = false;
};
