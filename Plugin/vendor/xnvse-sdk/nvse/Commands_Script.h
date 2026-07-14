#pragma once

#include "CommandTable.h"
#include "ParamInfos.h"
#include "ScriptUtils.h"

DEFINE_COMMAND(IsScripted, returns 1 if the object or reference has a script attached to it., 0, 1, kParams_OneOptionalForm);
DEFINE_COMMAND(GetScript, returns the script of the reference or passed object., 0, 1, kParams_OneOptionalForm);
DEFINE_COMMAND(RemoveScript, removes the script of the reference or passed object., 0, 1, kParams_OneOptionalForm);
DEFINE_COMMAND(SetScript, sets the script of the reference or passed object., 0, 2, kParams_OneForm_OneOptionalForm);
DEFINE_COMMAND(IsFormValid, returns 1 if the reference or passed object is valid., 0, 1, kParams_OneOptionalForm);

static ParamInfo kParams_OneReference[1] =
{
	{ .typeStr = "reference", .typeID = kParamType_ObjectRef, .isOptional = 0 },
};

DEFINE_COMMAND(IsReference, returns 1 if the passed refVar is a reference., 0, 1, kParams_OneReference);

static ParamInfo kParams_GetVariable[2] =
{
	{ .typeStr = "variable name", .typeID = kParamType_String, .isOptional = 0 },
	{ .typeStr = "quest", .typeID = kParamType_Quest, .isOptional = 1 },
};

DEFINE_COMMAND(GetVariable, looks up the value of a variable by name, 0, 2, kParams_GetVariable);
DEFINE_COMMAND(HasVariable, returns true if the script has a variable with the specified name, 0, 2, kParams_GetVariable);
DEFINE_COMMAND(GetRefVariable, looks up the value of a ref variable by name, 0, 2, kParams_GetVariable);
DEFINE_CMD_ALT(
	GetArrayVariable,
	GetArrayVar,
	looks up an array variable by name on the calling object or specified quest,
	0,
	2,
	kParams_GetVariable
);
DEFINE_COMMAND(
	GetStringVariable,
	looks up a string variable by name on the calling object or specified quest,
	0,
	2,
	kParams_GetVariable
);

static ParamInfo kParams_SetNumVariable[3] =
{
	{ .typeStr = "variable name", .typeID = kParamType_String, .isOptional = 0 },
	{ .typeStr = "variable value", .typeID = kParamType_Float, .isOptional = 0 },
	{ .typeStr = "quest", .typeID = kParamType_Quest, .isOptional = 1 },
};

static ParamInfo kParams_SetRefVariable[3] =
{
	{ .typeStr = "variable name", .typeID = kParamType_String, .isOptional = 0 },
	{ .typeStr = "variable value", .typeID = kParamType_AnyForm, .isOptional = 0 },
	{ .typeStr = "quest", .typeID = kParamType_Quest, .isOptional = 1 },
};

static ParamInfo kParams_SetStrVariable[3] =
{
	{ .typeStr = "variable name", .typeID = kParamType_String, .isOptional = 0 },
	{ .typeStr = "variable value", .typeID = kParamType_String, .isOptional = 0 },
	{ .typeStr = "quest", .typeID = kParamType_Quest, .isOptional = 1 },
};

DEFINE_COMMAND(SetVariable, sets the value of a variable by name, 0, 3, kParams_SetNumVariable);
DEFINE_COMMAND(SetRefVariable, sets the value of a variable by name, 0, 3, kParams_SetRefVariable);
DEFINE_COMMAND(SetStringVariable, sets the value of a variable by name, 0, 3, kParams_SetStrVariable);

static ParamInfo kParams_CompareScripts[2] =
{
	{ .typeStr = "script", .typeID = kParamType_ObjectID, .isOptional = 0 },
	{ .typeStr = "script", .typeID = kParamType_ObjectID, .isOptional = 0 },
};

DEFINE_COMMAND(CompareScripts, returns true if the compiled scripts are identical, 0, 2, kParams_CompareScripts);

DEFINE_COMMAND(ResetAllVariables, sets all variables in a script to zero, 0, 0, NULL);

static ParamInfo kParams_GetFormFromMod[2] =
{
	{ .typeStr = "modName", .typeID = kParamType_String, .isOptional = 0 },
	{ .typeStr = "formID", .typeID = kParamType_String, .isOptional = 0 },
};

DEFINE_COMMAND(GetNumExplicitRefs, returns the number of literal references in a script, 0, 1, kParams_OneOptionalForm);

