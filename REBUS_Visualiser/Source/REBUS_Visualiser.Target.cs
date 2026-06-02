// Copyright REBUS Industries. Minimal Game target so the visualiser project can
// compile its in-tree plugins (OrbitConnector + glTFRuntime). Added to give the
// content-only template a code module; non-destructive.
using UnrealBuildTool;
using System.Collections.Generic;

public class REBUS_VisualiserTarget : TargetRules
{
	public REBUS_VisualiserTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("REBUS_Visualiser");
	}
}
