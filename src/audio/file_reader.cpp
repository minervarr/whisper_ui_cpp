#include "file_reader.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace audio {

namespace {

// Builds the output media type the source reader must deliver:
// 16 kHz, mono, 32-bit IEEE float.
HRESULT create_pcm_float_type(IMFMediaType** ppType) {
    IMFMediaType* pType = nullptr;
    HRESULT hr = MFCreateMediaType(&pType);
    if (FAILED(hr)) return hr;

    hr = pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (FAILED(hr)) { pType->Release(); return hr; }

    hr = pType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    if (FAILED(hr)) { pType->Release(); return hr; }

    hr = pType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 1);
    if (FAILED(hr)) { pType->Release(); return hr; }

    hr = pType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 16000);
    if (FAILED(hr)) { pType->Release(); return hr; }

    hr = pType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
    if (FAILED(hr)) { pType->Release(); return hr; }

    hr = pType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);       // 1 ch × 4 bytes
    if (FAILED(hr)) { pType->Release(); return hr; }

    hr = pType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 64000); // 16000 × 4
    if (FAILED(hr)) { pType->Release(); return hr; }

    *ppType = pType;
    return S_OK;
}

std::wstring hr_message(HRESULT hr) {
    wchar_t buf[128];
    swprintf(buf, 128, L"Error MF: 0x%08X", (unsigned)hr);
    return buf;
}

} // namespace

std::shared_ptr<std::vector<float>>
    load_audio_file(const std::wstring& path, std::wstring* out_error)
{
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    if (FAILED(hr)) {
        if (out_error) *out_error = L"No se pudo inicializar Media Foundation.";
        return nullptr;
    }

    auto mf_guard = [](void*) { MFShutdown(); };
    std::unique_ptr<void, decltype(mf_guard)> mf_scope((void*)1, mf_guard);

    // Source reader attributes: enable hardware transforms for faster decoding.
    IMFAttributes* pAttr = nullptr;
    MFCreateAttributes(&pAttr, 1);
    if (pAttr) {
        pAttr->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, FALSE);
        pAttr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    }

    IMFSourceReader* pReader = nullptr;
    hr = MFCreateSourceReaderFromURL(path.c_str(), pAttr, &pReader);
    if (pAttr) pAttr->Release();

    if (FAILED(hr)) {
        if (out_error) {
            *out_error = L"No se pudo abrir el archivo de audio: ";
            *out_error += hr == MF_E_UNSUPPORTED_BYTESTREAM_TYPE
                ? L"formato no compatible."
                : hr_message(hr);
        }
        return nullptr;
    }

    // Deselect all streams, then select only the first audio stream.
    pReader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    pReader->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);

    IMFMediaType* pOutType = nullptr;
    hr = create_pcm_float_type(&pOutType);
    if (FAILED(hr)) {
        pReader->Release();
        if (out_error) *out_error = L"Error al crear tipo de media de salida.";
        return nullptr;
    }

    hr = pReader->SetCurrentMediaType(
        (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, pOutType);
    pOutType->Release();

    if (FAILED(hr)) {
        pReader->Release();
        if (out_error) {
            *out_error = hr == MF_E_INVALIDMEDIATYPE
                ? L"El archivo no contiene audio compatible."
                : L"Error al configurar formato de audio: " + hr_message(hr);
        }
        return nullptr;
    }

    // Read all samples.
    auto samples = std::make_shared<std::vector<float>>();
    samples->reserve(16000 * 60); // pre-allocate 1 minute

    for (;;) {
        DWORD flags = 0;
        IMFSample* pSample = nullptr;
        hr = pReader->ReadSample(
            (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM,
            0, nullptr, &flags, nullptr, &pSample);

        if (FAILED(hr)) break;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { break; }
        if (!pSample) continue;

        IMFMediaBuffer* pBuf = nullptr;
        if (SUCCEEDED(pSample->ConvertToContiguousBuffer(&pBuf))) {
            BYTE*  pData   = nullptr;
            DWORD  cbLen   = 0;
            if (SUCCEEDED(pBuf->Lock(&pData, nullptr, &cbLen)) && pData && cbLen > 0) {
                size_t count = cbLen / sizeof(float);
                const float* fData = reinterpret_cast<const float*>(pData);
                samples->insert(samples->end(), fData, fData + count);
                pBuf->Unlock();
            }
            pBuf->Release();
        }
        pSample->Release();
    }

    pReader->Release();

    if (samples->empty()) {
        if (out_error) *out_error = L"El archivo no contiene audio decodificable.";
        return nullptr;
    }

    return samples;
}

bool is_supported_audio_extension(const std::wstring& path) {
    auto pos = path.rfind(L'.');
    if (pos == std::wstring::npos) return false;

    std::wstring ext = path.substr(pos);
    // Lowercase for comparison.
    for (wchar_t& c : ext) c = (wchar_t)towlower(c);

    static const wchar_t* const kExts[] = {
        L".wav", L".mp3", L".m4a", L".aac",
        L".wma", L".flac", L".ogg", L".opus",
        L".mp4", L".mkv", L".mov", L".avi",  // video containers also have audio tracks
    };
    for (const wchar_t* e : kExts) {
        if (ext == e) return true;
    }
    return false;
}

} // namespace audio
