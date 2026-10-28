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

namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
constexpr UINT logMessage = WM_APP + 1, finishedMessage = WM_APP + 2;
enum Control : int { Map = 100, Game, Output, Mounts, Hdr, Quality, Water, Time, Device, Cache, Experimental, Overwrite, Incremental, NoCache, Inspect, Build, Cancel, Validate, OpenOutput, OpenReport, Faces, Summary, Log, Progress, Status, Preview, BrowseMap, BrowseGame, BrowseOutput, BrowseCache };

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
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR | (folder ? FOS_PICKFOLDERS : 0));
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
    HWND window = nullptr;
    std::map<int, HWND> controls;
    std::vector<HWND> labels;
    HFONT font = nullptr, titleFont = nullptr;
    std::jthread worker;
    Handle cancellation;
    bool busy = false, closeWhenDone = false;
    std::wstring operation;
    fs::path reportPath;
    std::vector<fs::path> previews;
    std::unique_ptr<Gdiplus::Bitmap> preview;
    UINT dpi = 96;

    ~App() { if (font) { DeleteObject(font); } if (titleFont) { DeleteObject(titleFont); } }
    int scaled(int value) const { return MulDiv(value, dpi, 96); }
    HWND control(int id, const wchar_t* kind, std::wstring text, DWORD style = 0) {
        auto handle = CreateWindowExW(kind == std::wstring_view(L"EDIT") ? WS_EX_CLIENTEDGE : 0, kind, text.c_str(), WS_CHILD | WS_VISIBLE | style, 0, 0, 1, 1, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
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
    bool checked(int id) const { return SendMessageW(controls.at(id), BM_GETCHECK, 0, 0) == BST_CHECKED; }
    void combo(int id, std::initializer_list<const wchar_t*> values) {
        control(id, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP);
        for (auto value : values) { SendMessageW(controls.at(id), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value)); }
        SendMessageW(controls.at(id), CB_SETCURSEL, 0, 0);
    }
    void initialize() {
        dpi = GetDpiForWindow(window); font = CreateFontW(-scaled(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        titleFont = CreateFontW(-scaled(25), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        label(L"SourceCubemap"); SendMessageW(labels[0], WM_SETFONT, reinterpret_cast<WPARAM>(titleFont), TRUE);
        label(L"Standalone cubemap generation for Source maps");
        for (auto value : {L"Map", L"Game content directory", L"New BSP output", L"Additional mounts (one per line, highest priority first)", L"Lighting", L"Quality", L"Water", L"Capture time (seconds)", L"GPU name filter (optional)", L"Cache directory", L"Inspection and results", L"Captured face preview", L"Build log"}) { label(value); }
        for (auto id : {Map, Game, Output, Time, Device, Cache}) { control(id, L"EDIT", id == Time ? L"0" : L"", ES_AUTOHSCROLL | WS_TABSTOP); }
        control(Mounts, L"EDIT", L"", ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP);
        for (auto [id, caption] : std::initializer_list<std::pair<int,const wchar_t*>>{{BrowseMap,L"Browse..."},{BrowseGame,L"Browse..."},{BrowseOutput,L"Browse..."},{BrowseCache,L"Browse..."},{Inspect,L"Inspect map"},{Build,L"Generate cubemaps"},{Cancel,L"Cancel"},{Validate,L"Validate output"},{OpenOutput,L"Open output folder"},{OpenReport,L"Open report"}}) { control(id, L"BUTTON", caption, WS_TABSTOP | (id == Build ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON)); }
        combo(Hdr, {L"auto", L"ldr", L"hdr", L"both"}); combo(Quality, {L"standard", L"fast", L"high"}); combo(Water, {L"strict", L"planar"});
        for (auto [id, caption] : std::initializer_list<std::pair<int,const wchar_t*>>{{Experimental,L"Allow experimental rendering"},{Overwrite,L"Replace an existing separate output"},{Incremental,L"Reuse unchanged cubemaps"},{NoCache,L"Disable application caches"}}) { control(id, L"BUTTON", caption, BS_AUTOCHECKBOX | WS_TABSTOP); }
        control(Summary, L"EDIT", L"Choose a map, then inspect its probes and dependencies.\r\nGeneration never starts a Source game.", ES_MULTILINE | ES_READONLY | WS_VSCROLL);
        combo(Faces, {}); control(Preview, L"ScmPreview", L"");
        control(Log, L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_HSCROLL);
        SendMessageW(controls.at(Log), EM_SETLIMITTEXT, 200000, 0);
        control(Progress, PROGRESS_CLASSW, L"", PBS_MARQUEE); control(Status, L"STATIC", L"Ready");
        set(Cache, (stateDirectory() / "cache").wstring());
        auto gmod = fs::path(L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\GarrysMod\\garrysmod");
        if (fs::exists(gmod / "gameinfo.txt")) { set(Game, gmod.wstring()); }
        loadSettings();
        int count=0; auto arguments=CommandLineToArgvW(GetCommandLineW(),&count);
        if (arguments && count==2 && fs::path(arguments[1]).extension()==L".bsp") {
            fs::path input(arguments[1]); set(Map,input.wstring()); set(Output,(input.parent_path()/(input.stem().wstring()+L"_cubemaps.bsp")).wstring());
        }
        if (arguments) { LocalFree(arguments); }
        enable(false); layout();
    }
    void layout() {
        RECT area{}; GetClientRect(window, &area); int width = MulDiv(area.right, 96, dpi), height = MulDiv(area.bottom, 96, dpi);
        int right = 406, rightWidth = width - right - 20;
        place(labels[0],20,12,600,34); place(labels[1],20,49,800,23);
        const std::array<int, 4> ys{86,142,198,254};
        for (unsigned i = 0; i < 3; ++i) { place(labels[i+2],20,ys[i],360,20); place(Map+i,20,ys[i]+23,276,26); place(BrowseMap+i,303,ys[i]+23,83,26); }
        place(labels[5],20,ys[3],366,20); place(Mounts,20,ys[3]+23,366,62);
        place(labels[6],20,349,176,20); place(labels[7],210,349,176,20); place(Hdr,20,371,176,180); place(Quality,210,371,176,180);
        place(labels[8],20,409,176,20); place(labels[9],210,409,176,20); place(Water,20,431,176,160); place(Time,210,431,176,26);
        place(labels[10],20,468,366,20); place(Device,20,490,366,26);
        place(labels[11],20,525,366,20); place(Cache,20,547,276,26); place(BrowseCache,303,547,83,26);
        place(Experimental,20,585,366,24); place(Overwrite,20,612,366,24); place(Incremental,20,639,366,24); place(NoCache,20,666,366,24);
        int buttons = std::max(706,height-98); place(Inspect,20,buttons,118,30); place(Build,146,buttons,240,30);
        place(Validate,20,buttons+37,166,28); place(Cancel,194,buttons+37,192,28);
        place(labels[12],right,86,rightWidth,20); place(Summary,right,109,rightWidth,136);
        place(labels[13],right,257,rightWidth,20); place(Faces,right,280,rightWidth,240);
        int pictureHeight = std::max(130,height-540); place(Preview,right,315,rightWidth,pictureHeight);
        int logY = 327+pictureHeight; place(labels[14],right,logY,rightWidth,20); place(Log,right,logY+23,rightWidth,116);
        place(OpenOutput,right,logY+146,190,28); place(OpenReport,right+200,logY+146,160,28);
        place(Status,20,height-29,width-400,22); place(Progress,width-368,height-29,348,18);
    }
    void enable(bool running) {
        busy = running;
        for (auto id : {Map,Game,Output,Mounts,Hdr,Quality,Water,Time,Device,Cache,Experimental,Overwrite,Incremental,NoCache,Inspect,Build,Validate,BrowseMap,BrowseGame,BrowseOutput,BrowseCache}) { EnableWindow(controls.at(id), !running); }
        EnableWindow(controls.at(Cancel), running); EnableWindow(controls.at(OpenReport), !reportPath.empty());
    }
    void appendLog(std::wstring value) {
        auto handle = controls.at(Log); if (GetWindowTextLengthW(handle) > 150000) { SetWindowTextW(handle,L"Earlier lines are in worker.log beside this job's report.\r\n"); }
        SendMessageW(handle, EM_SETSEL, -1, -1); value += L"\r\n"; SendMessageW(handle, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(value.c_str()));
    }
    void log(std::wstring value) {
        if (value.starts_with(L"{")) {
            auto event = Json::parse(utf8(value), nullptr, false);
            if (!event.is_discarded() && event.value("event", "") == "progress") {
                std::map<std::string,std::wstring> names{{"parsing",L"Reading the BSP"},{"starting_renderer",L"Starting the renderer"},{"loading_ldr_assets",L"Loading LDR assets"},{"loading_hdr_assets",L"Loading HDR assets"},{"preparing_gpu",L"Preparing GPU resources"},{"capturing_ldr",L"Capturing LDR cubemaps"},{"capturing_hdr",L"Capturing HDR cubemaps"},{"writing_bsp",L"Writing the new BSP"},{"validating_output",L"Validating the written BSP"}};
                auto stage = event.value("stage", ""); set(Status, names.contains(stage) ? names.at(stage) : wide(stage));
                unsigned total = event.value("total",0u), current = event.value("current",0u);
                if (total) { SendMessageW(controls.at(Progress),PBM_SETMARQUEE,FALSE,0); SetWindowLongPtrW(controls.at(Progress),GWL_STYLE,GetWindowLongPtrW(controls.at(Progress),GWL_STYLE)&~PBS_MARQUEE); SendMessageW(controls.at(Progress),PBM_SETRANGE32,0,total); SendMessageW(controls.at(Progress),PBM_SETPOS,current,0); }
                else { SetWindowLongPtrW(controls.at(Progress),GWL_STYLE,GetWindowLongPtrW(controls.at(Progress),GWL_STYLE)|PBS_MARQUEE); SendMessageW(controls.at(Progress),PBM_SETMARQUEE,TRUE,40); }
                return;
            }
            if (!event.is_discarded() && event.value("event", "") == "complete") { return; }
        }
        appendLog(std::move(value));
    }
    void start(std::wstring command) {
        if (busy) { return; }
        auto input = command == L"validate" ? text(Output) : text(Map);
        if (input.empty() || !fs::is_regular_file(input)) { throw std::runtime_error("Choose an existing BSP map."); }
        if (command == L"build" && !fs::is_directory(text(Game))) { throw std::runtime_error("Choose the game's content directory containing gameinfo.txt."); }
        GUID id{}; CoCreateGuid(&id); wchar_t identifier[40]{}; StringFromGUID2(id,identifier,40);
        auto directory = stateDirectory() / "jobs" / identifier; fs::create_directories(directory);
        auto report = directory / "report.json", previewDirectory = directory / "previews";
        std::vector<std::wstring> arguments{command,input,L"--json",report.wstring(),L"--progress-json"};
        if (command != L"validate" && !text(Game).empty()) {
            auto game = fs::path(text(Game)); arguments.insert(arguments.end(), {L"--game",game.wstring(),L"--profile",game.filename() == L"garrysmod" ? L"gmod" : L"hl2"});
            std::wistringstream mounts(text(Mounts)); std::wstring mount;
            while (std::getline(mounts,mount)) { if (!mount.empty() && mount.back()==L'\r') { mount.pop_back(); } if (!mount.empty()) { arguments.insert(arguments.end(),{L"--mount",mount}); } }
        }
        if (cancellation.value) { CloseHandle(cancellation.value); cancellation.value=nullptr; }
        if (command == L"build") {
            if (text(Output).empty()) { throw std::runtime_error("Choose a new BSP output path."); }
            std::wstring eventName=L"Local\\SourceCubemapCancel"+std::wstring(identifier);
            cancellation.value=CreateEventW(nullptr,TRUE,FALSE,eventName.c_str()); if (!cancellation.value) { throw std::runtime_error("Cannot create the cancellation event."); }
            arguments.insert(arguments.end(),{L"--output",text(Output),L"--hdr",text(Hdr),L"--quality",text(Quality),L"--water",text(Water),L"--capture-time",text(Time),L"--previews",previewDirectory.wstring(),L"--cancel-event",eventName});
            if (!text(Device).empty()) { arguments.insert(arguments.end(),{L"--device",text(Device)}); }
            if (!text(Cache).empty()) { arguments.insert(arguments.end(),{L"--cache",text(Cache)}); }
            for (auto [control,flag] : std::initializer_list<std::pair<int,const wchar_t*>>{{Experimental,L"--experimental"},{Overwrite,L"--overwrite"},{Incremental,L"--incremental"},{NoCache,L"--no-cache"}}) { if (checked(control)) { arguments.emplace_back(flag); } }
        }
        if (command != L"validate") {
            previews.clear(); preview.reset(); SendMessageW(controls.at(Faces),CB_RESETCONTENT,0,0); InvalidateRect(controls.at(Preview),nullptr,TRUE);
        }
        reportPath.clear();
        saveSettings(); operation=command; set(Log,L""); set(Status,L"Starting..."); enable(true);
        EnableWindow(controls.at(Cancel),command==L"build");
        SetWindowLongPtrW(controls.at(Progress),GWL_STYLE,GetWindowLongPtrW(controls.at(Progress),GWL_STYLE)|PBS_MARQUEE);
        SendMessageW(controls.at(Progress),PBM_SETMARQUEE,TRUE,40);
        worker=std::jthread(runWorker,window,std::move(arguments),report,previewDirectory);
    }
    void finished(const JobResult& result) {
        if (worker.joinable()) { worker.join(); }
        SendMessageW(controls.at(Progress),PBM_SETMARQUEE,FALSE,0);
        enable(false);
        if (!result.error.empty()) { appendLog(result.error); }
        if (result.exitCode != 0) { set(Status,L"Stopped. See the build log for the reason."); set(Summary,L"No successful result was reported.\r\nThe build log contains the missing dependency or unsupported feature."); }
        else {
            std::ifstream input(result.report); auto report=Json::parse(input); reportPath=result.report;
            std::ostringstream summary;
            if (operation == L"build") {
                summary << "Structural validation: " << report.value("structural_validation", "unknown") << "\r\n";
                summary << report.value("probes",0) << " probes, " << report.value("textures_written",0) << " textures. Total: " << std::format("{:.3f}",report.at("timings_seconds").at("total").get<double>()) << " seconds\r\n";
                summary << "Rendering: " << report.value("rendering_fidelity", "unknown") << ". Native game acceptance: not performed.\r\n";
                for (const auto& [stage,time] : report.at("timings_seconds").items()) { summary << stage << ": " << std::format("{:.3f}",time.get<double>()) << " s\r\n"; }
                for (const auto& scene : report.at("scenes")) { for (const auto& warning : scene.at("geometry").at("warnings")) { summary << "Experimental: " << warning.get<std::string>() << "\r\n"; } }
                set(Status,L"BSP generated and structurally validated. Review experimental limitations above.");
            } else {
                summary << "BSP version " << report.value("version",0) << " | " << report.at("probes").size() << " cubemap probes\r\n";
                summary << "Validation: " << report.value("validation","not requested") << "\r\n";
                for (const auto& probe : report.at("probes")) { summary << probe.at("name").get<std::string>() << " | " << probe.at("resolution") << " pixels | " << probe.at("origin").dump() << "\r\n"; }
                if (report.contains("assets")) { for (const auto& mount : report.at("assets").at("mounts")) { summary << "Mount: " << mount.get<std::string>() << "\r\n"; } }
                set(Status,operation==L"validate" ? L"Structural validation passed." : L"Map inspected. Review probe sizes and mounted content above.");
            }
            set(Summary,wide(summary.str()));
            if (operation==L"build") {
                previews.clear(); preview.reset(); SendMessageW(controls.at(Faces),CB_RESETCONTENT,0,0);
                if (fs::exists(result.previews)) { for (const auto& entry : fs::directory_iterator(result.previews)) { if (entry.path().extension()==L".png") { previews.push_back(entry.path()); } } }
                std::sort(previews.begin(),previews.end());
                for (const auto& path : previews) { SendMessageW(controls.at(Faces),CB_ADDSTRING,0,reinterpret_cast<LPARAM>(path.filename().c_str())); }
                if (!previews.empty()) { SendMessageW(controls.at(Faces),CB_SETCURSEL,0,0); selectPreview(); }
            }
        }
        enable(false); if (closeWhenDone) { DestroyWindow(window); }
    }
    void selectPreview() {
        auto index=SendMessageW(controls.at(Faces),CB_GETCURSEL,0,0);
        if (index>=0 && static_cast<size_t>(index)<previews.size()) { preview=std::make_unique<Gdiplus::Bitmap>(previews[index].c_str()); }
        InvalidateRect(controls.at(Preview),nullptr,TRUE);
    }
    void saveSettings() {
        Json settings;
        for (auto id : {Map,Game,Output,Mounts,Hdr,Quality,Water,Time,Device,Cache}) { settings[std::to_string(id)]=utf8(text(id)); }
        for (auto id : {Experimental,Overwrite,Incremental,NoCache}) { settings[std::to_string(id)]=checked(id); }
        auto temporary=stateDirectory()/std::format("gui-settings-{}.tmp",GetCurrentProcessId()); std::ofstream out(temporary); out<<settings.dump(2); out.close();
        if (!MoveFileExW(temporary.c_str(),(stateDirectory()/"gui-settings.json").c_str(),MOVEFILE_REPLACE_EXISTING)) { throw std::runtime_error("Cannot save GUI settings."); }
    }
    void loadSettings() {
        std::ifstream input(stateDirectory()/"gui-settings.json"); if (!input) { return; }
        auto settings=Json::parse(input,nullptr,false); if (!settings.is_object()) { return; }
        for (auto id : {Map,Game,Output,Mounts,Time,Device,Cache}) { if (settings.contains(std::to_string(id)) && settings[std::to_string(id)].is_string()) { set(id,wide(settings[std::to_string(id)].get<std::string>())); } }
        for (auto id : {Hdr,Quality,Water}) { auto key=std::to_string(id); if (settings.contains(key) && settings[key].is_string()) { auto value=wide(settings[key].get<std::string>()); SendMessageW(controls.at(id),CB_SELECTSTRING,-1,reinterpret_cast<LPARAM>(value.c_str())); } }
        for (auto id : {Experimental,Overwrite,Incremental,NoCache}) { auto key=std::to_string(id); if (settings.contains(key) && settings[key].is_boolean()) { SendMessageW(controls.at(id),BM_SETCHECK,settings[key].get<bool>()?BST_CHECKED:BST_UNCHECKED,0); } }
    }
    void command(int id, int notification) {
        if (id==Faces && notification==CBN_SELCHANGE) { selectPreview(); return; }
        if (notification!=BN_CLICKED) { return; }
        if (id==BrowseMap) {
            auto path=choose(window,false,false); if (path.empty()) { return; } set(Map,path); auto output=fs::path(path); output.replace_filename(output.stem().wstring()+L"_cubemaps.bsp"); set(Output,output.wstring());
        } else if (id==BrowseGame || id==BrowseCache) { auto path=choose(window,true,false); if (!path.empty()) { set(id==BrowseGame?Game:Cache,path); } }
        else if (id==BrowseOutput) { auto path=choose(window,false,true,text(Output)); if (!path.empty()) { set(Output,path); } }
        else if (id==Inspect) { start(L"inspect"); } else if (id==Build) { start(L"build"); } else if (id==Validate) { start(L"validate"); }
        else if (id==Cancel && cancellation.value) { SetEvent(cancellation.value); set(Status,L"Cancelling at the next safe boundary. Publication already in progress will finish."); EnableWindow(controls.at(Cancel),FALSE); }
        else if (id==OpenOutput && !text(Output).empty()) { ShellExecuteW(window,L"open",fs::path(text(Output)).parent_path().c_str(),nullptr,nullptr,SW_SHOWNORMAL); }
        else if (id==OpenReport && !reportPath.empty()) { ShellExecuteW(window,L"open",reportPath.c_str(),nullptr,nullptr,SW_SHOWNORMAL); }
    }
};

LRESULT CALLBACK previewProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message==WM_PAINT) {
        PAINTSTRUCT paint{}; auto context=BeginPaint(window,&paint); auto* app=reinterpret_cast<App*>(GetWindowLongPtrW(GetParent(window),GWLP_USERDATA));
        Gdiplus::Graphics graphics(context); graphics.Clear(Gdiplus::Color(255,30,32,36)); RECT area{}; GetClientRect(window,&area);
        if (app && app->preview && app->preview->GetLastStatus()==Gdiplus::Ok) {
            auto& image=*app->preview; float scale=std::min(float(area.right)/image.GetWidth(),float(area.bottom)/image.GetHeight());
            float width=image.GetWidth()*scale,height=image.GetHeight()*scale;
            graphics.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
            graphics.DrawImage(&image,Gdiplus::RectF((area.right-width)*0.5f,(area.bottom-height)*0.5f,width,height));
        } else { SetTextColor(context,RGB(205,210,218)); SetBkMode(context,TRANSPARENT); DrawTextW(context,L"Rendered faces appear here after generation.",-1,&area,DT_CENTER|DT_VCENTER|DT_SINGLELINE); }
        EndPaint(window,&paint); return 0;
    }
    return DefWindowProcW(window,message,wparam,lparam);
}
LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* app=reinterpret_cast<App*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    try {
        if (message==WM_NCCREATE) { app=static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams); app->window=window; SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(app)); }
        if (!app) { return DefWindowProcW(window,message,wparam,lparam); }
        if (message==WM_CREATE) { app->initialize(); return 0; }
        if (message==WM_SIZE && !app->controls.empty()) { app->layout(); return 0; }
        if (message==WM_GETMINMAXINFO) { auto* limits=reinterpret_cast<MINMAXINFO*>(lparam); limits->ptMinTrackSize={app->scaled(1040),app->scaled(850)}; return 0; }
        if (message==WM_COMMAND) { app->command(LOWORD(wparam),HIWORD(wparam)); return 0; }
        if (message==WM_CTLCOLORSTATIC || message==WM_CTLCOLORBTN) {
            SetBkColor(reinterpret_cast<HDC>(wparam),GetSysColor(COLOR_WINDOW)); SetBkMode(reinterpret_cast<HDC>(wparam),TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        }
        if (message==logMessage) { std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lparam)); app->log(std::move(*text)); return 0; }
        if (message==finishedMessage) { std::unique_ptr<JobResult> result(reinterpret_cast<JobResult*>(lparam)); app->finished(*result); return 0; }
        if (message==WM_CLOSE) {
            if (app->busy) { app->closeWhenDone=true; if (app->cancellation.value) { SetEvent(app->cancellation.value); } app->set(Status,L"Closing after the worker reaches a safe boundary..."); return 0; }
            app->saveSettings(); DestroyWindow(window); return 0;
        }
        if (message==WM_DESTROY) { PostQuitMessage(0); return 0; }
    } catch (const std::exception& error) { MessageBoxW(window,wide(error.what()).c_str(),L"SourceCubemap",MB_OK|MB_ICONERROR); if (app && message==finishedMessage) { app->enable(false); } }
    return DefWindowProcW(window,message,wparam,lparam);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE); CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX common{sizeof(common),ICC_PROGRESS_CLASS|ICC_STANDARD_CLASSES}; InitCommonControlsEx(&common);
    Gdiplus::GdiplusStartupInput graphics; ULONG_PTR token=0; Gdiplus::GdiplusStartup(&token,&graphics,nullptr);
    int result=1;
    {
        App app;
        WNDCLASSW previewClass{}; previewClass.lpfnWndProc=previewProcedure; previewClass.hInstance=instance; previewClass.lpszClassName=L"ScmPreview"; RegisterClassW(&previewClass);
        WNDCLASSW mainClass{}; mainClass.lpfnWndProc=windowProcedure; mainClass.hInstance=instance; mainClass.lpszClassName=L"SourceCubemapWindow"; mainClass.hCursor=LoadCursorW(nullptr,IDC_ARROW); mainClass.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1); RegisterClassW(&mainClass);
        UINT dpi=GetDpiForSystem(); auto window=CreateWindowExW(0,mainClass.lpszClassName,L"SourceCubemap",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,MulDiv(1120,dpi,96),MulDiv(860,dpi,96),nullptr,nullptr,instance,&app);
        if (window) {
            ShowWindow(window,show); UpdateWindow(window); MSG message{};
            while (GetMessageW(&message,nullptr,0,0)>0) { if (!IsDialogMessageW(window,&message)) { TranslateMessage(&message); DispatchMessageW(&message); } }
            result=static_cast<int>(message.wParam);
        }
    }
    Gdiplus::GdiplusShutdown(token); CoUninitialize(); return result;
}
