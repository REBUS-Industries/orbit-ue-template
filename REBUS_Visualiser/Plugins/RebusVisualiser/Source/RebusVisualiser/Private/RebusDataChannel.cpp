// Copyright REBUS Industries.
#include "RebusDataChannel.h"
#include "RebusFixtureControlSubsystem.h"
#include "RebusSceneSettingsSubsystem.h"
#include "RebusVisualiserSubsystem.h"
#include "RebusCineCameraPawn.h"
#include "RebusJson.h"
#include "RebusVisualiserLog.h"

#include "Async/Async.h"
#include "Serialization/MemoryReader.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/EngineTypes.h"

// Pixel Streaming 2 public surface (UE 5.5+). If a future engine drop shifts these headers,
// this is the single translation unit that needs touching.
#include "IPixelStreaming2Module.h"
#include "IPixelStreaming2Streamer.h"
#include "IPixelStreaming2InputHandler.h"
#include "PixelStreaming2Delegates.h"

namespace
{
	const TCHAR* RebusResponseType = TEXT("Response");
	const TCHAR* RebusUIInteraction = TEXT("UIInteraction");

	// A Pixel Streaming 2 "UIInteraction" payload is a uint16 character-count prefix followed by
	// that many UTF-16 code units (see FEpicRtcStreamer::OnUIInteraction). The input handler has
	// already stripped the leading message-id byte, so the reader starts at the length prefix.
	// We MUST consume that prefix first -- treating the whole buffer as string data prepends the
	// length as a garbage char and makes every descriptor fail to parse (dropping all control).
	FString ReadDescriptorString(FMemoryReader& Ar)
	{
		if (Ar.TotalSize() - Ar.Tell() < (int64)sizeof(uint16)) return FString();

		uint16 NumUnits = 0;
		Ar << NumUnits;
		if (NumUnits == 0) return FString();

		// Guard against a malformed length that exceeds the remaining payload.
		const int64 Remaining = Ar.TotalSize() - Ar.Tell();
		if (Remaining < (int64)NumUnits * (int64)sizeof(UTF16CHAR)) return FString();

		TArray<UTF16CHAR> Units;
		Units.SetNumUninitialized(NumUnits + 1);
		Ar.Serialize(Units.GetData(), NumUnits * sizeof(UTF16CHAR));
		Units[NumUnits] = 0;
		return FString(StringCast<TCHAR>(reinterpret_cast<const UTF16CHAR*>(Units.GetData())).Get());
	}

	TSharedPtr<IPixelStreaming2Streamer> ResolveStreamer(const FString& StreamerId)
	{
		if (!IPixelStreaming2Module::IsAvailable())
		{
			return nullptr;
		}
		IPixelStreaming2Module& Module = IPixelStreaming2Module::Get();
		const FString Id = StreamerId.IsEmpty() ? Module.GetDefaultStreamerID() : StreamerId;
		return Module.FindStreamer(Id);
	}
}

FRebusDataChannel::~FRebusDataChannel()
{
	if (DataTrackOpenHandle.IsValid())
	{
		if (UPixelStreaming2Delegates* Delegates = UPixelStreaming2Delegates::Get())
		{
			Delegates->OnDataTrackOpenNative.Remove(DataTrackOpenHandle);
		}
		DataTrackOpenHandle.Reset();
	}
}

void FRebusDataChannel::Initialize(const FString& InStreamerId,
	URebusFixtureControlSubsystem* InControl, URebusSceneSettingsSubsystem* InScene)
{
	StreamerId = InStreamerId;
	Control = InControl;
	Scene = InScene;
}

void FRebusDataChannel::SetSceneSettings(URebusSceneSettingsSubsystem* InScene)
{
	Scene = InScene;
}

