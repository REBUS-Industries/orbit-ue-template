// Copyright REBUS Industries.
#include "RebusDataChannel.h"
#include "RebusFixtureControlSubsystem.h"
#include "RebusSceneSettingsSubsystem.h"
#include "RebusJson.h"
#include "RebusVisualiserLog.h"

#include "Async/Async.h"
#include "Serialization/MemoryReader.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

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
	const FString& DisplayName, bool bHasPanTilt, bool bHasGobo)
{
	TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
	E->SetStringField(TEXT("type"), TEXT("FixtureRegistered"));
	E->SetStringField(TEXT("fixtureId"), FixtureId);
	E->SetStringField(TEXT("libraryFixtureId"), LibraryFixtureId);
	E->SetStringField(TEXT("displayName"), DisplayName);
	E->SetBoolField(TEXT("hasPanTilt"), bHasPanTilt);
	E->SetBoolField(TEXT("hasGobo"), bHasGobo);
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