DEFINE_COMMAND(GetNthExplicitRef, returns the nth literal reference in a script, 0, 2, kParams_OneInt_OneOptionalForm);

DEFINE_COMMAND(RunScript, debug, 0, 1, kParams_OneForm);

DEFINE_COMMAND(GetCurrentScript, returns the calling script, 0, 0, NULL);
DEFINE_COMMAND(GetCallingScript, returns the script that called the executing function script, 0, 0, NULL);

static ParamInfo kNVSEParams_SetEventHandler[5] =
{
	{ .typeStr = "event name", .typeID = kNVSEParamType_String, .isOptional = 0 },
	{ .typeStr = "function script", .typeID = kNVSEParamType_Form, .isOptional = 0 },
	{ .typeStr = "filter", .typeID = kNVSEParamType_Pair, .isOptional = 1 },
	{ .typeStr = "filter", .typeID = kNVSEParamType_Pair, .isOptional = 1 },

	// an extra filter for "priority"
	{ .typeStr = "filter", .typeID = kNVSEParamType_Pair, .isOptional = 1 },
};

static ParamInfo kNVSEParams_SetEventHandlerAlt[19] =
{
	{ .typeStr = "event name", .typeID = kNVSEParamType_String, .isOptional = 0 },
	{ .typeStr = "function script", .typeID = kNVSEParamType_Form, .isOptional = 0 },

	{ .typeStr = "filter", .typeID = kNVSEParamType_Pair, .isOptional = 1 },
	{ .typeStr = "filter", .typeID = kNVSEParamType_Pair, .isOptional = 1 },
	{ .typeStr = "filter", .typeID = kNVSEParamType_Pair, .isOptional = 1 },
	{ .typeStr = "filter", .typeID = kNVSEParamType_Pair, .isOptional = 1 },
	{ .typeStr = "filter", .typeID = kNVSEParamType_Pair, .isOptional = 1 },

	{ .typeStr = "filter", .typeID = kNVSEParamType_Pair, .isOptional = 1 },
	{ .typeStr = "filter", .typeID = kNVSEParamType_Pair, .isOptional = 1 },
	{ .typeStr = "filter", .typeID = kNVSEParamType_Pair, .isOptional = 1 },
	{ .typeStr = "filter", .typeID = kNVSEParamType_Pair, .isOptional = 1 },
	{ .typeStr = "filter", .typeID = kNVSEParamType_Pair, .isOptional = 1 },

	{ .typeStr = "filter", .typeID = kNVSEParamType_Pair, .isOptional = 1 },
	{ .typeStr = "filter", .typeID = kNVSEParamType_Pair, .isOptional = 1 },
	{ .typeStr = "filter", .typeID = kNVSEParamType_Pair, .isOptional = 1 },
	{ .typeStr = "filter", .typeID = kNVSEParamType_Pair, .isOptional = 1 },
	{ .typeStr = "filter", .typeID = kNVSEParamType_Pair, .isOptional = 1 },

	{ .typeStr = "filter", .typeID = kNVSEParamType_Pair, .isOptional = 1 },
	{ .typeStr = "filter", .typeID = kNVSEParamType_Pair, .isOptional = 1 },
	// 1 filter for thisObj (0::SomeFilter), 1 for "priority", 15 for the rest.
};


DEFINE_COMMAND_EXP(
	SetEventHandler,
	"defines a function script to serve as a callback for game events",
	0,
	kNVSEParams_SetEventHandler
);
DEFINE_COMMAND_EXP(
	SetEventHandlerAlt,
	"Uses the new event filtering system.",
	0,
	kNVSEParams_SetEventHandlerAlt
);

DEFINE_COMMAND_EXP(
	RemoveEventHandler,
	"removes event handlers matching the event, script, and optional filters specified",
	0,
	kNVSEParams_SetEventHandler
);
DEFINE_CMD(GetCurrentEventName, returns the name of the event currently being processed by an event handler, 0, NULL);

static ParamInfo kNVSEParams_DispatchEvent[3] =
{
	{ .typeStr = "eventName", .typeID = kNVSEParamType_String, .isOptional = 0 },
	{ .typeStr = "args", .typeID = kNVSEParamType_Array, .isOptional = 1 },
	{ .typeStr = "sender", .typeID = kNVSEParamType_String, .isOptional = 1 }
};
static ParamInfo kNVSEParams_DispatchEventAlt[16] =
{
	{ .typeStr = "eventName", .typeID = kNVSEParamType_String, .isOptional = 0 },

	{ .typeStr = "arg1", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "arg2", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "arg3", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "arg4", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "arg5", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },

	{ .typeStr = "arg6", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "arg7", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "arg8", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "arg9", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "arg10", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },

	{ .typeStr = "arg11", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "arg12", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "arg13", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "arg14", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "arg15", .typeID = kNVSEParamType_BasicType, .isOptional = 1 }
};

