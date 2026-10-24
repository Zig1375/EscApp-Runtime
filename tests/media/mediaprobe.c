/* Plays a video file the two ways games do and prints "PROBE <api> <file> OK" for each that works:
 *   mf      Media Foundation source reader, decoding video frames (to RGB32, with the reader's video processing
 *           on, as games ask for it) and audio (to PCM);
 *   dshow   DirectShow graph from RenderFile, run to the end with the video window hidden.
 * Usage: mediaprobe.exe mf|dshow <file>. Exit code 0 when the probe passed. */
#define COBJMACROS
#include <windows.h>
#include <stdio.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <dshow.h>

static int probe_mf(const WCHAR *path)
{
    IMFSourceReader *reader;
    IMFAttributes *attrs;
    IMFMediaType *type;
    int frames = 0, audio = 0, i;
    HRESULT hr;

    if (FAILED(hr = MFStartup(MF_VERSION, MFSTARTUP_FULL))) { printf("MFStartup %#lx\n", hr); return 0; }
    MFCreateAttributes(&attrs, 1);
    IMFAttributes_SetUINT32(attrs, &MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    if (FAILED(hr = MFCreateSourceReaderFromURL(path, attrs, &reader))) { printf("source reader %#lx\n", hr); return 0; }

    MFCreateMediaType(&type);
    IMFMediaType_SetGUID(type, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(type, &MF_MT_SUBTYPE, &MFVideoFormat_RGB32);
    if (FAILED(hr = IMFSourceReader_SetCurrentMediaType(reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, type)))
    {
        printf("video RGB32 %#lx\n", hr);
        return 0;
    }
    IMFMediaType_Release(type);
    MFCreateMediaType(&type);
    IMFMediaType_SetGUID(type, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
    IMFMediaType_SetGUID(type, &MF_MT_SUBTYPE, &MFAudioFormat_PCM);
    if (FAILED(hr = IMFSourceReader_SetCurrentMediaType(reader, MF_SOURCE_READER_FIRST_AUDIO_STREAM, NULL, type)))
    {
        printf("audio PCM %#lx\n", hr);
        return 0;
    }
    IMFMediaType_Release(type);

    for (i = 0; i < 400 && (frames < 10 || audio < 10); i++)
    {
        DWORD index, flags;
        LONGLONG time;
        IMFSample *sample = NULL;

        if (FAILED(hr = IMFSourceReader_ReadSample(reader, MF_SOURCE_READER_ANY_STREAM, 0, &index, &flags, &time, &sample)))
        {
            printf("ReadSample %#lx\n", hr);
            return 0;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (!sample) continue;
        if (index == 0) frames++; else audio++;
        IMFSample_Release(sample);
    }
    printf("mf: %d video frames, %d audio buffers\n", frames, audio);
    IMFSourceReader_Release(reader);
    MFShutdown();
    return frames >= 10 && audio >= 10;
}

static int probe_dshow(const WCHAR *path)
{
    IGraphBuilder *graph;
    IMediaControl *control;
    IMediaEvent *event;
    IVideoWindow *window;
    LONG code = 0;
    HRESULT hr;

    if (FAILED(hr = CoCreateInstance(&CLSID_FilterGraph, NULL, CLSCTX_INPROC_SERVER, &IID_IGraphBuilder, (void **)&graph)))
    {
        printf("filter graph %#lx\n", hr);
        return 0;
    }
    if (FAILED(hr = IGraphBuilder_RenderFile(graph, path, NULL))) { printf("RenderFile %#lx\n", hr); return 0; }
    if (SUCCEEDED(IGraphBuilder_QueryInterface(graph, &IID_IVideoWindow, (void **)&window)))
    {
        IVideoWindow_put_AutoShow(window, OAFALSE);
        IVideoWindow_Release(window);
    }
    IGraphBuilder_QueryInterface(graph, &IID_IMediaControl, (void **)&control);
    IGraphBuilder_QueryInterface(graph, &IID_IMediaEvent, (void **)&event);
    if (FAILED(hr = IMediaControl_Run(control))) { printf("Run %#lx\n", hr); return 0; }
    hr = IMediaEvent_WaitForCompletion(event, 30000, &code);
    printf("dshow: wait %#lx, event %#lx\n", hr, code);
    IMediaControl_Stop(control);
    IMediaEvent_Release(event);
    IMediaControl_Release(control);
    IGraphBuilder_Release(graph);
    return hr == S_OK && code == EC_COMPLETE;
}

int wmain(int argc, WCHAR **argv)
{
    int ok;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc != 3) { printf("usage: mediaprobe mf|dshow <file>\n"); return 2; }
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    ok = !wcscmp(argv[1], L"mf") ? probe_mf(argv[2]) : probe_dshow(argv[2]);
    printf("PROBE %ls %ls %s\n", argv[1], argv[2], ok ? "OK" : "FAILED");
    return ok ? 0 : 1;
}
