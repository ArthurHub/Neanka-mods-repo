#include <shlobj.h>
#include <xbyak/xbyak.h>

#include "f4se/PluginAPI.h"

#include "f4se_common/SafeWrite.h"
#include "f4se_common/BranchTrampoline.h"
#include "f4se/GameReferences.h"
#include "f4se/GameRTTI.h"

#include "Config.h"
#include "rva/RVA.h"
#include "Globals.h"
#include "GameUtils.h"

#include "ExtraTypes.h"
#include "Scaleform.h"
#include "Papyrus.h"
#include "DialogueEx.h"
#include "f4se/GameMenus.h"

#define DEBUG _DEBUG
#if DEBUG
#include "Debug.h"
#endif

IDebugLog gLog;
PluginHandle g_pluginHandle = kPluginHandle_Invalid;

F4SEScaleformInterface *g_scaleform = NULL;
F4SEPapyrusInterface   *g_papyrus   = NULL;
F4SEMessagingInterface *g_messaging = NULL;



// vr input

_DialogueMenu__ShouldHandleEvent DialogueMenu__ShouldHandleEvent_original;

void ProcessUserEvent(const char * controlName, bool isDown, int deviceType, UInt32 keyCode)
{
    BSFixedString mainMenuStr("DialogueMenu");
    if ((*G::ui)->IsMenuOpen("DialogueMenu")) {
        //_MESSAGE("DialogueMenu opened");
        IMenu* menu = (*G::ui)->GetMenu(mainMenuStr);
        GFxMovieRoot* movieRoot = menu->movie->movieRoot;
        GFxValue args[4];
        args[0].SetString(controlName);
        args[1].SetBool(isDown);
        args[2].SetInt(deviceType);
        args[3].SetInt(keyCode);
        movieRoot->Invoke("root.ProcessUserEventEx", nullptr, args, 4);
    }
}

class F4SEInputHandler : public BSInputEventUser
{
public:
    F4SEInputHandler() : BSInputEventUser(true) { }

    virtual void OnThumbstickEvent(ThumbstickEvent * inputEvent)
    {
        if (inputEvent->stick == 0xC && inputEvent->previousDirection != inputEvent->direction)
        {
            _MESSAGE("OnThumbstickEvent move %i", inputEvent->direction);
            switch (inputEvent->direction)
            {
            case 1: ProcessUserEvent(BSFixedString("Forward"), true, 0, 38);
                break;
            case 2: ProcessUserEvent(BSFixedString("StrafeRight"), true, 0, 39);
                break;
            case 3: ProcessUserEvent(BSFixedString("Back"), true, 0, 40);
                break;
            case 4: ProcessUserEvent(BSFixedString("StrafeLeft"), true, 0, 37);
                break;
            default:
                break;
            }
        }
    }

    virtual void OnButtonEvent(ButtonEvent * inputEvent)
    {
        UInt32	keyCode;
        UInt32	deviceType = inputEvent->deviceType;
        UInt32	keyMask = inputEvent->keyMask;

        //_MESSAGE("OnButtonEvent");
        //_MESSAGE("deviceType %i, keymask %i", deviceType,  keyMask);
        //_MESSAGE("control %s", inputEvent->GetControlID()->c_str());

        /*if (deviceType == InputEvent::kDeviceType_Mouse) {
            // Mouse
            keyCode = InputMap::kMacro_MouseButtonOffset + keyMask;
        }
        else if (deviceType == InputEvent::kDeviceType_Gamepad) {
            // Gamepad
            keyCode = InputMap::GamepadMaskToKeycode(keyMask);
        }
        else {
            // Keyboard
            keyCode = keyMask;
        }*/
        keyCode = keyMask;

        float timer = inputEvent->timer;
        bool  isDown = inputEvent->isDown == 1.0f && timer == 0.0f;
        bool  isUp = inputEvent->isDown == 0.0f && timer != 0.0f;

        BSFixedString* control = inputEvent->GetControlID();

        if (isDown) {
            ProcessUserEvent(control->c_str(), true, deviceType, keyCode);
        }
        else if (isUp) {
            ProcessUserEvent(control->c_str(), false, deviceType, keyCode);
        }
    }
};
F4SEInputHandler g_scaleformInputHandler;

void RegisterForInput(bool bRegister) {
    if (bRegister) {
        g_scaleformInputHandler.enabled = true;
        tArray<BSInputEventUser*>* inputEvents = &((*g_menuControls)->inputEvents);
        BSInputEventUser* inputHandler = &g_scaleformInputHandler;
        int idx = inputEvents->GetItemIndex(inputHandler);
        if (idx == -1) {
            inputEvents->Push(&g_scaleformInputHandler);
            _MESSAGE("Registered for input events.");
        }
    }
    else {
        g_scaleformInputHandler.enabled = false;
    }
}

// end of vr input

//-------------------------
// Event Handlers
//-------------------------

void OnF4SEMessage(F4SEMessagingInterface::Message* msg) {
    switch (msg->type) {
        case F4SEMessagingInterface::kMessage_GameDataReady:
            G::OnDataLoaded();
            break;
        case F4SEMessagingInterface::kMessage_GameLoaded:
            DialogueEx::OnGameLoaded();
            RegisterForInput(true);
            break;
    }
}

//-------------------------
// F4SE Init
//-------------------------