bool FRebusDataChannel::TryBind()
{
	if (bBound) return true;

	TSharedPtr<IPixelStreaming2Streamer> Streamer = ResolveStreamer(StreamerId);
	if (!Streamer.IsValid())
	{
		return false;
	}

	TSharedPtr<IPixelStreaming2InputHandler> InputHandler = Streamer->GetInputHandler().Pin();
	if (!InputHandler.IsValid())
	{
		return false;
	}

	TWeakPtr<FRebusDataChannel> WeakSelf = AsShared();
	InputHandler->RegisterMessageHandler(RebusUIInteraction,
		[WeakSelf](FString /*SourceId*/, FMemoryReader Ar)
		{
			// Diagnostic: confirms inbound UIInteraction reaches our handler at all (the portal
			// side reports "no Pong/Ready"). If this never logs, the message isn't arriving here.
			UE_LOG(LogRebusVisualiser, Log, TEXT("UIInteraction received (%lld bytes)."), Ar.TotalSize());

			const FString Descriptor = ReadDescriptorString(Ar);
			if (Descriptor.IsEmpty())
			{
				UE_LOG(LogRebusVisualiser, Warning, TEXT("UIInteraction decoded to empty descriptor (length-prefix mismatch?)."));
				return;
			}

			// The message handler can fire off the game thread; route on the game thread.
			AsyncTask(ENamedThreads::GameThread, [WeakSelf, Descriptor]()
			{
				if (TSharedPtr<FRebusDataChannel> Self = WeakSelf.Pin())
				{
					Self->HandleDescriptor(Descriptor);
				}
			});
		});

	// A viewer's data track usually opens AFTER we bind, so the one-shot Ready broadcast below
	// can land before anyone is listening. Subscribe so we can re-greet each viewer on connect.
	if (!DataTrackOpenHandle.IsValid())
	{
		if (UPixelStreaming2Delegates* Delegates = UPixelStreaming2Delegates::Get())
		{
			TWeakPtr<FRebusDataChannel> WeakChannel = AsShared();
			DataTrackOpenHandle = Delegates->OnDataTrackOpenNative.AddLambda(
				[WeakChannel](FString InStreamerId, FString PlayerId)
				{
					if (TSharedPtr<FRebusDataChannel> Self = WeakChannel.Pin())
					{
						Self->OnViewerDataTrackOpen(InStreamerId, PlayerId);
					}
				});
		}
	}

	bBound = true;
	UE_LOG(LogRebusVisualiser, Log, TEXT("Data channel bound to streamer '%s'."),
		StreamerId.IsEmpty() ? TEXT("<default>") : *StreamerId);

	// The portal dismisses its splash on Ready; we fire once we have a bound channel.
	if (!bReadyFired)
	{
		bReadyFired = true;
		OnChannelReady.ExecuteIfBound();
	}
	return true;
}

void FRebusDataChannel::OnViewerDataTrackOpen(FString InStreamerId, FString PlayerId)
{
	// Only react to our own streamer (PRISM may run others in-proc). Empty StreamerId means we
	// fell back to the module default, so accept any in that case.
	if (!StreamerId.IsEmpty() && InStreamerId != StreamerId)
	{
		return;
	}

	UE_LOG(LogRebusVisualiser, Log, TEXT("Viewer data track open (streamer='%s', player='%s'); re-greeting."),
		*InStreamerId, *PlayerId);

	// The delegate can fire off the game thread; re-greet on the game thread.
	TWeakPtr<FRebusDataChannel> WeakSelf = AsShared();
	AsyncTask(ENamedThreads::GameThread, [WeakSelf]()
	{
		if (TSharedPtr<FRebusDataChannel> Self = WeakSelf.Pin())
		{
			Self->OnViewerConnected.ExecuteIfBound();
		}
	});
}

