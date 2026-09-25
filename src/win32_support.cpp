// Windows platform helpers.
//
// Async HTTP via WinHTTP, mirroring ios_support.mm's NSURLSession pattern:
// thistle_http_start() kicks off the request on a detached background thread
// and returns immediately; the game polls thistle_http_poll() each frame.
// The shared_ptr<ThistleHttp> is captured BY VALUE into the thread function,
// so it stays alive until the request finishes even if the caller frees its
// handle first (same use-after-free fix as the Apple side).
#include <string>
#include <cstring>
#include <atomic>
#include <memory>
#include <thread>

// Before windows.h: it defines min/max as macros unless told not to, which
// breaks any std::min/std::max used later in this file (see the same fix
// and full explanation in thistle.cpp).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX // Jolt (THISTLE_PHYSICS3D) passes it on the command line too
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#include <xinput.h>
#include <shobjidl.h> // IFileOpenDialog, for pick_folder()

#include "thistle_gamepad.h"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "xinput.lib")

extern "C" void* thistle_http_start(const char* method, const char* url, const char* body);
extern "C" int   thistle_http_poll(void* h, int* status, const char** body, int* len);
extern "C" void  thistle_http_free(void* h);
std::string thistle_win32_pick_folder(void* owner, const std::string& title, const std::string& start_in);

// pick_folder(): the Explorer "Select Folder" dialog (Vista and later).
//
// The dialog needs a single-threaded COM apartment, and the main thread is a
// multithreaded one by the time a game runs: miniaudio makes it one
// (CoInitializeEx with COINIT_MULTITHREADED, in ma_engine_init). Shown from
// there, the dialog hung and the whole window went Not Responding (found on
// Windows 11, in the Thistle Editor's Open folder). So the dialog gets a
// thread of its own, and the main thread keeps handling its window's messages
// until the dialog closes.
namespace {

struct FolderDialog {
    HWND owner = nullptr;
    std::wstring title, start_in;
    std::string result;
};

std::wstring widen_utf8(const std::string& s) {
    if (s.empty()) return {};
    std::wstring w(static_cast<size_t>(MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), static_cast<int>(w.size()));
    return w;
}

void show_folder_dialog(FolderDialog& d) {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return;
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(d.title.c_str());
    if (!d.start_in.empty()) {
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(d.start_in.c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
            dialog->SetFolder(folder);
            folder->Release();
        }
    }
    if (SUCCEEDED(dialog->Show(d.owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                const int n = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
                if (n > 1) {
                    d.result.resize(static_cast<size_t>(n));
                    WideCharToMultiByte(CP_UTF8, 0, path, -1, d.result.data(), n, nullptr, nullptr);
                    d.result.pop_back(); // the terminator
                }
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
}

DWORD WINAPI folder_dialog_thread(void* arg) {
    if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
        show_folder_dialog(*static_cast<FolderDialog*>(arg));
        CoUninitialize();
    }
    return 0;
}

} // namespace

std::string thistle_win32_pick_folder(void* owner, const std::string& title, const std::string& start_in) {
    FolderDialog d;
    d.owner = static_cast<HWND>(owner);
    d.title = widen_utf8(title);
    d.start_in = widen_utf8(start_in);
    HANDLE thread = CreateThread(nullptr, 0, folder_dialog_thread, &d, 0, nullptr);
    if (!thread) return {};
    // While it's up the dialog disables its owner, our window, and Windows
    // does that by sending it a message and waiting for the answer: this
    // thread has to keep answering. Input can't reach the window meanwhile.
    bool quit = false;
    WPARAM quit_code = 0;
    for (;;) {
        const DWORD r = MsgWaitForMultipleObjectsEx(1, &thread, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (r != WAIT_OBJECT_0 + 1) {
            if (r != WAIT_OBJECT_0) WaitForSingleObject(thread, INFINITE); // d must outlive the thread
            break;
        }
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                quit = true;
                quit_code = msg.wParam;
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    CloseHandle(thread);
    if (quit) PostQuitMessage(static_cast<int>(quit_code)); // for the app's own loop
    return d.result;
}

namespace {

struct ThistleHttp {
    std::atomic<bool> done{false};
    int status = 0;
    std::string body;
};
using HttpPtr = std::shared_ptr<ThistleHttp>;

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

void run_request(HttpPtr sp, std::string method, std::string url, std::string body) {
    HINTERNET hSession = nullptr, hConnect = nullptr, hRequest = nullptr;
    auto finish = [&]() {
        if (hRequest) WinHttpCloseHandle(hRequest);
        if (hConnect) WinHttpCloseHandle(hConnect);
        if (hSession) WinHttpCloseHandle(hSession);
        sp->done.store(true);
    };

    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t hostBuf[256] = {}, pathBuf[2048] = {};
    uc.lpszHostName = hostBuf; uc.dwHostNameLength = 256;
    uc.lpszUrlPath  = pathBuf; uc.dwUrlPathLength  = 2048;
    std::wstring wurl = widen(url);
    if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.size(), 0, &uc)) return finish();

    bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;
    hSession = WinHttpOpen(L"Thistle/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return finish();
    WinHttpSetTimeouts(hSession, 15000, 15000, 15000, 15000);

    hConnect = WinHttpConnect(hSession, uc.lpszHostName, uc.nPort, 0);
    if (!hConnect) return finish();

    hRequest = WinHttpOpenRequest(hConnect, widen(method).c_str(), uc.lpszUrlPath,
                                   nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                   https ? WINHTTP_FLAG_SECURE : 0);
    if (!hRequest) return finish();

    BOOL ok;
    if (!body.empty()) {
        static const wchar_t* kJsonHeader = L"Content-Type: application/json";
        ok = WinHttpSendRequest(hRequest, kJsonHeader, (DWORD)-1,
                                 (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0);
    } else {
        ok = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    }
    if (!ok || !WinHttpReceiveResponse(hRequest, nullptr)) return finish();

    DWORD statusCode = 0, size = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                         WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &size, WINHTTP_NO_HEADER_INDEX);
    sp->status = (int)statusCode;

    std::string out;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &avail) || avail == 0) break;
        std::string chunk(avail, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(hRequest, chunk.data(), avail, &read)) break;
        out.append(chunk.data(), read);
    }
    sp->body = std::move(out);
    finish();
}

} // namespace

