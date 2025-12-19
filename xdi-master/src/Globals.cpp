#include "Globals.h"
#include "f4se_common/Relocation.h"
#include "f4se_common/f4se_version.h"

#define GET_RVA(relocPtr) relocPtr.GetUIntPtr() - RelocationManager::s_baseAddr

/*
This file makes globals version-independent.

Initialization order is important for this file.

Since RelocPtrs are static globals with constructors they are initialized during the dynamic initialization phase.
Static initialization order is undefined for variables in different translation units, so we can't obtain the value of a RelocPtr during static init.

Initialization must thus be done explicitly:
Call G::Init() in the plugin load routine before calling RVAManager::UpdateAddresses().

Doing so ensures that all RelocPtrs have been initialized and can be used to initialize an RVA.
*/

#include "Config.h"
#include "GameUtils.h"

#include "f4se/GameRTTI.h"
#include "f4se/GameData.h"
#include "f4se/GameMenus.h"
#include "f4se/PapyrusVM.h"
#include "f4se/GameReferences.h"
#include "f4se/GameSettings.h"

namespace G
{
    RVA<UI*>                        ui;
    RVA<GameVM*>                    gameVM;
    RVA<DataHandler*>               dataHandler;
    RVA<PlayerCharacter*>           player;
    RVA<INISettingCollection*>      iniSettings;
    RVA<INIPrefSettingCollection*>  iniPrefSettings;

    std::vector<BGSKeyword*>        activationKeywords;
    std::vector<TESGlobal*>         resultGlobals;

    void Init()
    {
        ui                  = RVA<UI*>                          (GET_RVA(g_ui),                 "48 8B 0D ? ? ? ? BA ? ? ? ? 8B 1C 16", 0, 3, 7); // vr
        gameVM              = RVA<GameVM*>                      (GET_RVA(g_gameVM),             "4C 8B 05 ? ? ? ? 48 8B F9", 0, 3, 7); //vr
        dataHandler         = RVA<DataHandler*>                 (GET_RVA(g_dataHandler),        "48 8B 05 ? ? ? ? 8B 13", 0, 3, 7); // vr
        player              = RVA<PlayerCharacter*>             (GET_RVA(g_player),             "48 8B 0D ? ? ? ? E8 ? ? ? ? 48 3B C3 75 0C", 0, 3, 7); // vr
        iniSettings         = RVA<INISettingCollection*>        (GET_RVA(g_iniSettings),        "48 8B 0D ? ? ? ? 48 8D 15 ? ? ? ? E8 ? ? ? ? 48 8B D8", 0, 3, 7); //vr
        iniPrefSettings     = RVA<INIPrefSettingCollection*>    (GET_RVA(g_iniPrefSettings),    "48 8B 1D ? ? ? ? E8 ? ? ? ? 4C 8D 43 08 48 8B 1D ? ? ? ? 4D 85 C0 74 16 48 8D 4B 08 4C 3B C1 74 11 BA 04 01 00 00 FF 15 ? ? ? ? EB 04 44 88 73 08", 0, 3, 7); //vr
    }

    // mov     rcx, cs:qq_g_ui
    // mov     r8, cs:qq_g_gameVM
    // mov     rax, cs:qq_g_dataHandler
    // mov     rcx, cs:qq_g_player
    // mov     rcx, cs:qq_g_iniSettings
    // mov     rdi, cs:qq_g_iniPrefSettings

    void OnDataLoaded()
    {
        activationKeywords.clear();
        resultGlobals.clear();

        _MESSAGE("Loading plugin data...");

        // XDI Forms
        RegisterForm(G::activationKeywords, GAME_PLUGIN_NAME, GAME_ACTIVATION_KEYWORD_ID);
        RegisterForm(G::resultGlobals, GAME_PLUGIN_NAME, GAME_RESULT_ID);

        // Plugin forms
        char searchString[MAX_PATH] = INI_LOCATION_PLUGINS;
        strcat_s(searchString, "*.ini");
        HANDLE hFind; WIN32_FIND_DATA data;
        hFind = FindFirstFile(searchString, &data);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                char iniPath[MAX_PATH] = INI_LOCATION_PLUGINS;
                strcat_s(iniPath, data.cFileName);

                char sPluginName[MAX_PATH] = "";
                char sActivationKeyword[MAX_PATH] = "";
                char sResultGlobal[MAX_PATH] = "";
                GetPrivateProfileString("XDI", "Plugin", NULL, sPluginName, sizeof(sPluginName), iniPath);
                GetPrivateProfileString("XDI", "ActivationKeyword", NULL, sActivationKeyword, sizeof(sActivationKeyword), iniPath);
                GetPrivateProfileString("XDI", "ResultGlobal", NULL, sResultGlobal, sizeof(sResultGlobal), iniPath);

                if (strlen(sPluginName) == 0) continue;

                if (sActivationKeyword) {
                    UInt32 formID = strtoul(sActivationKeyword, nullptr, 16);
                    RegisterForm(G::activationKeywords, sPluginName, formID);
                }

                if (sResultGlobal) {
                    UInt32 formID = strtoul(sResultGlobal, nullptr, 16);
                    RegisterForm(G::resultGlobals, sPluginName, formID);
                }

            } while (FindNextFile(hFind, &data));
            FindClose(hFind);
        }

        // Report status
        if (activationKeywords.size() > 0) {
            _MESSAGE("Plugin data loaded.");
        } else {
            _MESSAGE("FATAL: Failed to load plugin data!");
        }
    }
}

