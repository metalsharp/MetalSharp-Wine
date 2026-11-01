// slverify: runs Streamline's own plugin signature check (sl_security.h, verifyEmbeddedSignature) on each file given,
// as sl.interposer does before it loads a plugin
#include <windows.h>
#include <cstdio>
#include "include/sl_security.h"

int wmain(int argc, wchar_t **argv)
{
    for (int i = 1; i < argc; i++)
        printf("%S: %s\n", argv[i], sl::security::verifyEmbeddedSignature(argv[i]) ? "loads" : "refused");
    return 0;
}