extern "C"
{

bool F4SEPlugin_Query(const F4SEInterface * f4se, PluginInfo * info)
{
    char logPath[MAX_PATH];
    sprintf_s(logPath, sizeof(logPath), "\\My Games\\Fallout4VR\\F4SE\\%s.log", PLUGIN_NAME_SHORT);
    gLog.OpenRelative(CSIDL_MYDOCUMENTS, logPath);

    _MESSAGE("%s v%s", PLUGIN_NAME_SHORT, PLUGIN_VERSION_STRING);
    _MESSAGE("%s query", PLUGIN_NAME_SHORT);

    // populate info structure
    info->infoVersion = PluginInfo::kInfoVersion;
    info->name    = PLUGIN_NAME_SHORT;
    info->version = PLUGIN_VERSION;

    // store plugin handle so we can identify ourselves later
    g_pluginHandle = f4se->GetPluginHandle();

    // Check game version
    /*
	if (!COMPATIBLE(f4se->runtimeVersion)) {
		char str[512];
		sprintf_s(str, sizeof(str), "Your game version: v%d.%d.%d.%d\nExpected version: v%d.%d.%d.%d\n%s will be disabled.",
			GET_EXE_VERSION_MAJOR(f4se->runtimeVersion),
			GET_EXE_VERSION_MINOR(f4se->runtimeVersion),
			GET_EXE_VERSION_BUILD(f4se->runtimeVersion),
			GET_EXE_VERSION_SUB(f4se->runtimeVersion),
			GET_EXE_VERSION_MAJOR(SUPPORTED_RUNTIME_VERSION),
			GET_EXE_VERSION_MINOR(SUPPORTED_RUNTIME_VERSION),
			GET_EXE_VERSION_BUILD(SUPPORTED_RUNTIME_VERSION),
			GET_EXE_VERSION_SUB(SUPPORTED_RUNTIME_VERSION),
			PLUGIN_NAME_LONG
		);

		MessageBox(NULL, str, PLUGIN_NAME_LONG, MB_OK | MB_ICONEXCLAMATION);
		return false;
	}
    */
    
    if (f4se->runtimeVersion > SUPPORTED_RUNTIME_VERSION) {
        _MESSAGE("INFO: Newer game version (%08X) than target (%08X).", f4se->runtimeVersion, SUPPORTED_RUNTIME_VERSION);
    }

    // Get the scaleform interface
    g_scaleform = (F4SEScaleformInterface *)f4se->QueryInterface(kInterface_Scaleform);
    if (!g_scaleform) {
        _MESSAGE("couldn't get scaleform interface");
        return false;
    }

    // Get the papyrus interface
    g_papyrus = (F4SEPapyrusInterface *)f4se->QueryInterface(kInterface_Papyrus);
    if (!g_papyrus) {
        _MESSAGE("couldn't get papyrus interface");
        return false;
    }

    // Get the messaging interface
    g_messaging = (F4SEMessagingInterface *)f4se->QueryInterface(kInterface_Messaging);
    if (!g_messaging) {
        _MESSAGE("couldn't get messaging interface");
        return false;
    }

	return true;
}

bool F4SEPlugin_Load(const F4SEInterface *f4se)
{
    _MESSAGE("%s load", PLUGIN_NAME_SHORT);

    G::Init();
    RVAManager::UpdateAddresses(f4se->runtimeVersion);

    if (!g_localTrampoline.Create(1024 * 64, nullptr)) {
        _ERROR("couldn't create codegen buffer. this is fatal. skipping remainder of init process.");
        return false;
    }

    if (!g_branchTrampoline.Create(1024 * 64)) {
        _ERROR("couldn't create branch trampoline. this is fatal. skipping remainder of init process.");
        return false;
    }

    // Register Scaleform handlers
    g_scaleform->Register(PLUGIN_NAME_SHORT, Scaleform::RegisterScaleform);

    // Register Papyrus native functions
    g_papyrus->Register(Papyrus::RegisterPapyrus);

    // Register for F4SE messages
    g_messaging->RegisterListener(g_pluginHandle, "F4SE", OnF4SEMessage);

    // Patch game memory
    DialogueEx::Init();

    // VR input

    {
        struct DialogueMenu__ShouldHandleEvent_Code : Xbyak::CodeGenerator {
            DialogueMenu__ShouldHandleEvent_Code(void * buf) : Xbyak::CodeGenerator(4096, buf)
            {
                Xbyak::Label retnLabel;

                mov(ptr[rsp + 8], rbx);
                jmp(ptr[rip + retnLabel]);

                L(retnLabel);
                dq(DialogueMenu__ShouldHandleEvent.GetUIntPtr() + 5);
            }
        };

        void * codeBuf = g_localTrampoline.StartAlloc();
        DialogueMenu__ShouldHandleEvent_Code code(codeBuf);
        g_localTrampoline.EndAlloc(code.getCurr());

        DialogueMenu__ShouldHandleEvent_original = (_DialogueMenu__ShouldHandleEvent)codeBuf;

        g_branchTrampoline.Write5Branch(DialogueMenu__ShouldHandleEvent.GetUIntPtr(), (uintptr_t)DialogueMenu__ShouldHandleEvent_Hook);
    }
    
    // ond of VR input
    
    #if DEBUG
        Debug::Init();
    #endif

	return true;
}

};