void FRebusDataChannel::HandleDescriptor(const FString& Descriptor)
{
	const TSharedPtr<FJsonObject> Msg = RebusJson::ParseObject(Descriptor);
	if (!Msg.IsValid())
	{
		UE_LOG(LogRebusVisualiser, Verbose, TEXT("Dropped malformed descriptor."));
		return; // drop malformed (§5.1)
	}

	FString Type;
	if (!RebusJson::TryGetString(Msg, TEXT("type"), Type) || Type.IsEmpty())
	{
		return;
	}

	UE_LOG(LogRebusVisualiser, Log, TEXT("Descriptor type '%s'."), *Type);

	// Portal-pushed scene/fixture definition (data-channel alternative to /api/ue/scene). The
	// session owns scene parsing + fixture spawning, so route these straight to it.
	if (Type == TEXT("LoadScene") || Type == TEXT("RegisterFixtureProfile") || Type == TEXT("RegisterFixtureMeshes") || Type == TEXT("RegisterFixtureIes") || Type == TEXT("RegisterFixtureGobos") || Type == TEXT("ClearScene"))
	{
		OnSceneDefinition.ExecuteIfBound(Type, Msg);
		return;
	}

	// Per-fixture / selection first.
	if (URebusFixtureControlSubsystem* Ctl = Control.Get())
	{
		if (Ctl->HandleControlDescriptor(Type, Msg))
		{
			return;
		}
	}

	// Scene / environment / render quality.
	if (URebusSceneSettingsSubsystem* Sce = Scene.Get())
	{
		if (Sce->HandleSceneDescriptor(Type, Msg))
		{
			return;
		}
	}

	// v1.0.79: cinematic-camera control (SetCameraTransform / SetCameraFocalLength / ...).
	// Routed before the generic console-command bypass so the portal can address the camera
	// without colliding with a CVar of the same prefix.
	if (URebusVisualiserSubsystem* Viz = Visualiser.Get())
	{
		if (Viz->HandleCameraDescriptor(Type, Msg))
		{
			return;
		}
	}

	// Read-back triggers + keepalive.
	if (Type == TEXT("RequestSceneState"))
	{
		if (URebusSceneSettingsSubsystem* Sce = Scene.Get())
		{
			SendSceneState(Sce->GetSceneState());
		}
		return;
	}
	if (Type == TEXT("Ping"))
	{
		double Ts = 0.0;
		RebusJson::TryGetNumber(Msg, TEXT("ts"), Ts);
		SendPong(Ts);
		return;
	}

	// v1.0.56: ConsoleCommand bypass. The PS2 protocol-level "Command" / "ConsoleCommand" message
	// (gated by PixelStreaming2.AllowPixelStreamingCommands, forced to 1 in v1.0.55) is the
	// "blessed" path, but it requires the portal's PS2 frontend to actually emit the opcode on
	// the data channel. The user's logs show the gate is 1 (UE-side is ready) yet ZERO
	// protocol-level Command messages arrive -- the PRISM portal frontend either doesn't send
	// them, sends a wrong opcode, or they're dropped before reaching the streamer handler. We
	// can't fix the portal from here, so v1.0.56 piggy-backs console-command execution onto the
	// UIInteraction descriptor channel that demonstrably works (SelectFixtures, RegisterFixture*,
	// LoadScene all flow over it). The portal sends:
	//   { "type": "ConsoleCommand", "command": "<console line>", "silent": false }
	// and we run it on the game thread via GEngine->Exec. SAFETY: we deliberately do NOT filter
	// the command string -- matching Epic's PS2 design, the gate is the portal's existing
	// authentication, not a server-side allowlist. We only refuse empty/whitespace input.
	if (Type == TEXT("ConsoleCommand"))
	{
		FString Cmd;
		RebusJson::TryGetString(Msg, TEXT("command"), Cmd);
		Cmd = Cmd.TrimStartAndEnd();
		if (Cmd.IsEmpty())
		{
			UE_LOG(LogRebusVisualiser, Warning,
				TEXT("Fixture-channel ConsoleCommand: dropped (empty 'command' field). Expected payload: {\"type\":\"ConsoleCommand\",\"command\":\"<line>\",\"silent\":false}"));
			return;
		}

		bool bSilent = false;
		RebusJson::TryGetBool(Msg, TEXT("silent"), bSilent);

		// Pick a live Game / PIE world to scope the exec against, matching the world-selection
		// pattern the existing `Rebus.*` console commands use (RebusVisualiser.cpp
		// HandleMeshBeams/HandleDriveOrbitModels). Falling back to nullptr is fine for
		// engine-scoped commands like `stat fps`, but a real world lets per-world handlers
		// (`Rebus.MeshBeams`, scalability tweaks, etc.) resolve correctly.
		UWorld* ExecWorld = nullptr;
		if (GEngine)
		{
			for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
			{
				if (Ctx.WorldType == EWorldType::Game || Ctx.WorldType == EWorldType::PIE)
				{
					if (UWorld* W = Ctx.World())
					{
						ExecWorld = W;
						break;
					}
				}
			}
		}

		// v1.0.72: Rebus.* prefix tolerance. Portal logs show payloads like
		//   { "command": "ShowOrbitFixtures 0" }
		// arriving without the `Rebus.` namespace prefix that our IConsoleCommand objects are
		// actually registered under (`Rebus.ShowOrbitFixtures`, `Rebus.OverrideFixtureMaterials`,
		// `Rebus.DriveOrbitModels`, `Rebus.MeshBeams`, ...). GEngine->Exec on the bare name
		// returns false (unknown command) and the user sees the previous "success=0" line.
		//
		// To keep the contract loose for the portal (and any future Rebus.* command we add) we
		// try Exec as-given first; if that fails AND the first token has no '.' (so it can't
		// already be a namespaced command), retry once with `Rebus.` prepended. Either-path
		// success counts as success and we log which variant actually ran -- empty PrefixedCmd
		// when the original path succeeded, so existing telemetry stays clean.
		FString PrefixedCmd;
		bool bOk = GEngine ? GEngine->Exec(ExecWorld, *Cmd, *GLog) : false;
		if (!bOk && GEngine)
		{
			FString FirstToken, Rest;
			if (!Cmd.Split(TEXT(" "), &FirstToken, &Rest)) { FirstToken = Cmd; }
			if (!FirstToken.Contains(TEXT(".")))
			{
				PrefixedCmd = FString::Printf(TEXT("Rebus.%s"), *Cmd);
				bOk = GEngine->Exec(ExecWorld, *PrefixedCmd, *GLog);
			}
		}
		if (!bSilent)
		{
			if (!PrefixedCmd.IsEmpty())
			{
				UE_LOG(LogRebusVisualiser, Log,
					TEXT("Fixture-channel ConsoleCommand: '%s' -> retried as '%s' (success=%d)"),
					*Cmd, *PrefixedCmd, bOk ? 1 : 0);
			}
			else
			{
				UE_LOG(LogRebusVisualiser, Log,
					TEXT("Fixture-channel ConsoleCommand: '%s' (success=%d)"),
					*Cmd, bOk ? 1 : 0);
			}
		}
		return;
	}

	// Unknown / not-the-live-path types (SetCamera, SetPostFx, Snapshot, ...) -> ignore (§5.6).
	UE_LOG(LogRebusVisualiser, Verbose, TEXT("Ignoring descriptor type '%s'."), *Type);
}

