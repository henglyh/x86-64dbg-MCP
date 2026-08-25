#include <windows.h>
#include <stdio.h>
#include <stdbool.h>

/* 模拟 x64dbg 的插件加载：LoadLibrary + 调用 pluginit/plugsetup */

typedef struct
{
    int pluginHandle;
    int sdkVersion;
    int pluginVersion;
    char pluginName[256];
} PLUG_INITSTRUCT;

typedef bool (*PFN_PLUGINIT)(PLUG_INITSTRUCT*);
typedef bool (*PFN_PLUGSETUP)(void*);
typedef bool (*PFN_PLUGSTOP)(void);

int main(void)
{
    SetDllDirectoryA("C:\\Users\\Administrator\\Desktop\\x\\release\\x32");

    HMODULE h = LoadLibraryA("C:\\Users\\Administrator\\Desktop\\x\\release\\x32\\plugins\\x64dbg_mcp.dp32");
    if(!h)
    {
        printf("LoadLibrary failed: %lu (0x%08lX)\n", GetLastError(), GetLastError());
        return 1;
    }
    printf("LoadLibrary OK: %p\n", (void*)h);

    PFN_PLUGINIT pluginit = (PFN_PLUGINIT)GetProcAddress(h, "pluginit");
    PFN_PLUGSETUP plugsetup = (PFN_PLUGSETUP)GetProcAddress(h, "plugsetup");
    PFN_PLUGSTOP plugstop = (PFN_PLUGSTOP)GetProcAddress(h, "plugstop");
    printf("pluginit=%p plugsetup=%p plugstop=%p\n",
           (void*)pluginit, (void*)plugsetup, (void*)plugstop);
    if(!pluginit || !plugsetup || !plugstop)
    {
        printf("MISSING EXPORTS!\n");
        return 2;
    }

    PLUG_INITSTRUCT init = {0};
    init.pluginHandle = 0;
    init.sdkVersion = 0;
    init.pluginVersion = 0;
    memset(init.pluginName, 0, sizeof(init.pluginName));

    printf("calling pluginit...\n");
    fflush(stdout);
    bool ok = pluginit(&init);
    printf("pluginit returned: %d (name='%s')\n", ok ? 1 : 0, init.pluginName);
    fflush(stdout);

    printf("calling plugsetup(NULL)...\n");
    fflush(stdout);
    bool ok2 = plugsetup(NULL);
    printf("plugsetup returned: %d\n", ok2 ? 1 : 0);
    fflush(stdout);

    printf("calling plugstop...\n");
    fflush(stdout);
    bool ok3 = plugstop();
    printf("plugstop returned: %d\n", ok3 ? 1 : 0);
    fflush(stdout);

    FreeLibrary(h);
    return 0;
}
