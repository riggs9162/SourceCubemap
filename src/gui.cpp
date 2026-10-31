#define UNICODE
#define _UNICODE
#include <Windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <gdiplus.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <memory>
#include <vector>
#include <map>
#include <algorithm>
#include <format>
#include <array>
#include <cstring>
#include <optional>
#include <set>

namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
constexpr UINT logMessage = WM_APP + 1, finishedMessage = WM_APP + 2;
enum Control : int { Map = 100, Game, Output, BrowseMap, BrowseGame, BrowseOutput, Build, Settings, Status, Progress, Preview, Position, Slider, Variant, Result, OpenFolder, OpenLog, Hdr, Quality, Water, KeepGoing, Incremental, Mounts, AddMount, Done };
struct Choice { const wchar_t* label; const wchar_t* value; };
constexpr std::array<Choice, 4> lightingChoices{{{L"Same as the map", L"auto"}, {L"LDR only", L"ldr"}, {L"HDR only", L"hdr"}, {L"LDR and HDR", L"both"}}};
constexpr std::array<Choice, 3> qualityChoices{{{L"Standard", L"standard"}, {L"Fast", L"fast"}, {L"High (sharper, slower)", L"high"}}};
constexpr std::array<Choice, 2> variantChoices{{{L"LDR", L""}, {L"HDR", L".hdr"}}};
struct FaceView { const wchar_t* suffix; int column, row; Gdiplus::RotateFlipType turn; };
const std::array<FaceView, 6> faceViews{{{L"px", 1, 1, Gdiplus::Rotate90FlipX}, {L"nx", 3, 1, Gdiplus::Rotate270FlipX}, {L"py", 0, 1, Gdiplus::RotateNoneFlipY}, {L"ny", 2, 1, Gdiplus::RotateNoneFlipX}, {L"pz", 1, 0, Gdiplus::Rotate90FlipX}, {L"nz", 1, 2, Gdiplus::Rotate90FlipX}}};
constexpr std::array<std::pair<int, const char*>, 4> textSettings{{{Map, "map"}, {Game, "game"}, {Output, "output"}, {Mounts, "mounts"}}};
constexpr std::array<std::pair<int, const char*>, 3> flagSettings{{{Water, "water"}, {KeepGoing, "keep_going"}, {Incremental, "incremental"}}};

struct Handle {
    HANDLE value = nullptr;
    Handle() = default;
    explicit Handle(HANDLE input) : value(input) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) { CloseHandle(value); } }
};
std::wstring wide(std::string_view text) {
    if (text.empty()) { return {}; }
    int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(size, 0); MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size); return result;
}
std::string utf8(std::wstring_view text) {
    int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, 0); WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr); return result;
}
std::wstring quote(std::wstring_view text) {
    std::wstring result = L"\""; size_t slashes = 0;
    for (auto character : text) {
        if (character == L'\\') { ++slashes; continue; }
        result.append(slashes * (character == L'"' ? 2 : 1), L'\\'); slashes = 0;
        if (character == L'"') { result += L'\\'; }
        result += character;
    }
    result.append(slashes * 2, L'\\'); return result + L'"';
}
fs::path executableDirectory() {
    std::wstring path(32768, 0); auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) { throw std::runtime_error("Cannot locate the application directory."); }
    path.resize(length); return fs::path(path).parent_path();
}
fs::path stateDirectory() {
    wchar_t* value = nullptr; size_t count = 0; _wdupenv_s(&value, &count, L"LOCALAPPDATA");
    fs::path result = value ? fs::path(value) / "SourceCubemap" : fs::temp_directory_path() / "SourceCubemap";
    std::free(value); fs::create_directories(result); return result;
}
std::wstring choose(HWND owner, bool folder, bool save, std::wstring initial = {}) {
    IFileDialog* dialog = nullptr;
    HRESULT status = CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(status)) { throw std::runtime_error("The Windows file picker is unavailable."); }
    FILEOPENDIALOGOPTIONS options; dialog->GetOptions(&options);
    dialog->SetOptions((options & ~FOS_OVERWRITEPROMPT) | FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR | (folder ? FOS_PICKFOLDERS : 0));
    if (!folder) { COMDLG_FILTERSPEC filter{L"Source BSP maps", L"*.bsp"}; dialog->SetFileTypes(1, &filter); dialog->SetDefaultExtension(L"bsp"); }
    if (!initial.empty()) { dialog->SetFileName(initial.c_str()); }
    std::wstring result;
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) { result = path; CoTaskMemFree(path); }
            item->Release();
        }
    }
    dialog->Release(); return result;
}
struct MapInfo { unsigned cubemaps = 0; bool ldr = false, hdr = false; };
std::optional<MapInfo> readMapInfo(const fs::path& path) {
    struct Lump { int32_t offset, length, version; uint32_t uncompressed; };
    struct Header { char ident[4]; int32_t version; Lump lumps[64]; } header{};
    std::ifstream input(path, std::ios::binary);
    if (!input.read(reinterpret_cast<char*>(&header), sizeof(header)) || std::memcmp(header.ident, "VBSP", 4)) { return std::nullopt; }
    const auto& samples = header.lumps[42];
    return MapInfo{(samples.uncompressed ? samples.uncompressed : static_cast<uint32_t>(samples.length)) / 16, header.lumps[8].length > 0, header.lumps[53].length > 0};
}
bool isFile(const fs::path& path) { std::error_code ignored; return fs::is_regular_file(path, ignored); }
struct JobResult { DWORD exitCode = 1; fs::path report, previews; std::wstring error; };