// ---- Sending --------------------------------------------------------------------------

void FRebusDataChannel::SendEvent(const TSharedRef<FJsonObject>& Event)
{
	TSharedPtr<IPixelStreaming2Streamer> Streamer = ResolveStreamer(StreamerId);
	if (!Streamer.IsValid())
	{
		UE_LOG(LogRebusVisualiser, Warning, TEXT("SendEvent: no streamer '%s' resolved; dropping event."),
			StreamerId.IsEmpty() ? TEXT("<default>") : *StreamerId);
		return;
	}

	FString Out;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Event, Writer);

	FString EventType;
	Event->TryGetStringField(TEXT("type"), EventType);
	UE_LOG(LogRebusVisualiser, Log, TEXT("Sending '%s' (Response, %d players)."),
		*EventType, Streamer->GetConnectedPlayers().Num());

	Streamer->SendAllPlayersMessage(RebusResponseType, Out);
}

void FRebusDataChannel::SendReady(const FString& UeVersion, const FString& ProjectVersion,
	const TArray<FString>& Capabilities, const FString& ProjectId, const FString& ModelId,
	const FString& CommitId, int32 FixtureCount, int32 TrussCount)
{
	TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
	E->SetStringField(TEXT("type"), TEXT("Ready"));
	E->SetStringField(TEXT("ueVersion"), UeVersion);
	E->SetStringField(TEXT("projectVersion"), ProjectVersion);

	TArray<TSharedPtr<FJsonValue>> Caps;
	for (const FString& C : Capabilities) Caps.Add(MakeShared<FJsonValueString>(C));
	E->SetArrayField(TEXT("capabilities"), Caps);

	TSharedRef<FJsonObject> Loaded = MakeShared<FJsonObject>();
	Loaded->SetStringField(TEXT("projectId"), ProjectId);
	Loaded->SetStringField(TEXT("modelId"), ModelId);
	if (!CommitId.IsEmpty()) Loaded->SetStringField(TEXT("commitId"), CommitId);
	Loaded->SetNumberField(TEXT("fixtureCount"), FixtureCount);
	Loaded->SetNumberField(TEXT("trussCount"), TrussCount);
	E->SetObjectField(TEXT("loadedModel"), Loaded);

	SendEvent(E);
}

void FRebusDataChannel::SendFixtureRegistered(const FString& FixtureId, const FString& LibraryFixtureId,
	const FString& DisplayName, bool bHasPanTilt, bool bHasGobo,
	int32 AxisCount, int32 MeshCount)
{
	TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
	E->SetStringField(TEXT("type"), TEXT("FixtureRegistered"));
	E->SetStringField(TEXT("fixtureId"), FixtureId);
	E->SetStringField(TEXT("libraryFixtureId"), LibraryFixtureId);
	E->SetStringField(TEXT("displayName"), DisplayName);
	E->SetBoolField(TEXT("hasPanTilt"), bHasPanTilt);
	E->SetBoolField(TEXT("hasGobo"), bHasGobo);
	// Additive diagnostics so the portal can verify the pushed motionRig/meshes wired up; the
	// portal ignores unknown fields, so these are non-breaking.
	E->SetNumberField(TEXT("axisCount"), AxisCount);
	E->SetNumberField(TEXT("meshCount"), MeshCount);
	SendEvent(E);
}