void* thistle_http_start(const char* method, const char* url, const char* body) {
    auto sp = std::make_shared<ThistleHttp>();
    if (!url) { sp->done.store(true); return new HttpPtr(sp); }
    std::thread(run_request, sp, std::string(method && *method ? method : "GET"),
                std::string(url), std::string(body ? body : "")).detach();
    return new HttpPtr(sp);
}

int thistle_http_poll(void* handle, int* status, const char** body, int* len) {
    auto* spp = static_cast<HttpPtr*>(handle);
    if (!spp || !(*spp) || !(*spp)->done.load()) return 0;
    ThistleHttp& h = **spp;
    if (status) *status = h.status;
    if (body) *body = h.body.c_str();
    if (len) *len = (int)h.body.size();
    return 1;
}

void thistle_http_free(void* handle) { delete static_cast<HttpPtr*>(handle); }

// --- gamepad via XInput ------------------------------------------------
// XInput only ever represents an Xbox-shaped controller (that's the whole
// point of the API — it's how Windows normalizes third-party pads too), so
// kind is always Xbox; there's no vendor-name query to do here the way
// GameController.framework or a Linux joystick device name lets us pick a
// real display name. -32768..32767 sticks and 0..255 triggers get normalized
// to match the same -1..1 / 0..1 ranges every other platform reports.
void thistle_win32_poll_gamepad(ThistleGamepad* g) {
    std::memset(g, 0, sizeof(*g));
    XINPUT_STATE state{};
    DWORD found = ERROR_DEVICE_NOT_CONNECTED;
    for (DWORD i = 0; i < XUSER_MAX_COUNT && found != ERROR_SUCCESS; ++i) {
        found = XInputGetState(i, &state);
    }
    if (found != ERROR_SUCCESS) return;

    g->connected = 1;
    g->kind = 1; // Xbox
    std::strncpy(g->name, "XInput Controller", sizeof(g->name) - 1);

    const WORD b = state.Gamepad.wButtons;
    g->a = (b & XINPUT_GAMEPAD_A) != 0;
    g->b = (b & XINPUT_GAMEPAD_B) != 0;
    g->x = (b & XINPUT_GAMEPAD_X) != 0;
    g->y = (b & XINPUT_GAMEPAD_Y) != 0;
    g->up = (b & XINPUT_GAMEPAD_DPAD_UP) != 0;
    g->down = (b & XINPUT_GAMEPAD_DPAD_DOWN) != 0;
    g->left = (b & XINPUT_GAMEPAD_DPAD_LEFT) != 0;
    g->right = (b & XINPUT_GAMEPAD_DPAD_RIGHT) != 0;
    g->l1 = (b & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;
    g->r1 = (b & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;
    g->start = (b & XINPUT_GAMEPAD_START) != 0;
    g->back = (b & XINPUT_GAMEPAD_BACK) != 0;

    g->lx = state.Gamepad.sThumbLX / 32767.0f;
    g->ly = state.Gamepad.sThumbLY / 32767.0f;
    g->rx = state.Gamepad.sThumbRX / 32767.0f;
    g->ry = state.Gamepad.sThumbRY / 32767.0f;
    g->lt = state.Gamepad.bLeftTrigger / 255.0f;
    g->rt = state.Gamepad.bRightTrigger / 255.0f;
}