DEFINE_COMMAND_EXP(DispatchEvent, dispatches a user - defined event to any registered listeners, 0, kNVSEParams_DispatchEvent);
DEFINE_COMMAND_EXP(DispatchEventAlt, dispatches a user - defined event to any registered listeners, 0, kNVSEParams_DispatchEventAlt);

static ParamInfo kNVSEParams_DumpEventHandlers[18] =
{
	{ .typeStr = "eventName", .typeID = kNVSEParamType_String, .isOptional = 1 },
	{ .typeStr = "script", .typeID = kNVSEParamType_Form, .isOptional = 1 },
	{ .typeStr = "priority", .typeID = kNVSEParamType_Number, .isOptional = 1 },

	{ .typeStr = "arg1", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "arg2", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "arg3", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "arg4", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "arg5", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },

	{ .typeStr = "arg6", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "arg7", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "arg8", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "arg9", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "arg10", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },

	{ .typeStr = "arg11", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "arg12", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "arg13", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "arg14", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "arg15", .typeID = kNVSEParamType_BasicType, .isOptional = 1 }
};

DEFINE_COMMAND_EXP(
	DumpEventHandlers,
	"dumps event handlers, optionally filtered by eventName, script and args.",
	0,
	kNVSEParams_DumpEventHandlers
);
DEFINE_COMMAND_EXP(
	GetEventHandlers,
	"returns a multi-dimensional array of event handlers, optionally filtered by eventName, script and args.",
	0,
	kNVSEParams_DumpEventHandlers
);


static ParamInfo kNVSEParams_GetHigherOrLowerPriorityEventHandlers[] =
{
	{ .typeStr = "eventName", .typeID = kNVSEParamType_String, .isOptional = 0 },
	{ .typeStr = "script", .typeID = kNVSEParamType_Form, .isOptional = 0 },
	{ .typeStr = "priority", .typeID = kNVSEParamType_Number, .isOptional = 0 },
	// optional args
	{ .typeStr = "scriptsToIgnore", .typeID = kNVSEParamType_Form, .isOptional = 1 },
	{ .typeStr = "pluginsToIgnore", .typeID = kNVSEParamType_Array, .isOptional = 1 },
	{ .typeStr = "pluginHandlersToIgnore", .typeID = kNVSEParamType_Array, .isOptional = 1 }
};

DEFINE_COMMAND_EXP(GetHigherPriorityEventHandlers, "", 0, kNVSEParams_GetHigherOrLowerPriorityEventHandlers);
DEFINE_COMMAND_EXP(GetLowerPriorityEventHandlers, "", 0, kNVSEParams_GetHigherOrLowerPriorityEventHandlers);

DEFINE_COMMAND_EXP(IsEventHandlerFirst, "", 0, kNVSEParams_GetHigherOrLowerPriorityEventHandlers);
DEFINE_COMMAND_EXP(IsEventHandlerLast, "", 0, kNVSEParams_GetHigherOrLowerPriorityEventHandlers);


static ParamInfo kParams_CallAfter_OLD[3] =
{
	{ .typeStr = "seconds", .typeID = kParamType_Float, .isOptional = 0 },
	{ .typeStr = "function", .typeID = kParamType_AnyForm, .isOptional = 0 },
	{ .typeStr = "runs in menumode", .typeID = kParamType_Integer, .isOptional = 1 }
};