void FRebusDataChannel::SendSceneState(const TMap<FString, FRebusPropertyValue>& Properties)
{
	TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
	E->SetStringField(TEXT("type"), TEXT("SceneState"));

	TArray<TSharedPtr<FJsonValue>> Props;
	for (const TPair<FString, FRebusPropertyValue>& Pair : Properties)
	{
		TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("name"), Pair.Key);
		P->SetField(TEXT("value"), Pair.Value.ToJson());
		Props.Add(MakeShared<FJsonValueObject>(P));
	}
	E->SetArrayField(TEXT("properties"), Props);
	SendEvent(E);
}

void FRebusDataChannel::SendError(const FString& Code, const FString& Message, bool bFatal, const FString& FixtureId)
{
	TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
	E->SetStringField(TEXT("type"), TEXT("Error"));
	E->SetStringField(TEXT("code"), Code);
	E->SetStringField(TEXT("message"), Message);
	E->SetBoolField(TEXT("fatal"), bFatal);
	if (!FixtureId.IsEmpty()) E->SetStringField(TEXT("fixtureId"), FixtureId);
	SendEvent(E);
}

void FRebusDataChannel::SendNotice(const FString& Code, const FString& Message, const FString& FixtureId)
{
	TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
	E->SetStringField(TEXT("type"), TEXT("Notice"));
	E->SetStringField(TEXT("code"), Code);
	E->SetStringField(TEXT("message"), Message);
	if (!FixtureId.IsEmpty()) E->SetStringField(TEXT("fixtureId"), FixtureId);
	SendEvent(E);
}

void FRebusDataChannel::SendFrameStats(float Fps, float BitrateKbps, float PacketLossPct, float LatencyMs)
{
	TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
	E->SetStringField(TEXT("type"), TEXT("FrameStats"));
	E->SetNumberField(TEXT("fps"), Fps);
	E->SetNumberField(TEXT("bitrateKbps"), BitrateKbps);
	E->SetNumberField(TEXT("packetLossPct"), PacketLossPct);
	E->SetNumberField(TEXT("latencyMs"), LatencyMs);
	SendEvent(E);
}

void FRebusDataChannel::SendPong(double Ts)
{
	TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
	E->SetStringField(TEXT("type"), TEXT("Pong"));
	E->SetNumberField(TEXT("ts"), Ts);
	E->SetNumberField(TEXT("ueClockMs"), FPlatformTime::Seconds() * 1000.0);
	UE_LOG(LogRebusVisualiser, Log, TEXT("Sending Pong (ts=%.0f)."), Ts);
	SendEvent(E);
}

void FRebusDataChannel::SendCameraState(const FRebusCameraState& State)
{
	// Schema: { type:"CameraState", loc:[x,y,z]cm, rot:[pitch,yaw,roll]deg,
	//           focalMm, fStop, focusCm, manualFocus, ev, sensor:{wMm,hMm} }
	// All numbers in UE world units (cm) / degrees so the portal doesn't have to know about
	// UE's coordinate scale -- it just echoes the values it would send back via SetCamera*.
	TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
	E->SetStringField(TEXT("type"), TEXT("CameraState"));

	auto MakeArr3 = [](double A, double B, double C)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Add(MakeShared<FJsonValueNumber>(A));
		Out.Add(MakeShared<FJsonValueNumber>(B));
		Out.Add(MakeShared<FJsonValueNumber>(C));
		return Out;
	};

	E->SetArrayField(TEXT("loc"), MakeArr3(State.Location.X, State.Location.Y, State.Location.Z));
	E->SetArrayField(TEXT("rot"), MakeArr3(State.Rotation.Pitch, State.Rotation.Yaw, State.Rotation.Roll));
	E->SetNumberField(TEXT("focalMm"), State.FocalLengthMm);
	E->SetNumberField(TEXT("fStop"), State.Aperture);
	E->SetNumberField(TEXT("focusCm"), State.FocusDistanceCm);
	E->SetBoolField(TEXT("manualFocus"), State.bManualFocus);
	E->SetNumberField(TEXT("ev"), State.ExposureBiasEv);

	TSharedRef<FJsonObject> Sensor = MakeShared<FJsonObject>();
	Sensor->SetNumberField(TEXT("wMm"), State.SensorWidthMm);
	Sensor->SetNumberField(TEXT("hMm"), State.SensorHeightMm);
	E->SetObjectField(TEXT("sensor"), Sensor);

	SendEvent(E);
}
