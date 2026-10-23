/* archs: x86_64 i686 */
/* MSHTML (Wine Gecko) smoke test: create an HTML document, write markup into it and read the
 * body's text back. There is no ARM64 Gecko, so this is built for x86_64 and i386 only. */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <ole2.h>
#include <mshtml.h>
#include <stdio.h>

int main(void)
{
    IHTMLDocument2 *doc = NULL;
    IHTMLElement *body = NULL;
    SAFEARRAY *array;
    VARIANT *var;
    BSTR text = NULL;
    HRESULT hr;

    OleInitialize( NULL );
    hr = CoCreateInstance( &CLSID_HTMLDocument, NULL, CLSCTX_INPROC_SERVER, &IID_IHTMLDocument2, (void **)&doc );
    if (FAILED(hr)) { printf( "CoCreateInstance(HTMLDocument) failed: %#lx\n", hr ); return 1; }
    array = SafeArrayCreateVector( VT_VARIANT, 0, 1 );
    SafeArrayAccessData( array, (void **)&var );
    V_VT(var) = VT_BSTR;
    V_BSTR(var) = SysAllocString( L"<html><body><p>Hello from <b>Wine Gecko</b></p></body></html>" );
    SafeArrayUnaccessData( array );
    hr = IHTMLDocument2_write( doc, array );
    IHTMLDocument2_close( doc );
    if (SUCCEEDED(hr)) hr = IHTMLDocument2_get_body( doc, &body );
    if (SUCCEEDED(hr) && body) hr = IHTMLElement_get_innerText( body, &text );
    printf( "body text: %ls (%#lx)\n", text ? text : L"(none)", hr );
    return !text || wcscmp( text, L"Hello from Wine Gecko" );
}
