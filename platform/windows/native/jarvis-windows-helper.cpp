#include <windows.h>
#include <tlhelp32.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <map>
#include <set>
#include <string>
#include <vector>

using Gdiplus::Bitmap;
using Gdiplus::GdiplusShutdown;
using Gdiplus::GdiplusStartup;
using Gdiplus::GdiplusStartupInput;
using Gdiplus::ImageCodecInfo;
using Gdiplus::Ok;

namespace {

bool parse_pid(const wchar_t* value, DWORD* result) {
  if (!value || !*value || !result) return false;
  wchar_t* end = nullptr;
  unsigned long parsed = std::wcstoul(value, &end, 10);
  if (!end || *end != L'\0' || parsed == 0 || parsed > MAXDWORD) return false;
  *result = static_cast<DWORD>(parsed);
  return true;
}

std::wstring normalize_path(const std::wstring& input) {
  if (input.empty()) return {};
  std::vector<wchar_t> buffer(32768);
  DWORD length = GetFullPathNameW(input.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
  if (length == 0 || length >= buffer.size()) return {};
  std::wstring output(buffer.data(), length);
  std::transform(output.begin(), output.end(), output.begin(), towlower);
  return output;
}

struct WindowSearch {
  DWORD pid;
  HWND window;
};

BOOL CALLBACK enum_window_for_pid(HWND window, LPARAM parameter) {
  auto* search = reinterpret_cast<WindowSearch*>(parameter);
  DWORD pid = 0;
  GetWindowThreadProcessId(window, &pid);
  if (pid != search->pid || !IsWindowVisible(window) || GetWindow(window, GW_OWNER) != nullptr) return TRUE;
  search->window = window;
  return FALSE;
}

HWND main_window_for_pid(DWORD pid) {
  WindowSearch search{pid, nullptr};
  EnumWindows(enum_window_for_pid, reinterpret_cast<LPARAM>(&search));
  return search.window;
}

int window_find(const wchar_t* executable_path) {
  const std::wstring expected = normalize_path(executable_path ? executable_path : L"");
  if (expected.empty()) return 2;
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return 3;
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  DWORD found = 0;
  if (Process32FirstW(snapshot, &entry)) {
    do {
      HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
      if (!process) continue;
      std::vector<wchar_t> path_buffer(32768);
      DWORD path_length = static_cast<DWORD>(path_buffer.size());
      if (QueryFullProcessImageNameW(process, 0, path_buffer.data(), &path_length)) {
        std::wstring actual(path_buffer.data(), path_length);
        std::transform(actual.begin(), actual.end(), actual.begin(), towlower);
        if (actual == expected) {
          found = entry.th32ProcessID;
          CloseHandle(process);
          break;
        }
      }
      CloseHandle(process);
    } while (Process32NextW(snapshot, &entry));
  }
  CloseHandle(snapshot);
  if (!found) {
    std::printf("NOT_FOUND\n");
    return 0;
  }
  std::printf("%lu\n", static_cast<unsigned long>(found));
  return 0;
}

int window_action(const std::wstring& action, DWORD pid) {
  HWND window = main_window_for_pid(pid);
  if (!window) return 4;
  if (action == L"restore") {
    ShowWindowAsync(window, SW_RESTORE);
  } else if (action == L"maximize") {
    ShowWindowAsync(window, SW_MAXIMIZE);
  } else if (action == L"minimize") {
    ShowWindowAsync(window, SW_MINIMIZE);
  } else if (action == L"focus") {
    ShowWindowAsync(window, IsIconic(window) ? SW_RESTORE : SW_SHOW);
    DWORD foreground_thread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    DWORD target_thread = GetWindowThreadProcessId(window, nullptr);
    DWORD current_thread = GetCurrentThreadId();
    if (foreground_thread) AttachThreadInput(current_thread, foreground_thread, TRUE);
    if (target_thread && target_thread != foreground_thread) AttachThreadInput(current_thread, target_thread, TRUE);
    BringWindowToTop(window);
    const BOOL focused = SetForegroundWindow(window);
    SetFocus(window);
    if (target_thread && target_thread != foreground_thread) AttachThreadInput(current_thread, target_thread, FALSE);
    if (foreground_thread) AttachThreadInput(current_thread, foreground_thread, FALSE);
    if (!focused && GetForegroundWindow() != window) return 5;
  } else {
    return 2;
  }
  std::printf("OK\n");
  return 0;
}

class ComGuard {
 public:
  ComGuard() : result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
  ~ComGuard() {
    if (SUCCEEDED(result_)) CoUninitialize();
  }
  bool ok() const { return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE; }
 private:
  HRESULT result_;
};

int volume_action(const std::wstring& action, int value, bool has_value) {
  ComGuard com;
  if (!com.ok()) return 6;
  IMMDeviceEnumerator* enumerator = nullptr;
  IMMDevice* device = nullptr;
  IAudioEndpointVolume* endpoint = nullptr;
  HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
  if (SUCCEEDED(result)) result = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
  if (SUCCEEDED(result)) {
    result = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(&endpoint));
  }
  if (FAILED(result) || !endpoint) {
    if (device) device->Release();
    if (enumerator) enumerator->Release();
    return 7;
  }
  float scalar = 0.0f;
  BOOL muted = FALSE;
  endpoint->GetMasterVolumeLevelScalar(&scalar);
  endpoint->GetMute(&muted);
  if (action == L"set" || action == L"up" || action == L"down") {
    if (!has_value || value < 0 || value > 100) result = E_INVALIDARG;
    else {
      int current = static_cast<int>(std::lround(scalar * 100.0f));
      int target = action == L"set" ? value : action == L"up" ? current + value : current - value;
      target = std::max(0, std::min(100, target));
      result = endpoint->SetMasterVolumeLevelScalar(static_cast<float>(target) / 100.0f, nullptr);
    }
  } else if (action == L"mute") {
    result = endpoint->SetMute(TRUE, nullptr);
  } else if (action == L"unmute") {
    result = endpoint->SetMute(FALSE, nullptr);
  } else if (action != L"get") {
    result = E_INVALIDARG;
  }
  if (SUCCEEDED(result)) {
    endpoint->GetMasterVolumeLevelScalar(&scalar);
    endpoint->GetMute(&muted);
  }
  endpoint->Release();
  device->Release();
  enumerator->Release();
  if (FAILED(result)) return 8;
  int level = std::max(0, std::min(100, static_cast<int>(std::lround(scalar * 100.0f))));
  std::printf("VOLUME %d %s\n", level, muted ? "MUTED" : "UNMUTED");
  return 0;
}

int png_encoder_clsid(CLSID* clsid) {
  UINT count = 0;
  UINT bytes = 0;
  Gdiplus::GetImageEncodersSize(&count, &bytes);
  if (!count || !bytes) return 9;
  std::vector<BYTE> storage(bytes);
  auto* encoders = reinterpret_cast<ImageCodecInfo*>(storage.data());
  if (Gdiplus::GetImageEncoders(count, bytes, encoders) != Ok) return 9;
  for (UINT index = 0; index < count; ++index) {
    if (std::wcscmp(encoders[index].MimeType, L"image/png") == 0) {
      *clsid = encoders[index].Clsid;
      return 0;
    }
  }
  return 9;
}

int screenshot(const wchar_t* output_path) {
  const std::wstring path = output_path ? output_path : L"";
  if (path.size() < 5 || _wcsicmp(path.c_str() + path.size() - 4, L".png") != 0) return 2;
  const int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
  const int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
  const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
  const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  if (width <= 0 || height <= 0) return 10;
  HDC screen_dc = GetDC(nullptr);
  HDC memory_dc = CreateCompatibleDC(screen_dc);
  HBITMAP bitmap = CreateCompatibleBitmap(screen_dc, width, height);
  HGDIOBJ old_bitmap = SelectObject(memory_dc, bitmap);
  const BOOL copied = BitBlt(memory_dc, 0, 0, width, height, screen_dc, left, top, SRCCOPY | CAPTUREBLT);
  SelectObject(memory_dc, old_bitmap);
  DeleteDC(memory_dc);
  ReleaseDC(nullptr, screen_dc);
  if (!copied) {
    DeleteObject(bitmap);
    return 11;
  }
  GdiplusStartupInput input;
  ULONG_PTR token = 0;
  if (GdiplusStartup(&token, &input, nullptr) != Ok) {
    DeleteObject(bitmap);
    return 12;
  }
  Bitmap image(bitmap, nullptr);
  CLSID png_clsid{};
  int encoder_result = png_encoder_clsid(&png_clsid);
  const Gdiplus::Status saved = encoder_result == 0 ? image.Save(path.c_str(), &png_clsid, nullptr) : Gdiplus::GenericError;
  GdiplusShutdown(token);
  DeleteObject(bitmap);
  if (saved != Ok) return 13;
  std::printf("SAVED\n");
  return 0;
}

int process_terminate_descendants(DWORD root_pid, const std::set<DWORD>& exclusions) {
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return 14;
  std::map<DWORD, DWORD> parents;
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  if (Process32FirstW(snapshot, &entry)) {
    do {
      parents[entry.th32ProcessID] = entry.th32ParentProcessID;
    } while (Process32NextW(snapshot, &entry));
  }
  CloseHandle(snapshot);

  auto depth_from = [&parents](DWORD pid, DWORD ancestor) {
    int depth = 0;
    std::set<DWORD> visited;
    while (pid && visited.insert(pid).second) {
      auto found = parents.find(pid);
      if (found == parents.end()) return -1;
      pid = found->second;
      ++depth;
      if (pid == ancestor) return depth;
    }
    return -1;
  };

  std::vector<std::pair<int, DWORD>> targets;
  const DWORD self_pid = GetCurrentProcessId();
  for (const auto& item : parents) {
    const DWORD pid = item.first;
    if (pid == root_pid || pid == self_pid) continue;
    const int depth = depth_from(pid, root_pid);
    if (depth < 1) continue;
    bool excluded = exclusions.count(pid) > 0;
    for (DWORD exclusion : exclusions) {
      if (depth_from(pid, exclusion) > 0) {
        excluded = true;
        break;
      }
    }
    if (!excluded) targets.push_back({depth, pid});
  }
  std::sort(targets.begin(), targets.end(), [](const auto& left, const auto& right) {
    return left.first > right.first;
  });
  int terminated = 0;
  int failed = 0;
  for (const auto& target : targets) {
    HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, target.second);
    if (!process) {
      if (GetLastError() != ERROR_INVALID_PARAMETER) ++failed;
      continue;
    }
    if (TerminateProcess(process, 1)) {
      WaitForSingleObject(process, 1000);
      ++terminated;
    } else if (WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
      ++failed;
    }
    CloseHandle(process);
  }
  if (failed > 0) return 15;
  std::printf("TERMINATED %d\n", terminated);
  return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc < 3) return 2;
  const std::wstring group = argv[1];
  const std::wstring action = argv[2];
  if (group == L"window") {
    if (action == L"find" && argc == 4) return window_find(argv[3]);
    DWORD pid = 0;
    if (argc == 4 && parse_pid(argv[3], &pid)) return window_action(action, pid);
    return 2;
  }
  if (group == L"volume") {
    int value = 0;
    bool has_value = false;
    if (argc == 4) {
      wchar_t* end = nullptr;
      long parsed = std::wcstol(argv[3], &end, 10);
      if (!end || *end != L'\0' || parsed < 0 || parsed > 100) return 2;
      value = static_cast<int>(parsed);
      has_value = true;
    } else if (argc != 3) {
      return 2;
    }
    return volume_action(action, value, has_value);
  }
  if (group == L"screenshot" && action.size() > 0 && argc == 3) {
    return screenshot(argv[2]);
  }
  if (group == L"process" && action == L"terminate-descendants" && argc >= 4) {
    DWORD root_pid = 0;
    if (!parse_pid(argv[3], &root_pid)) return 2;
    std::set<DWORD> exclusions;
    for (int index = 4; index < argc; ++index) {
      DWORD pid = 0;
      if (!parse_pid(argv[index], &pid)) return 2;
      exclusions.insert(pid);
    }
    return process_terminate_descendants(root_pid, exclusions);
  }
  return 2;
}
