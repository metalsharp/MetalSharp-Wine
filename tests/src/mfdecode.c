/* archs: aarch64 x86_64 */
/* Media Foundation smoke test (winedmo, i.e. FFmpeg): decode an AAC .m4a to PCM through the
 * source reader and count the samples. Test file (not in git):
 *   afconvert -f m4af -d aac -b 128000 /System/Library/Sounds/Glass.aiff tests/media/glass.m4a */
#define COBJMACROS
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <stdio.h>

int main( int argc, char **argv )
{
    WCHAR path[MAX_PATH];
    IMFSourceReader *reader;
    IMFMediaType *type, *out;
    UINT32 rate = 0, channels = 0;
    DWORD flags, total = 0;
    HRESULT hr;

    if (argc < 2) return 1;
    MultiByteToWideChar( CP_ACP, 0, argv[1], -1, path, MAX_PATH );
    MFStartup( MF_VERSION, 0 );
    if (FAILED(hr = MFCreateSourceReaderFromURL( path, NULL, &reader )))
    {
        printf( "MFCreateSourceReaderFromURL failed: %#lx\n", hr );
        return 1;
    }
    MFCreateMediaType( &type );
    IMFMediaType_SetGUID( type, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio );
    IMFMediaType_SetGUID( type, &MF_MT_SUBTYPE, &MFAudioFormat_PCM );
    hr = IMFSourceReader_SetCurrentMediaType( reader, MF_SOURCE_READER_FIRST_AUDIO_STREAM, NULL, type );
    if (FAILED(hr)) { printf( "SetCurrentMediaType(PCM) failed: %#lx\n", hr ); return 1; }
    IMFSourceReader_GetCurrentMediaType( reader, MF_SOURCE_READER_FIRST_AUDIO_STREAM, &out );
    IMFMediaType_GetUINT32( out, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate );
    IMFMediaType_GetUINT32( out, &MF_MT_AUDIO_NUM_CHANNELS, &channels );
    for (;;)
    {
        IMFSample *sample = NULL;
        DWORD len = 0;

        hr = IMFSourceReader_ReadSample( reader, MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, NULL, &flags, NULL, &sample );
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) break;
        if (sample)
        {
            IMFSample_GetTotalLength( sample, &len );
            total += len;
            IMFSample_Release( sample );
        }
    }
    printf( "decoded %lu bytes of PCM, %u Hz, %u channels (%#lx)\n", total, rate, channels, hr );
    return FAILED(hr) || !total;
}
