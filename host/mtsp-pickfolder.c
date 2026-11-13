/* mtsp-pickfolder: choose a folder with Wine's folder browser and print its path on the Mac.
 *
 * metalsharp-steam runs this ("library choose") in the Wine prefix whose drives Steam's storage is shown
 * by, so a library is chosen the way Wine sees the disks: C:, and the drives Wine has for the Mac's
 * volumes. It is a console program so that what it prints reaches the pipe it was started with.
 *
 *   exit 0   a folder was chosen; its Unix path is printed as one UTF-8 line
 *   exit 1   nothing was chosen (cancelled, or what was chosen is not a folder with a path)
 *   exit 3   this is not Wine, or a Wine that cannot say where a Windows path is on the Mac; nothing is printed
 *
 * Plain Win32, built as an arm64 PE with llvm-mingw (scripts/stage-steamvm.sh):
 *   aarch64-w64-mingw32-clang -O2 -o mtsp-pickfolder.exe mtsp-pickfolder.c -lshell32 -lole32
 */
#define COBJMACROS
#include <windows.h>
#include <shlobj.h>
#include <string.h>

/* Wine's kernel32 has this; it is looked up at run time so that this links against stock mingw.
 * The result is in the Unix code page (UTF-8 on a Mac) and comes from the process heap. */
typedef char * (CDECL *unix_file_name_fn)(LPCWSTR dos);

static int CALLBACK browse_callback(HWND hwnd, UINT msg, LPARAM lparam, LPARAM data)
{
    (void)lparam;
    if (msg == BFFM_INITIALIZED) {
        SendMessageW(hwnd, BFFM_SETSELECTIONW, TRUE, data);     /* start at C:\ */
        /* it was started from a Mac application: come in front of that */
        BringWindowToTop(hwnd);
        SetForegroundWindow(hwnd);
    }
    return 0;
}

int main(void)
{
    static WCHAR start[] = L"C:\\";
    WCHAR display[MAX_PATH], path[MAX_PATH];
    BROWSEINFOW info;
    LPITEMIDLIST chosen;
    unix_file_name_fn unix_file_name;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    char *unix_path;
    DWORD written;
    BOOL have_path, ok;
    size_t len;

    unix_file_name = (unix_file_name_fn)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "wine_get_unix_file_name");
    if (!unix_file_name) return 3;

    OleInitialize(NULL);                    /* the new dialog style needs it */
    memset(&info, 0, sizeof(info));
    info.pszDisplayName = display;
    info.lpszTitle = L"Choose a folder for Steam to install games into";
    info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    info.lpfn = browse_callback;
    info.lParam = (LPARAM)start;
    chosen = SHBrowseForFolderW(&info);
    if (!chosen) { OleUninitialize(); return 1; }
    have_path = SHGetPathFromIDListW(chosen, path);
    CoTaskMemFree(chosen);
    OleUninitialize();
    if (!have_path || !path[0]) return 1;

    unix_path = unix_file_name(path);
    if (!unix_path) return 1;
    len = strlen(unix_path);
    /* one line; a path with a line break in it cannot be told apart from two, so it is not a choice */
    if (!len || unix_path[0] != '/' || strpbrk(unix_path, "\r\n")) return 1;
    ok = WriteFile(out, unix_path, (DWORD)len, &written, NULL) && written == len &&
         WriteFile(out, "\n", 1, &written, NULL) && written == 1;
    HeapFree(GetProcessHeap(), 0, unix_path);
    return ok ? 0 : 1;
}