void runWorker(HWND window, std::vector<std::wstring> arguments, fs::path report, fs::path previews) {
    auto result = std::make_unique<JobResult>(); result->report = report; result->previews = previews;
    try {
        auto executable = executableDirectory() / "sourcecubemap.exe";
        if (!fs::exists(executable)) { throw std::runtime_error("sourcecubemap.exe must be beside the GUI executable."); }
        std::wstring command = quote(executable.wstring());
        for (const auto& argument : arguments) { command += L" " + quote(argument); }
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        Handle read, write, input;
        if (!CreatePipe(&read.value, &write.value, &security, 0) || !SetHandleInformation(read.value, HANDLE_FLAG_INHERIT, 0)) { throw std::runtime_error("Cannot create the worker output pipe."); }
        input.value = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr);
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = startup.hStdError = write.value; startup.hStdInput = input.value;
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, executableDirectory().c_str(), &startup, &process)) { throw std::runtime_error("Cannot start the SourceCubemap worker."); }
        Handle processHandle(process.hProcess), threadHandle(process.hThread), job(CreateJobObjectW(nullptr, nullptr));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job.value || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) || !AssignProcessToJobObject(job.value, processHandle.value)) {
            TerminateProcess(processHandle.value, 1); throw std::runtime_error("Cannot contain the worker process safely.");
        }
        ResumeThread(threadHandle.value); CloseHandle(write.value); write.value = nullptr;
        std::ofstream savedLog(report.parent_path() / "worker.log", std::ios::binary);
        if (!savedLog) { throw std::runtime_error("Cannot create the worker log."); }
        std::string pending; char buffer[4096]; DWORD count = 0;
        auto send = [&](std::string_view line) {
            auto text = std::make_unique<std::wstring>(wide(line));
            if (PostMessageW(window, logMessage, 0, reinterpret_cast<LPARAM>(text.get()))) { text.release(); }
        };
        while (ReadFile(read.value, buffer, sizeof(buffer), &count, nullptr) && count) {
            savedLog.write(buffer, count); savedLog.flush();
            pending.append(buffer, count); size_t end = 0;
            while ((end = pending.find('\n')) != std::string::npos) { send(pending.substr(0, end)); pending.erase(0, end + 1); }
            if (pending.size() > 65536) { send(pending); pending.clear(); }
        }
        if (!pending.empty()) { send(pending); }
        WaitForSingleObject(processHandle.value, INFINITE); GetExitCodeProcess(processHandle.value, &result->exitCode);
    } catch (const std::exception& error) { result->error = wide(error.what()); }
    if (PostMessageW(window, finishedMessage, 0, reinterpret_cast<LPARAM>(result.get()))) { result.release(); }
}

struct App {
    HWND window = nullptr, options = nullptr, host = nullptr;
    std::map<int, HWND> controls;
    std::vector<HWND> labels;
    HFONT font = nullptr;
    std::jthread worker;
    Handle cancellation;
    bool busy = false, closeWhenDone = false, finishedOnce = false, saved = false;
    std::wstring failure, derivedOutput;
    fs::path logPath;
    fs::path previewFolder;
    std::array<std::vector<std::wstring>, 2> cubes;
    std::array<std::unique_ptr<Gdiplus::Bitmap>, 6> faces;
    UINT dpi = 96;
    int lineHeight = 19;