static ParamInfo kNVSEParams_CallAfter[18] =
{
	{ .typeStr = "seconds", .typeID = kNVSEParamType_Number, .isOptional = 0 },
	{ .typeStr = "function", .typeID = kNVSEParamType_Form, .isOptional = 0 },
	{ .typeStr = "flags", .typeID = kNVSEParamType_Number, .isOptional = 1 },

	{ .typeStr = "element", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "element", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "element", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "element", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "element", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },

	{ .typeStr = "element", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "element", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "element", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "element", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "element", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },

	{ .typeStr = "element", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "element", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "element", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "element", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "element", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	//#elems should not exceed max # of UDF args.
};

static ParamInfo kParams_CallWhile_OLD[2] =
{
	{ .typeStr = "function", .typeID = kParamType_AnyForm, .isOptional = 0 },
	{ .typeStr = "condition", .typeID = kParamType_AnyForm, .isOptional = 0 },
};

static ParamInfo kNVSEParams_CallWhile[18] =
{
	{ .typeStr = "function", .typeID = kNVSEParamType_Form, .isOptional = 0 },
	{ .typeStr = "condition", .typeID = kNVSEParamType_Form, .isOptional = 0 },
	{ .typeStr = "flags", .typeID = kNVSEParamType_Number, .isOptional = 1 },

	{ .typeStr = "element", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "element", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "element", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "element", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "element", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },

	{ .typeStr = "element", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "element", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "element", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "element", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "element", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },

	{ .typeStr = "element", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "element", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "element", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "element", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "element", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	//#elems should not exceed max # of UDF args.
};

DEFINE_CMD_ALT(CallAfterSeconds_OLD, CallAfter_OLD, "deprecated", 0, std::size(kParams_CallAfter_OLD), kParams_CallAfter_OLD);
DEFINE_CMD_ALT(CallForSeconds_OLD, CallFor_OLD, "deprecated", 0, std::size(kParams_CallAfter_OLD), kParams_CallAfter_OLD);
DEFINE_COMMAND(CallWhile_OLD, "deprecated", 0, std::size(kParams_CallWhile_OLD), kParams_CallWhile_OLD);
DEFINE_COMMAND(CallWhen_OLD, "deprecated", 0, std::size(kParams_CallWhile_OLD), kParams_CallWhile_OLD);

DEFINE_CMD_ALT_EXP(CallAfterSeconds, CallAfter, "calls UDF after argument number of seconds", false, kNVSEParams_CallAfter);
DEFINE_COMMAND_EXP(CallAfterFrames, "calls UDF after argument number of frame", false, kNVSEParams_CallAfter);
DEFINE_CMD_ALT_EXP(CallForSeconds, CallFor, "calls UDF each frame for argument number of seconds", false, kNVSEParams_CallAfter);
DEFINE_COMMAND_EXP(CallWhile, "calls UDF each frame while condition is met", false, kNVSEParams_CallWhile);
DEFINE_COMMAND_EXP(CallWhen, "calls UDF once when a condition is met which is polled each frame", false, kNVSEParams_CallWhile);

static ParamInfo kNVSEParams_DelayedCallWhile[19] =
{
	{ .typeStr = "seconds", .typeID = kNVSEParamType_Number, .isOptional = 0 },
	{ .typeStr = "function", .typeID = kNVSEParamType_Form, .isOptional = 0 },
	{ .typeStr = "condition", .typeID = kNVSEParamType_Form, .isOptional = 0 },
	{ .typeStr = "flags", .typeID = kNVSEParamType_Number, .isOptional = 1 },

	{ .typeStr = "element", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "element", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "element", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "element", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "element", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },

	{ .typeStr = "element", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "element", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "element", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "element", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "element", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },

	{ .typeStr = "element", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "element", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "element", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "element", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "element", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	//#elems should not exceed max # of UDF args.
};

DEFINE_COMMAND_EXP(
	CallWhilePerSeconds,
	"calls UDF every couple of seconds, until the condition UDF returns false.",
	false,
	kNVSEParams_DelayedCallWhile
);


#if RUNTIME
using CallArgs = std::vector<SelfOwningArrayElement>;

struct DelayedCallInfo {
	Script* script;
	float time;
	TESObjectREFR* thisObj;
	LambdaManager::LambdaVariableContext lambdaVariableContext;

	enum Mode : UInt8 {
		kMode_RunInGameModeOnly = 0,
		kMode_AlsoRunInMenuMode = 1,
		kMode_AlsoDontRunWhilePaused = 2, // won't run when paused (main menu, pause menu, console menu).
	} mode;

	CallArgs args;

	[[nodiscard]] bool ShouldRun(bool isMenuMode, bool isPaused) {
		if (isMenuMode && mode == kMode_RunInGameModeOnly)
			return false;
		if (isPaused && mode >= kMode_AlsoDontRunWhilePaused)
			return false;
		return true;
	}

	DelayedCallInfo(Script* script, float time, TESObjectREFR* thisObj, Mode mode, CallArgs&& args = {}) : script(script), time(time),
		thisObj(thisObj),
		lambdaVariableContext(script), mode(mode),
		args(std::move(args)) {
	}
};

struct CallWhileInfo {
	Script* callFunction;
	Script* condition;
	TESObjectREFR* thisObj;
	LambdaManager::LambdaVariableContext callFnLambdaCtx;
	LambdaManager::LambdaVariableContext condFnLambdaCtx;

	enum eFlags : UInt8 {
		kFlags_None = 0,
		kPassArgs_ToCallFunc = 1 << 0,
		kPassArgs_ToConditionFunc = 1 << 1,

		// Runs in both MenuMode and GameMode by default.
		kFlag_DontRunInMenuMode = 1 << 2,
		kFlag_DontRunInGameMode = 1 << 3,

		// Runs while paused by default
		kFlag_DontRunWhilePaused = 1 << 4,
	} flags;

	CallArgs args;

	[[nodiscard]] bool PassArgsToCallFunc() const {
		return flags & kPassArgs_ToCallFunc;
	}

	[[nodiscard]] bool PassArgsToCondFunc() const {
		return flags & kPassArgs_ToConditionFunc;
	}

	[[nodiscard]] bool ShouldRun(bool isMenuMode, bool isPaused) {
		if (isMenuMode && (flags & kFlag_DontRunInMenuMode))
			return false;
		if (!isMenuMode && (flags & kFlag_DontRunInGameMode))
			return false;
		if (isPaused && (flags & kFlag_DontRunWhilePaused))
			return false;
		return true;
	}

	CallWhileInfo(Script* callFunction,
				  Script* condition,
				  TESObjectREFR* thisObj,
				  eFlags flags,
				  CallArgs&& args = {}) : callFunction(callFunction), condition(condition), thisObj(thisObj),
										  callFnLambdaCtx(callFunction), condFnLambdaCtx(condition), flags(flags),
										  args(std::move(args)) {
	}
};

struct DelayedCallWhileInfo {
	float interval, oldTime;
	Script* callFunction;
	Script* condition;
	TESObjectREFR* thisObj;
	LambdaManager::LambdaVariableContext callFnLambdaCtx;
	LambdaManager::LambdaVariableContext condFnLambdaCtx;

	enum eFlags : UInt8 {
		kFlags_None = 0,
		kPassArgs_ToCallFunc = 1 << 0,
		kPassArgs_ToConditionFunc = 1 << 1,

		// Runs in both MenuMode and GameMode by default.
		kFlag_DontRunInMenuMode = 1 << 2,
		kFlag_DontRunInGameMode = 1 << 3,

		// Runs while paused by default
		kFlag_DontRunWhilePaused = 1 << 4,
	} flags;

	CallArgs args;

	[[nodiscard]] bool PassArgsToCallFunc() const {
		return flags & kPassArgs_ToCallFunc;
	}

	[[nodiscard]] bool PassArgsToCondFunc() const {
		return flags & kPassArgs_ToConditionFunc;
	}

	[[nodiscard]] bool ShouldRun(bool isMenuMode, bool isPaused) {
		if (isMenuMode && (flags & kFlag_DontRunInMenuMode))
			return false;
		if (!isMenuMode && (flags & kFlag_DontRunInGameMode))
			return false;
		if (isPaused && (flags & kFlag_DontRunWhilePaused))
			return false;
		return true;
	}

	DelayedCallWhileInfo(float interval,
						 float oldTime,
						 Script* callFunction,
						 Script* condition,
						 TESObjectREFR* thisObj,
						 eFlags flags,
						 CallArgs&& args = {}) : interval(interval), oldTime(oldTime), callFunction(callFunction), condition(condition),
												 thisObj(thisObj), callFnLambdaCtx(callFunction), condFnLambdaCtx(condition),
												 flags(flags), args(std::move(args)) {
	}
};

extern std::list<DelayedCallInfo> g_callForInfos;
extern std::list<CallWhileInfo> g_callWhileInfos;
extern std::list<DelayedCallInfo> g_callAfterInfos;
extern std::list<CallWhileInfo> g_callWhenInfos;
extern std::list<DelayedCallWhileInfo> g_callWhilePerSecondsInfos;
extern std::list<DelayedCallInfo> g_callAfterFramesInfos;

void ClearDelayedCalls();

extern ICriticalSection g_callForInfosCS;
extern ICriticalSection g_callWhileInfosCS;
extern ICriticalSection g_callAfterInfosCS;
extern ICriticalSection g_callWhenInfosCS;
extern ICriticalSection g_callWhilePerSecondsInfosCS;
extern ICriticalSection g_callAfterFramesInfosCS;

#endif

static ParamInfo kParams_HasScriptCommand[3] =
{
	{ .typeStr = "command opcode", .typeID = kParamType_Integer, .isOptional = 0 },
	{ .typeStr = "form", .typeID = kParamType_AnyForm, .isOptional = 1 },
	{ .typeStr = "event block id", .typeID = kParamType_Integer, .isOptional = 1 }
};


DEFINE_COMMAND(DecompileScript, decompiles a script to file, false, 2, kParams_OneForm_OneOptionalString);
DEFINE_COMMAND(HasScriptCommand, returns 1 if script contains call to a command, false, 3, kParams_HasScriptCommand);
DEFINE_COMMAND(GetCommandOpcode, gets opcode for command name, false, 1, kParams_OneString);
DEFINE_CMD_ALIAS(
	DumpCommandWikiDoc,
	DumpWikiDoc,
	dumps wiki - style documentation for a command,
	false,
	kParams_OneString
);
DEFINE_CMD_ALIAS(
	DumpCommandWikiDocs,
	DumpWikiDocs,
	dumps wiki - style documentation for multiple commands,
	false,
	kParams_TwoInts_OneOptionalString
);


static ParamInfo kNVSEParams_TernaryUDF[] =
{
	{ .typeStr = "value", .typeID = kNVSEParamType_BasicType, .isOptional = 0 },
	{ .typeStr = "callIfTrue (UDF)", .typeID = kNVSEParamType_Form, .isOptional = 0 },
	{ .typeStr = "callIfFalse (UDF)", .typeID = kNVSEParamType_Form, .isOptional = 0 },
};
DEFINE_COMMAND_EXP(TernaryUDF, "The ternary operator as a function, calling one of 2 UDFs.", false, kNVSEParams_TernaryUDF);

static ParamInfo kNVSEParams_Ternary[] =
{
	{ .typeStr = "condition", .typeID = kNVSEParamType_Boolean, .isOptional = 0 },
	{ .typeStr = "resultIfTrue", .typeID = kNVSEParamType_NoTypeCheck, .isOptional = 0 },
	{ .typeStr = "resultIfFalse", .typeID = kNVSEParamType_NoTypeCheck, .isOptional = 0 },
};
DEFINE_COMMAND_EXP(Ternary, "The ternary operator as a function, for real this time", false, kNVSEParams_Ternary);


DEFINE_COMMAND(GetSoldItemInvRef, Returns the invRef for the currently sold item(in OnSell event), false, 0, nullptr);

static ParamInfo kNVSEParams_OneString_OneOptionalBool[] =
{
	{ .typeStr = "string", .typeID = kNVSEParamType_String, .isOptional = 0 },
	{ .typeStr = "bool", .typeID = kNVSEParamType_Boolean, .isOptional = 1 },
};

DEFINE_CMD_ALT_EXP(
	CompileScript,
	GetUDFFromFile,
	"Returns a compiled script from a file, to call as a UDF.",
	false,
	kNVSEParams_OneString_OneOptionalBool
);

static ParamInfo kNVSEParams_MatchesAnyOf[] =
{
	{ .typeStr = "valueToMatch", .typeID = kNVSEParamType_BasicType, .isOptional = 0 },

	{ .typeStr = "value", .typeID = kNVSEParamType_BasicType, .isOptional = 0 }, // intentionally non-optional
	{ .typeStr = "value", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "value", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "value", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "value", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },

	{ .typeStr = "value", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "value", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "value", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "value", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "value", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },

	{ .typeStr = "value", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "value", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "value", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "value", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "value", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },

	{ .typeStr = "value", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "value", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "value", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
	{ .typeStr = "value", .typeID = kNVSEParamType_BasicType, .isOptional = 1 },
};

DEFINE_CMD_ALT_EXP(
	MatchesAnyOf, ,
	"Returns true/false if the first value matches any of the other values.",
	false,
	kNVSEParams_MatchesAnyOf
);

static ParamInfo kNVSEParams_Jmp_If_True[] =
{
	{ .typeStr = "", .typeID = kNVSEParamType_Boolean, .isOptional = 0 },
	{ .typeStr = "", .typeID = kNVSEParamType_Number, .isOptional = 0 },
};
DEFINE_COMMAND_EXP(Jmp_If_True, nullptr, false, kNVSEParams_Jmp_If_True)
DEFINE_COMMAND_EXP(Jmp_If_False, nullptr, false, kNVSEParams_Jmp_If_True)
DEFINE_COMMAND_EXP(Jmp, nullptr, false, kParams_OneInt)
