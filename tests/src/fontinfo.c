/* archs: x86_64 */
/* Font smoke test: check that the core Windows font families resolve to real fonts (macOS ships
 * them in /System/Library/Fonts/Supplemental; Wine loads the system fonts), plus a CJK family. */
#include <windows.h>
#include <stdio.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static int CALLBACK found( const LOGFONTW *lf, const TEXTMETRICW *tm, DWORD type, LPARAM param )
{
    *(int *)param = 1;
    return 0;
}

int main(void)
{
    static const WCHAR *families[] = { L"Arial", L"Times New Roman", L"Courier New", L"Verdana", L"Tahoma",
        L"Georgia", L"Trebuchet MS", L"Comic Sans MS", L"Impact", L"Webdings", L"Wingdings", L"Segoe UI",
        L"MS Sans Serif", L"Microsoft YaHei", L"MS Gothic", L"MS Mincho", L"SimSun", L"Microsoft JhengHei", L"PMingLiU", L"Malgun Gothic", L"Gulim" };
    HDC dc = GetDC( 0 );
    int i, missing = 0;

    for (i = 0; i < ARRAY_SIZE(families); i++)
    {
        LOGFONTW lf = { .lfCharSet = DEFAULT_CHARSET };
        WCHAR face[LF_FACESIZE] = L"";
        WORD glyphs[3];
        int exists = 0, cjk = 0, j;
        HFONT font, old;

        wcscpy( lf.lfFaceName, families[i] );
        EnumFontFamiliesExW( dc, &lf, found, (LPARAM)&exists, 0 );
        font = CreateFontIndirectW( &lf );
        old = SelectObject( dc, font );
        GetTextFaceW( dc, LF_FACESIZE, face );
        /* 中, あ, 가: how many of the Chinese, Japanese and Korean sample characters have glyphs */
        GetGlyphIndicesW( dc, L"\x4e2d\x3042\xac00", 3, glyphs, GGI_MARK_NONEXISTING_GLYPHS );
        for (j = 0; j < 3; j++) cjk += glyphs[j] != 0xffff;
        SelectObject( dc, old );
        DeleteObject( font );
        printf( "%-22ls %-8s -> %-24ls CJK glyphs %d/3\n", families[i], exists ? "present" : "missing", face, cjk );
        if (!exists) missing++;
    }
    printf( "%d of %u families missing\n", missing, (unsigned)ARRAY_SIZE(families) );
    return 0;
}