    ~App() { if (font) { DeleteObject(font); } }
    int scaled(int value) const { return MulDiv(value, dpi, 96); }
    HWND control(int id, const wchar_t* kind, std::wstring text, DWORD style = 0) {
        bool bordered = kind == std::wstring_view(L"EDIT") && !(style & ES_READONLY);
        auto handle = CreateWindowExW(bordered ? WS_EX_CLIENTEDGE : 0, kind, text.c_str(), WS_CHILD | WS_VISIBLE | style, 0, 0, 1, 1, host, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
        if (!handle) { throw std::runtime_error("Cannot create a GUI control."); }
        SendMessageW(handle, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE); controls[id] = handle; return handle;
    }
    HWND label(std::wstring text) {
        auto handle = control(1000 + static_cast<int>(labels.size()), L"STATIC", std::move(text)); labels.push_back(handle); return handle;
    }
    void place(HWND handle, int x, int y, int width, int height) { MoveWindow(handle, scaled(x), scaled(y), scaled(width), scaled(height), TRUE); }
    void place(int id, int x, int y, int width, int height) { place(controls.at(id), x, y, width, height); }
    std::wstring text(int id) const {
        auto handle = controls.at(id); int size = GetWindowTextLengthW(handle); std::wstring value(size + 1, 0); GetWindowTextW(handle, value.data(), size + 1); value.resize(size); return value;
    }
    void set(int id, std::wstring_view value) { SetWindowTextW(controls.at(id), std::wstring(value).c_str()); }
    void show(int id, bool visible) { ShowWindow(controls.at(id), visible ? SW_SHOW : SW_HIDE); }
    bool checked(int id) const { return SendMessageW(controls.at(id), BM_GETCHECK, 0, 0) == BST_CHECKED; }
    void check(int id, bool value) { SendMessageW(controls.at(id), BM_SETCHECK, value ? BST_CHECKED : BST_UNCHECKED, 0); }
    void hint(int id, const wchar_t* value) { SendMessageW(controls.at(id), EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(value)); }
    template <size_t Size> void combo(int id, const std::array<Choice, Size>& choices) {
        control(id, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP);
        for (const auto& choice : choices) { SendMessageW(controls.at(id), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(choice.label)); }
        SendMessageW(controls.at(id), CB_SETCURSEL, 0, 0);
    }
    template <size_t Size> std::wstring chosen(int id, const std::array<Choice, Size>& choices) const {
        return choices.at(static_cast<size_t>(SendMessageW(controls.at(id), CB_GETCURSEL, 0, 0))).value;
    }
    template <size_t Size> void choose(int id, const std::array<Choice, Size>& choices, std::wstring_view value) {
        for (size_t i = 0; i < Size; ++i) { if (value == choices[i].value) { SendMessageW(controls.at(id), CB_SETCURSEL, i, 0); } }
    }
    void initialize() {
        dpi = GetDpiForWindow(window); font = CreateFontW(-scaled(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        auto context = GetDC(window); auto previous = SelectObject(context, font); TEXTMETRICW metrics{}; GetTextMetricsW(context, &metrics); lineHeight = metrics.tmHeight + metrics.tmExternalLeading; SelectObject(context, previous); ReleaseDC(window, context);
        host = window;
        for (auto value : {L"Map", L"Game", L"Save as"}) { label(value); }
        for (auto id : {Map, Game, Output}) { control(id, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP); }
        hint(Map, L"Choose a .bsp file, or drop one here"); hint(Game, L"Your game's folder, such as GarrysMod\\garrysmod"); hint(Output, L"Where to save the new map");
        for (auto id : {BrowseMap, BrowseGame, BrowseOutput}) { control(id, L"BUTTON", L"Browse...", WS_TABSTOP); }
        control(Build, L"BUTTON", L"Generate cubemaps", WS_TABSTOP | BS_DEFPUSHBUTTON); control(Settings, L"BUTTON", L"Options...", WS_TABSTOP);
        control(Status, L"STATIC", L"", SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS); control(Progress, PROGRESS_CLASSW, L"", PBS_MARQUEE); show(Progress, false);
        control(Preview, L"ScmPreview", L"");
        control(Position, L"STATIC", L"", SS_LEFTNOWORDWRAP); control(Slider, TRACKBAR_CLASSW, L"", TBS_NOTICKS | TBS_DOWNISLEFT | WS_TABSTOP); combo(Variant, variantChoices);
        for (auto id : {Position, Slider, Variant}) { show(id, false); }
        control(Result, L"EDIT", L"", ES_MULTILINE | ES_READONLY | WS_VSCROLL); show(Result, false);
        control(OpenFolder, L"BUTTON", L"Open folder", WS_TABSTOP); control(OpenLog, L"BUTTON", L"Open log", WS_TABSTOP); show(OpenFolder, false); show(OpenLog, false);
        RECT frame{0, 0, scaled(420), scaled(340)}; AdjustWindowRectEx(&frame, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
        options = CreateWindowExW(WS_EX_DLGMODALFRAME, L"SourceCubemapWindow", L"Options", WS_POPUP | WS_CAPTION | WS_SYSMENU, 0, 0, frame.right - frame.left, frame.bottom - frame.top, window, nullptr, GetModuleHandleW(nullptr), this);
        if (!options) { throw std::runtime_error("Cannot create the options window."); }
        host = options;
        place(label(L"Lighting"), 20, 22, 100, 20); combo(Hdr, lightingChoices); place(Hdr, 130, 18, 270, 180);
        place(label(L"Quality"), 20, 56, 100, 20); combo(Quality, qualityChoices); place(Quality, 130, 52, 270, 180);
        for (auto [id, caption, y] : std::initializer_list<std::tuple<int, const wchar_t*, int>>{{Water, L"Water reflections", 92}, {KeepGoing, L"Keep going when something can't be matched exactly", 118}, {Incremental, L"Reuse cubemaps that haven't changed", 144}}) {
            control(id, L"BUTTON", caption, BS_AUTOCHECKBOX | WS_TABSTOP); place(id, 20, y, 380, 24);
        }
        place(label(L"Extra content folders"), 20, 186, 240, 20);
        control(AddMount, L"BUTTON", L"Add folder...", WS_TABSTOP); place(AddMount, 300, 182, 100, 26);
        control(Mounts, L"EDIT", L"", ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL | WS_TABSTOP); place(Mounts, 20, 214, 380, 68);
        control(Done, L"BUTTON", L"Done", WS_TABSTOP | BS_DEFPUSHBUTTON); place(Done, 310, 296, 90, 30);
        host = window;
        check(Water, true); check(KeepGoing, true);
        auto gmod = fs::path(L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\GarrysMod\\garrysmod");
        if (isFile(gmod / "gameinfo.txt")) { set(Game, gmod.wstring()); }
        loadSettings(); DragAcceptFiles(window,TRUE);
        int count=0; auto arguments=CommandLineToArgvW(GetCommandLineW(),&count);
        if (arguments && count==2 && fs::path(arguments[1]).extension()==L".bsp") {
            set(Map,arguments[1]);
        }
        if (arguments) { LocalFree(arguments); }
        layout(); enable(false); refresh();
    }
    void layout() {
        RECT area{}; GetClientRect(window, &area); int width = MulDiv(area.right, 96, dpi), height = MulDiv(area.bottom, 96, dpi);
        for (unsigned i = 0; i < 3; ++i) { int y = 16 + i * 34; place(labels[i], 20, y + 4, 66, 20); place(Map + i, 88, y, width - 200, 26); place(BrowseMap + i, width - 104, y, 84, 26); }
        place(Build, 20, 122, 170, 32); place(Settings, 198, 122, 90, 32); place(Status, 304, 129, width - 324, 20); place(Progress, 20, 164, width - 40, 5);
        constexpr int top = 180;
        if (!finishedOnce) { place(Preview, 20, top, width - 40, height - top - 20); return; }
        int previewWidth = hasCubes() ? (width - 56) * 3 / 5 : 0, side = hasCubes() ? 36 + previewWidth : 20, sideWidth = width - side - 20, row = height - 50;
        place(Preview, 20, top, previewWidth, row - top - 8);
        place(Position, 20, row + 5, 124, 20); place(Slider, 146, row + 2, previewWidth - 212, 26); place(Variant, 20 + previewWidth - 60, row + 2, 60, 200);
        place(Result, side, top, sideWidth, row - top - 8); place(OpenFolder, side, row, 110, 30); place(OpenLog, saved ? side + 118 : side, row, 90, 30);
        fitResult();
    }
    void fitResult() {
        auto handle = controls.at(Result); RECT inner{}; SendMessageW(handle, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&inner));
        ShowScrollBar(handle, SB_VERT, SendMessageW(handle, EM_GETLINECOUNT, 0, 0) * lineHeight > inner.bottom - inner.top);
    }
    std::wstring problem() const {
        auto map = fs::path(text(Map)), game = fs::path(text(Game)), output = fs::path(text(Output));
        if (map.empty()) { return L"Choose a map to get started."; }
        if (!isFile(map)) { return L"That map file doesn't exist."; }
        auto info = readMapInfo(map);
        if (!info) { return L"That file isn't a Source map."; }
        if (!info->cubemaps) { return L"This map has no cubemaps. Add env_cubemap entities and compile it again."; }
        if (game.empty()) { return L"Choose your game's folder."; }
        if (!isFile(game / "gameinfo.txt")) { return L"Choose the game folder that contains gameinfo.txt."; }
        if (output.empty()) { return L"Choose where to save the new map."; }
        std::error_code ignored;
        if (fs::equivalent(map, output, ignored)) { return L"Save under a different name, so your original map stays as it is."; }
        return {};
    }
    void mapChanged() {
        auto map = fs::path(text(Map));
        if (text(Output).empty() || text(Output) == derivedOutput) {
            derivedOutput = map.has_filename() ? (map.parent_path() / (map.stem().wstring() + L"_cubemaps.bsp")).wstring() : std::wstring{};
            set(Output, derivedOutput);
        }
        if (isFile(map)) {
            for (auto folder = map.parent_path(); folder != folder.parent_path(); folder = folder.parent_path()) {
                if (isFile(folder / "gameinfo.txt")) { set(Game, folder.wstring()); break; }
            }
        }
        refresh();
    }
    void dropped(HDROP drop) {
        std::wstring path(32768, 0); path.resize(DragQueryFileW(drop, 0, path.data(), static_cast<UINT>(path.size()))); DragFinish(drop);
        if (!busy && !_wcsicmp(fs::path(path).extension().c_str(), L".bsp")) { set(Map, path); }
    }
    void refresh() {
        if (busy) { return; }
        auto reason = problem(); EnableWindow(controls.at(Build), reason.empty());
        if (!reason.empty()) { set(Status, reason); return; }
        auto info = readMapInfo(text(Map));
        set(Status, std::format(L"{} {} found.", info->cubemaps, info->cubemaps == 1 ? L"cubemap" : L"cubemaps"));
    }
    void enable(bool running) {
        busy = running;
        for (auto id : {Map,Game,Output,BrowseMap,BrowseGame,BrowseOutput,Settings}) { EnableWindow(controls.at(id), !running); }
        set(Build, running ? L"Cancel" : L"Generate cubemaps"); show(Progress, running);
        if (running) { EnableWindow(controls.at(Build), TRUE); }
        else { EnableWindow(controls.at(Build), problem().empty()); }
    }
    void marquee(bool enabled) {
        auto handle = controls.at(Progress); auto style = GetWindowLongPtrW(handle, GWL_STYLE);
        SetWindowLongPtrW(handle, GWL_STYLE, enabled ? style | PBS_MARQUEE : style & ~PBS_MARQUEE); SendMessageW(handle, PBM_SETMARQUEE, enabled, 40);
    }
    void log(std::wstring value) {
        if (value.ends_with(L'\r')) { value.pop_back(); }
        if (value.starts_with(L"Error: ")) { failure = value.substr(7); return; }
        if (!value.starts_with(L"{")) { return; }
        auto event = Json::parse(utf8(value), nullptr, false);
        if (event.is_discarded() || event.value("event", "") != "progress") { return; }
        std::map<std::string,std::wstring> names{{"parsing",L"Reading the map"},{"starting_renderer",L"Starting the graphics card"},{"loading_ldr_assets",L"Loading textures and models"},{"loading_hdr_assets",L"Loading textures and models"},{"preparing_gpu",L"Preparing the scene"},{"capturing_ldr",L"Rendering cubemaps"},{"capturing_hdr",L"Rendering HDR cubemaps"},{"writing_bsp",L"Saving the map"},{"validating_output",L"Checking the saved map"}};
        auto stage = event.value("stage", ""); auto name = names.contains(stage) ? names.at(stage) : wide(stage);
        unsigned total = event.value("total",0u), current = event.value("current",0u);
        if (total) { marquee(false); SendMessageW(controls.at(Progress),PBM_SETRANGE32,0,total); SendMessageW(controls.at(Progress),PBM_SETPOS,current + 1,0); name += std::format(L": {} of {}", current + 1, total); }
        else { marquee(true); }
        set(Status, name);
    }
    void start() {
        if (busy) { return; }
        auto input = text(Map), output = text(Output); auto game = fs::path(text(Game));
        bool replacing = isFile(output);
        if (replacing && MessageBoxW(window,std::format(L"{} already exists. Replace it?",fs::path(output).filename().wstring()).c_str(),L"SourceCubemap",MB_YESNO|MB_ICONQUESTION)!=IDYES) { return; }
        GUID id{}; CoCreateGuid(&id); wchar_t identifier[40]{}; StringFromGUID2(id,identifier,40);
        auto directory = stateDirectory() / "jobs" / identifier; fs::create_directories(directory);
        auto report = directory / "report.json", previewDirectory = directory / "previews";
        std::vector<std::wstring> arguments{L"build",input,L"--json",report.wstring(),L"--progress-json",L"--game",game.wstring(),L"--profile",game.filename() == L"garrysmod" ? L"gmod" : L"hl2"};
        std::wistringstream mounts(text(Mounts)); std::wstring mount;
        while (std::getline(mounts,mount)) { if (!mount.empty() && mount.back()==L'\r') { mount.pop_back(); } if (!mount.empty()) { arguments.insert(arguments.end(),{L"--mount",mount}); } }
        if (cancellation.value) { CloseHandle(cancellation.value); cancellation.value=nullptr; }
        std::wstring eventName=L"Local\\SourceCubemapCancel"+std::wstring(identifier);
        cancellation.value=CreateEventW(nullptr,TRUE,FALSE,eventName.c_str()); if (!cancellation.value) { throw std::runtime_error("Cannot create the cancellation event."); }
        arguments.insert(arguments.end(),{L"--output",output,L"--hdr",chosen(Hdr,lightingChoices),L"--quality",chosen(Quality,qualityChoices),L"--previews",previewDirectory.wstring(),L"--cancel-event",eventName});
        if (checked(KeepGoing)) { arguments.emplace_back(L"--experimental"); if (checked(Water)) { arguments.insert(arguments.end(),{L"--water",L"planar"}); } }
        if (checked(Incremental)) { arguments.emplace_back(L"--incremental"); }
        if (replacing) { arguments.emplace_back(L"--overwrite"); }
        clearCubes(); InvalidateRect(controls.at(Preview),nullptr,TRUE);
        logPath=directory/"worker.log"; failure.clear(); saved=false; showResults(false);
        saveSettings(); set(Status,L"Starting..."); enable(true); marquee(true);
        worker=std::jthread(runWorker,window,std::move(arguments),report,previewDirectory);
    }
    void showResults(bool visible) {
        finishedOnce = visible;
        show(Result,visible); show(OpenLog,visible); show(OpenFolder,visible && saved);
        show(Preview,!visible || hasCubes()); layout();
    }
    std::wstring describe(const Json& report) const {
        std::wstring lighting; std::vector<std::string> warnings; std::set<std::string> seen; std::vector<bool> variants;
        for (const auto& scene : report.at("scenes")) {
            variants.push_back(scene.at("hdr").get<bool>());
            for (const auto& warning : scene.at("geometry").at("warnings")) { if (seen.insert(warning.get<std::string>()).second) { warnings.push_back(warning.get<std::string>()); } }
        }
        lighting = variants.size() > 1 ? L"LDR and HDR" : variants.front() ? L"HDR" : L"LDR";
        unsigned count = report.value("probes",0u);
        auto summary = std::format(L"{} {}, {} lighting.\r\n\r\nSaved as {}\r\n\r\nCopy it into your game's maps folder and keep this filename, because the cubemaps are stored under it.", count, count == 1 ? L"cubemap" : L"cubemaps", lighting, fs::path(text(Output)).filename().wstring());
        if (!warnings.empty()) {
            summary += std::format(L"\r\n\r\n{} {} could not be matched exactly:", warnings.size(), warnings.size() == 1 ? L"thing" : L"things");
            for (const auto& warning : warnings) { summary += L"\r\n- " + wide(warning); }
        }
        return summary;
    }
    void finished(const JobResult& result) {
        if (worker.joinable()) { worker.join(); }
        marquee(false);
        if (!result.error.empty()) { failure = result.error; }
        bool succeeded = result.exitCode == 0 && result.error.empty(), cancelled = failure.starts_with(L"Cancelled");
        enable(false);
        if (cancelled) { set(Status,L"Cancelled. Nothing was saved."); }
        else if (!succeeded) {
            auto reason = failure.empty() ? std::wstring(L"The generator stopped unexpectedly. The log has the details.") : failure;
            if (reason.find(L"--experimental") != std::wstring::npos) { reason += L"\r\n\r\nTo continue past this, turn on \"Keep going when something can't be matched exactly\" in Options."; }
            set(Status,L"Could not generate cubemaps."); set(Result,reason); showResults(true);
        } else {
            std::ifstream input(result.report); auto report=Json::parse(input);
            set(Status,std::format(L"Finished in {:.1f} seconds.",report.at("timings_seconds").at("total").get<double>()));
            set(Result,describe(report));
            loadCubes(result.previews); saved=true; showResults(true); selectVariant();
        }
        if (closeWhenDone) { DestroyWindow(window); }
    }
    bool hasCubes() const { return !cubes[0].empty() || !cubes[1].empty(); }
    size_t variant() const { return cubes[0].empty() || (!cubes[1].empty() && SendMessageW(controls.at(Variant),CB_GETCURSEL,0,0)==1); }
    void clearCubes() {
        cubes[0].clear(); cubes[1].clear(); for (auto& face : faces) { face.reset(); }
        SendMessageW(controls.at(Slider),TBM_SETPOS,TRUE,0);
        for (auto id : {Position,Slider,Variant}) { show(id,false); }
    }
    void loadCubes(const fs::path& folder) {
        clearCubes(); previewFolder=folder; std::error_code ignored;
        for (const auto& entry : fs::directory_iterator(folder,ignored)) {
            auto name=entry.path().filename().wstring();
            if (!name.ends_with(L"_px.png") || name.starts_with(L"cubemapdefault")) { continue; }
            name.resize(name.size()-7); cubes[name.ends_with(L".hdr")].push_back(name);
        }
        std::sort(cubes[0].begin(),cubes[0].end()); std::sort(cubes[1].begin(),cubes[1].end());
    }
    void selectVariant() {
        if (!hasCubes()) { return; }
        auto count=cubes[variant()].size();
        SendMessageW(controls.at(Slider),TBM_SETRANGEMIN,FALSE,0); SendMessageW(controls.at(Slider),TBM_SETRANGEMAX,TRUE,static_cast<LPARAM>(count-1));
        show(Position,true); show(Slider,count>1); show(Variant,!cubes[0].empty() && !cubes[1].empty()); selectCube();
    }
    void selectCube() {
        const auto& names=cubes[variant()]; auto index=std::min(static_cast<size_t>(SendMessageW(controls.at(Slider),TBM_GETPOS,0,0)),names.size()-1);
        for (size_t i=0;i<6;++i) {
            auto file=previewFolder/std::format(L"{}_{}.png",names[index],faceViews[i].suffix);
            faces[i]=std::make_unique<Gdiplus::Bitmap>(file.c_str()); if (faces[i]->GetLastStatus()==Gdiplus::Ok) { faces[i]->RotateFlip(faceViews[i].turn); }
        }
        set(Position,std::format(L"Cubemap {} of {}",index+1,names.size())); InvalidateRect(controls.at(Preview),nullptr,FALSE);
    }
    void saveSettings() {
        Json settings;
        for (auto [id,key] : textSettings) { settings[key]=utf8(text(id)); }
        for (auto [id,key] : flagSettings) { settings[key]=checked(id); }
        settings["lighting"]=utf8(chosen(Hdr,lightingChoices)); settings["quality"]=utf8(chosen(Quality,qualityChoices));
        auto temporary=stateDirectory()/std::format("gui-settings-{}.tmp",GetCurrentProcessId()); std::ofstream out(temporary); out<<settings.dump(2); out.close();
        if (!MoveFileExW(temporary.c_str(),(stateDirectory()/"gui-settings.json").c_str(),MOVEFILE_REPLACE_EXISTING)) { throw std::runtime_error("Cannot save GUI settings."); }
    }
    void loadSettings() {
        std::ifstream input(stateDirectory()/"gui-settings.json"); if (!input) { return; }
        auto settings=Json::parse(input,nullptr,false); if (!settings.is_object()) { return; }
        auto stored=[&](const char* key) { return settings.contains(key) && settings[key].is_string() ? wide(settings[key].get<std::string>()) : std::wstring{}; };
        for (auto [id,key] : textSettings) { if (settings.contains(key) && settings[key].is_string()) { set(id,stored(key)); } }
        for (auto [id,key] : flagSettings) { if (settings.contains(key) && settings[key].is_boolean()) { check(id,settings[key].get<bool>()); } }
        choose(Hdr,lightingChoices,stored("lighting")); choose(Quality,qualityChoices,stored("quality"));
        EnableWindow(controls.at(Water),checked(KeepGoing));
    }
    void openOptions() {
        RECT frame{}, box{}; GetWindowRect(window,&frame); GetWindowRect(options,&box);
        SetWindowPos(options,nullptr,(frame.left+frame.right-box.right+box.left)/2,(frame.top+frame.bottom-box.bottom+box.top)/2,0,0,SWP_NOSIZE|SWP_NOZORDER);
        EnableWindow(window,FALSE); ShowWindow(options,SW_SHOW); SetFocus(controls.at(Done));
    }
    void closeOptions() { EnableWindow(window,TRUE); ShowWindow(options,SW_HIDE); SetActiveWindow(window); }
    void command(int id, int notification) {
        if (id==Variant && notification==CBN_SELCHANGE) { selectVariant(); return; }
        if (id==Map && notification==EN_CHANGE) { mapChanged(); return; }
        if ((id==Game || id==Output) && notification==EN_CHANGE) { refresh(); return; }
        if (notification!=BN_CLICKED) { return; }
        if (id==BrowseMap) { auto path=::choose(window,false,false); if (!path.empty()) { set(Map,path); } }
        else if (id==BrowseGame) { auto path=::choose(window,true,false); if (!path.empty()) { set(Game,path); } }
        else if (id==BrowseOutput) { auto path=::choose(window,false,true,text(Output)); if (!path.empty()) { set(Output,path); } }
        else if (id==Build && busy) { if (cancellation.value) { SetEvent(cancellation.value); set(Status,L"Cancelling..."); EnableWindow(controls.at(Build),FALSE); } }
        else if (id==Build) { start(); }
        else if (id==Settings) { openOptions(); }
        else if (id==Done || id==IDOK || id==IDCANCEL) { if (IsWindowVisible(options)) { closeOptions(); } }
        else if (id==KeepGoing) { EnableWindow(controls.at(Water),checked(KeepGoing)); }
        else if (id==AddMount) {
            auto path=::choose(options,true,false); if (path.empty()) { return; }
            auto value=text(Mounts); if (!value.empty() && !value.ends_with(L'\n')) { value+=L"\r\n"; } set(Mounts,value+path);
        }
        else if (id==OpenFolder && !text(Output).empty()) { ShellExecuteW(window,L"open",fs::path(text(Output)).parent_path().c_str(),nullptr,nullptr,SW_SHOWNORMAL); }
        else if (id==OpenLog && !logPath.empty()) { ShellExecuteW(window,L"open",logPath.c_str(),nullptr,nullptr,SW_SHOWNORMAL); }
    }
};

LRESULT CALLBACK previewProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message==WM_ERASEBKGND) { return 1; }
    if (message==WM_PAINT) {
        PAINTSTRUCT paint{}; auto context=BeginPaint(window,&paint); auto* app=reinterpret_cast<App*>(GetWindowLongPtrW(GetParent(window),GWLP_USERDATA));
        RECT area{}; GetClientRect(window,&area);
        Gdiplus::Bitmap canvas(area.right,area.bottom); Gdiplus::Graphics graphics(&canvas); graphics.Clear(Gdiplus::Color(255,243,243,243));
        if (app && app->faces[0]) {
            int size=std::min(area.right/4,area.bottom/3), left=(area.right-size*4)/2, top=(area.bottom-size*3)/2;
            Gdiplus::ImageAttributes edges; edges.SetWrapMode(Gdiplus::WrapModeTileFlipXY);
            graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBilinear); graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
            for (size_t i=0;i<6;++i) {
                auto& image=*app->faces[i]; if (image.GetLastStatus()!=Gdiplus::Ok) { continue; }
                graphics.DrawImage(&image,Gdiplus::Rect(left+faceViews[i].column*size,top+faceViews[i].row*size,size,size),0,0,image.GetWidth(),image.GetHeight(),Gdiplus::UnitPixel,&edges);
            }
        }
        Gdiplus::Graphics(context).DrawImage(&canvas,0,0);
        if (app && !app->faces[0]) { auto previous=SelectObject(context,app->font); SetTextColor(context,RGB(110,110,110)); SetBkMode(context,TRANSPARENT); DrawTextW(context,L"Your cubemaps will appear here.",-1,&area,DT_CENTER|DT_VCENTER|DT_SINGLELINE); SelectObject(context,previous); }
        EndPaint(window,&paint); return 0;
    }
    return DefWindowProcW(window,message,wparam,lparam);
}
LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* app=reinterpret_cast<App*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    try {
        if (message==WM_NCCREATE) { app=static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams); if (!app->window) { app->window=window; } SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(app)); }
        if (!app) { return DefWindowProcW(window,message,wparam,lparam); }
        bool main=window==app->window;
        if (message==WM_CREATE) { if (main) { app->initialize(); } return 0; }
        if (message==WM_SIZE && main && !app->labels.empty()) { app->layout(); return 0; }
        if (message==WM_GETMINMAXINFO && main) { auto* limits=reinterpret_cast<MINMAXINFO*>(lparam); limits->ptMinTrackSize={app->scaled(640),app->scaled(500)}; return 0; }
        if (message==WM_COMMAND) { app->command(LOWORD(wparam),HIWORD(wparam)); return 0; }
        if (message==WM_HSCROLL && reinterpret_cast<HWND>(lparam)==app->controls.at(Slider)) { app->selectCube(); return 0; }
        if (message==WM_CTLCOLORSTATIC || message==WM_CTLCOLORBTN) {
            SetBkColor(reinterpret_cast<HDC>(wparam),GetSysColor(COLOR_WINDOW)); SetBkMode(reinterpret_cast<HDC>(wparam),TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        }
        if (message==logMessage) { std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lparam)); app->log(std::move(*text)); return 0; }
        if (message==finishedMessage) { std::unique_ptr<JobResult> result(reinterpret_cast<JobResult*>(lparam)); app->finished(*result); return 0; }
        if (message==WM_CLOSE) {
            if (!main) { app->closeOptions(); return 0; }
            if (app->busy) { app->closeWhenDone=true; if (app->cancellation.value) { SetEvent(app->cancellation.value); } app->set(Status,L"Closing..."); return 0; }
            app->saveSettings(); DestroyWindow(window); return 0;
        }
        if (message==WM_DROPFILES) { app->dropped(reinterpret_cast<HDROP>(wparam)); return 0; }
        if (message==WM_DESTROY) { if (main) { PostQuitMessage(0); } return 0; }
    } catch (const std::exception& error) { MessageBoxW(window,wide(error.what()).c_str(),L"SourceCubemap",MB_OK|MB_ICONERROR); if (app && message==finishedMessage) { app->enable(false); } }
    return DefWindowProcW(window,message,wparam,lparam);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE); CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX common{sizeof(common),ICC_PROGRESS_CLASS|ICC_STANDARD_CLASSES|ICC_BAR_CLASSES}; InitCommonControlsEx(&common);
    Gdiplus::GdiplusStartupInput graphics; ULONG_PTR token=0; Gdiplus::GdiplusStartup(&token,&graphics,nullptr);
    int result=1;
    {
        App app;
        WNDCLASSW previewClass{}; previewClass.lpfnWndProc=previewProcedure; previewClass.hInstance=instance; previewClass.lpszClassName=L"ScmPreview"; RegisterClassW(&previewClass);
        WNDCLASSW mainClass{}; mainClass.lpfnWndProc=windowProcedure; mainClass.hInstance=instance; mainClass.lpszClassName=L"SourceCubemapWindow"; mainClass.hCursor=LoadCursorW(nullptr,IDC_ARROW); mainClass.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1); RegisterClassW(&mainClass);
        UINT dpi=GetDpiForSystem(); auto window=CreateWindowExW(0,mainClass.lpszClassName,L"SourceCubemap",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,MulDiv(860,dpi,96),MulDiv(640,dpi,96),nullptr,nullptr,instance,&app);
        if (window) {
            ShowWindow(window,show); UpdateWindow(window); MSG message{};
            while (GetMessageW(&message,nullptr,0,0)>0) { if (!IsDialogMessageW(GetAncestor(message.hwnd,GA_ROOT),&message)) { TranslateMessage(&message); DispatchMessageW(&message); } }
            result=static_cast<int>(message.wParam);
        }
    }
    Gdiplus::GdiplusShutdown(token); CoUninitialize(); return result;
}
