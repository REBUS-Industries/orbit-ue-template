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

namespace
{
	const TCHAR* RebusResponseType = TEXT("Response");
	const TCHAR* RebusUIInteraction = TEXT("UIInteraction");

	// The UIInteraction payload is the descriptor string encoded as UTF-16. Read whatever is
	// left in the reader and reinterpret it as UTF-16 code units.
	FString ReadDescriptorString(FMemoryReader& Ar)
	{
		const int64 Remaining = Ar.TotalSize() - Ar.Tell();
		if (Remaining <= 1) return FString();

		const int32 NumUnits = (int32)(Remaining / sizeof(UTF16CHAR));
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
			const FString Descriptor = ReadDescriptorString(Ar);
			if (Descriptor.IsEmpty()) return;

			// The message handler can fire off the game thread; route on the game thread.
			AsyncTask(ENamedThreads::GameThread, [WeakSelf, Descriptor]()
			{
				if (TSharedPtr<FRebusDataChannel> Self = WeakSelf.Pin())
				{
					Self->HandleDescriptor(Descriptor);
				}
			});
		});

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
		return;
	}

	FString Out;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Event, Writer);
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
	SendEvent(E);
}
