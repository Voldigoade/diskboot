#include "app/service.hpp"
#include <commdlg.h>
#include <algorithm>
#include <memory>
#include <thread>

using namespace diskboot;
namespace {
constexpr unsigned Completed = WM_APP + 1;
constexpr unsigned Phase     = WM_APP + 2;
struct Result {
    std::function<void()> apply;
    std::string           error;
};
struct Theme {
    static constexpr int margin = 24, heading = 24, body = 16;
};
class Window {
    HWND                window_ = nullptr, title_ = nullptr, body_ = nullptr, choose_ = nullptr, lab_ = nullptr;
    HWND                disks_ = nullptr, size_label_ = nullptr, size_ = nullptr, partition_ = nullptr, back_ = nullptr,
                        next_ = nullptr, status_ = nullptr;
    HFONT               font_ = nullptr, heading_ = nullptr;
    std::jthread        worker_;
    bool                busy_ = false;
    int                 step_ = 0;
    Request             request_;
    std::optional<Iso>  image_;
    std::optional<Plan> plan_;
    Json                inventory_;
    std::vector<Partition> sources_;
    int                    scale(int value) const {
        return MulDiv(value, static_cast<int>(GetDpiForWindow(window_)), 96);
    }
    HWND control(const wchar_t *klass, const wchar_t *text, DWORD style, int id) {
        auto result =
            CreateWindowExW(0, klass, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, window_,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
        win_check(result != nullptr, "Create interface control");
        SendMessageW(result, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        return result;
    }
    void text(HWND control, const std::string &value) {
        SetWindowTextW(control, wide(value).c_str());
    }
    std::filesystem::path pick(bool vhd) {
        wchar_t       path[32768]{};
        OPENFILENAMEW dialog{sizeof(dialog)};
        dialog.hwndOwner   = window_;
        dialog.lpstrFile   = path;
        dialog.nMaxFile    = 32768;
        dialog.lpstrFilter = vhd ? L"Disposable virtual disks\0*.vhd;*.vhdx\0\0" : L"Linux ISO images\0*.iso\0\0";
        dialog.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&dialog))
            return path;
        require(CommDlgExtendedError() == 0, "File picker failed");
        return {};
    }
    void task(std::function<void(Result &)> work) {
        busy_ = true;
        render();
        text(status_, step_ == 4 ? "Preparing the selected disposable virtual disk."
                                 : "Inspecting. No storage changes are made during inspection or planning.");
        worker_ = std::jthread([this, work = std::move(work)] {
            auto result = std::make_unique<Result>();
            try {
                ComRuntime runtime;
                work(*result);
            } catch (const std::exception &e) {
                result->error = e.what();
            }
            if (PostMessageW(window_, Completed, 0, reinterpret_cast<LPARAM>(result.get())))
                result.release();
        });
    }
    void inspect() {
        auto path = pick(false);
        if (path.empty())
            return;
        request_.iso = path;
        image_.reset();
        plan_.reset();
        task([this, path](Result &result) {
            IsoImage image(path);
            auto     metadata = image.metadata;
            result.apply      = [this, metadata] { image_ = metadata; };
        });
    }
    void load_disks() {
        task([this](Result &result) {
            auto inventory = disk_inventory();
            result.apply   = [this, inventory] {
                inventory_ = inventory;
                step_      = 1;
                request_.disk_id.clear();
                SendMessageW(disks_, LB_RESETCONTENT, 0, 0);
                for (const auto &entry : inventory_.at("disks")) {
                    const auto disk = entry.at("disk").get<Disk>();
                    auto       label =
                        disk.identity.model + " | " + std::to_string(disk.identity.size / 1000000000) + " GB | ";
                    if (disk.protected_disk)
                        label += "Protected Windows disk / boot partition";
                    else
                        label += entry.at("selectable").get<bool>()
                                     ? "Available"
                                     : "Unavailable: " + entry.at("reason").get<std::string>();
                    auto value = wide(label);
                    SendMessageW(disks_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value.c_str()));
                }
            };
        });
    }
    void select_disk() {
        const auto index = SendMessageW(disks_, LB_GETCURSEL, 0, 0);
        if (index == LB_ERR)
            return;
        const auto &entry = inventory_.at("disks").at(static_cast<std::size_t>(index));
        const auto  disk  = entry.at("disk").get<Disk>();
        if (disk.protected_disk || (!entry.at("selectable").get<bool>() && request_.lab_vhd.empty())) {
            SendMessageW(disks_, LB_SETCURSEL, static_cast<WPARAM>(-1), 0);
            request_.disk_id.clear();
            text(status_, "This disk cannot be selected. " + entry.at("reason").get<std::string>());
            EnableWindow(next_, FALSE);
            return;
        }
        request_.disk_id = entry.at("id").get<std::string>();
        request_.partition_guid.clear();
        sources_.clear();
        SendMessageW(partition_, CB_RESETCONTENT, 0, 0);
        SendMessageW(partition_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Use existing unallocated space"));
        for (const auto &p : disk.partitions)
            if (p.filesystem == "NTFS") {
                sources_.push_back(p);
                auto label =
                    wide(p.label + " | " + std::to_string(p.range.size / 1000000000) + " GB | maximum shrink: " +
                         (p.resize_known ? std::to_string((p.range.size - p.minimum) / 1000000000) + " GB"
                                         : "not established"));
                SendMessageW(partition_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
            }
        SendMessageW(partition_, CB_SETCURSEL, sources_.size() == 1 ? 1 : 0, 0);
        EnableWindow(next_, TRUE);
        text(status_, "Windows boot-chain disks remain protected.");
    }
    void next() {
        if (step_ == 0) {
            load_disks();
            return;
        }
        if (step_ == 1) {
            require(!request_.disk_id.empty(), "Choose an available secondary disk");
            step_ = 2;
            render();
            return;
        }
        if (step_ == 2) {
            wchar_t amount[64]{};
            GetWindowTextW(size_, amount, 64);
            request_.size           = parse_size(utf8(amount) + "GB");
            const auto index        = SendMessageW(partition_, CB_GETCURSEL, 0, 0);
            request_.partition_guid = index > 0 ? sources_.at(static_cast<std::size_t>(index - 1)).guid : "";
            auto request            = request_;
            task([this, request](Result &result) {
                auto plan    = plan_request(request);
                result.apply = [this, plan] {
                    plan_ = plan;
                    step_ = 3;
                };
            });
            return;
        }
        if (step_ == 3 && plan_ && !request_.lab_vhd.empty()) {
            auto message =
                wide(summarize(*plan_) + "\nThis is a disposable VHD/VHDX laboratory operation. Prepare it?");
            if (MessageBoxW(window_, message.c_str(), L"Confirm virtual disk preparation",
                            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
                return;
            const auto request  = request_;
            const auto approved = *plan_;
            step_               = 4;
            task([this, request, approved](Result &result) {
                auto journal = prepare_virtual(request, approved, [this](const std::string &phase) {
                    auto value = std::make_unique<std::string>(phase);
                    if (PostMessageW(window_, Phase, 0, reinterpret_cast<LPARAM>(value.get())))
                        value.release();
                });
                result.apply = [this, journal] {
                    step_ = 5;
                    text(body_,
                         "Laboratory media prepared. Boot and installer behavior remain unverified.\r\nJournal: " +
                             utf8(journal.wstring()));
                };
            });
        }
    }
    void render() {
        const wchar_t *titles[] = {L"Choose Linux ISO",        L"Choose where Linux will be installed",
                                   L"Space for Linux",         L"Review",
                                   L"Preparing virtual media", L"Laboratory result"};
        SetWindowTextW(title_, titles[step_]);
        for (auto c : {choose_, lab_, disks_, size_label_, size_, partition_})
            ShowWindow(c, SW_HIDE);
        ShowWindow(back_, step_ > 0 && step_ < 4 ? SW_SHOW : SW_HIDE);
        ShowWindow(next_, step_ < 4 ? SW_SHOW : SW_HIDE);
        SetWindowTextW(next_, step_ == 3
                                  ? (request_.lab_vhd.empty() ? L"Prepare (unavailable)" : L"Prepare virtual disk")
                                  : L"Next");
        bool enabled = !busy_;
        if (step_ == 0) {
            ShowWindow(choose_, SW_SHOW);
            if (image_)
                text(body_, image_->distribution + "\r\n" + image_->version +
                                "\r\nSize: " + std::to_string(image_->size) + " bytes\r\nSHA-256: " + image_->sha256 +
                                "\r\n\r\n" + image_->limitation);
            else
                text(body_, "Choose an Ubuntu or Kubuntu ISO to inspect its contents and integrity "
                            "hash.\r\n\r\nPhysical preparation is disabled until UEFI installer testing is complete.");
            enabled &= image_.has_value() && image_->candidate;
        } else if (step_ == 1) {
            ShowWindow(disks_, SW_SHOW);
            ShowWindow(lab_, SW_SHOW);
            text(body_, "Windows and boot-chain disks are protected and cannot be selected.\r\nSelect another internal "
                        "SSD. Existing data partitions are preserved.");
            enabled &= !request_.disk_id.empty();
        } else if (step_ == 2) {
            for (auto c : {size_label_, size_, partition_})
                ShowWindow(c, SW_SHOW);
            text(body_,
                 "DiskBoot will use this total space for the Linux installer and the Linux installation.\r\n\r\nChoose "
                 "which NTFS data partition may shrink if sufficient unallocated space does not exist.");
        } else if (step_ == 3 && plan_) {
            text(body_, summarize(*plan_) + "\r\nThe third-party installer can still modify other disks. No live "
                                            "installer disk guard is implemented.");
            enabled &= !request_.lab_vhd.empty();
        } else if (step_ == 4)
            text(body_, "The transaction journal records each real phase. Keep DiskBoot open until the operation "
                        "finishes.\r\nTechnical transaction details are in LocalAppData\\DiskBoot.");
        EnableWindow(next_, enabled);
        EnableWindow(back_, !busy_);
        EnableWindow(choose_, !busy_);
        EnableWindow(lab_, !busy_);
        EnableWindow(disks_, !busy_);
        layout();
    }
    void layout() {
        RECT rect{};
        GetClientRect(window_, &rect);
        const int m = scale(Theme::margin), width = static_cast<int>(rect.right) - 2 * m;
        const int bottom = static_cast<int>(rect.bottom);
        MoveWindow(title_, m, m, width, scale(42), TRUE);
        MoveWindow(body_, m, scale(84), width, step_ == 1 ? scale(72) : std::max(scale(150), bottom - scale(330)),
                   TRUE);
        MoveWindow(choose_, m, rect.bottom - scale(190), scale(180), scale(36), TRUE);
        MoveWindow(disks_, m, scale(175), width, std::max(scale(120), bottom - scale(340)), TRUE);
        MoveWindow(lab_, m, rect.bottom - scale(150), scale(270), scale(32), TRUE);
        MoveWindow(size_label_, m, rect.bottom - scale(245), scale(210), scale(24), TRUE);
        MoveWindow(size_, m, rect.bottom - scale(214), scale(170), scale(32), TRUE);
        MoveWindow(partition_, m, rect.bottom - scale(160), width, scale(200), TRUE);
        MoveWindow(status_, m, rect.bottom - scale(104), width, scale(40), TRUE);
        MoveWindow(back_, m, rect.bottom - scale(48), scale(100), scale(32), TRUE);
        MoveWindow(next_, rect.right - m - scale(205), rect.bottom - scale(48), scale(205), scale(32), TRUE);
    }

public:
    static LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto *self = reinterpret_cast<Window *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self          = static_cast<Window *>(reinterpret_cast<CREATESTRUCTW *>(lparam)->lpCreateParams);
            self->window_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self)
            return DefWindowProcW(hwnd, message, wparam, lparam);
        try {
            return self->message(message, wparam, lparam);
        } catch (const std::exception &e) {
            MessageBoxW(hwnd, wide(e.what()).c_str(), L"DiskBoot", MB_OK | MB_ICONERROR);
            return 0;
        }
    }
    LRESULT message(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_CREATE:
            font_    = CreateFontW(-scale(Theme::body), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0,
                                   CLEARTYPE_QUALITY, 0, L"Segoe UI");
            heading_ = CreateFontW(-scale(Theme::heading), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                   0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
            title_   = control(L"STATIC", L"", 0, 0);
            SendMessageW(title_, WM_SETFONT, reinterpret_cast<WPARAM>(heading_), TRUE);
            body_       = control(L"EDIT", L"", ES_MULTILINE | ES_READONLY | WS_VSCROLL, 0);
            choose_     = control(L"BUTTON", L"Choose ISO...", WS_TABSTOP, 10);
            lab_        = control(L"BUTTON", L"Disposable VHD/VHDX (tests)...", WS_TABSTOP, 11);
            disks_      = control(L"LISTBOX", L"", WS_TABSTOP | LBS_NOTIFY | WS_VSCROLL | WS_BORDER, 12);
            size_label_ = control(L"STATIC", L"Total allocation (GB)", 0, 0);
            size_       = control(L"EDIT", L"500", WS_TABSTOP | ES_NUMBER | WS_BORDER, 13);
            partition_  = control(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 14);
            status_     = control(L"STATIC", L"", 0, 0);
            back_       = control(L"BUTTON", L"Back", WS_TABSTOP, 15);
            next_       = control(L"BUTTON", L"Next", WS_TABSTOP | BS_DEFPUSHBUTTON, 16);
            render();
            return 0;
        case WM_SIZE:
            layout();
            return 0;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO *>(lparam)->ptMinTrackSize = {scale(740), scale(600)};
            return 0;
        case WM_COMMAND:
            if (busy_)
                return 0;
            switch (LOWORD(wparam)) {
            case 10:
                inspect();
                break;
            case 11: {
                auto path = pick(true);
                if (!path.empty()) {
                    request_.lab_vhd = path;
                    text(status_, "Laboratory mode: only the supplied attached virtual disk can be modified.");
                }
                break;
            }
            case 12:
                if (HIWORD(wparam) == LBN_SELCHANGE)
                    select_disk();
                break;
            case 15:
                if (step_ > 0) {
                    --step_;
                    plan_.reset();
                    render();
                }
                break;
            case 16:
                next();
                break;
            }
            return 0;
        case Completed: {
            std::unique_ptr<Result> result(reinterpret_cast<Result *>(lparam));
            busy_ = false;
            if (result->error.empty()) {
                if (result->apply)
                    result->apply();
                render();
                text(status_, "");
            } else {
                if (step_ == 4)
                    step_ = 3;
                render();
                text(status_, result->error);
            }
            return 0;
        }
        case Phase: {
            std::unique_ptr<std::string> phase(reinterpret_cast<std::string *>(lparam));
            if (phase->starts_with("resize"))
                text(status_, "Resizing the selected data partition.");
            else if (phase->starts_with("create"))
                text(status_, "Creating boot and recovery partitions.");
            else if (phase->starts_with("format"))
                text(status_, "Preparing the new partitions.");
            else if (phase->starts_with("boot-files"))
                text(status_, "Copying and verifying installer files.");
            else
                text(status_, "Validating disk and transaction state.");
            return 0;
        }
        case WM_CLOSE:
            if (!busy_)
                DestroyWindow(window_);
            return 0;
        case WM_DESTROY:
            DeleteObject(font_);
            DeleteObject(heading_);
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window_, message, wparam, lparam);
        }
    }
    int run(HINSTANCE instance, int show) {
        WNDCLASSW klass{};
        klass.lpfnWndProc   = procedure;
        klass.hInstance     = instance;
        klass.lpszClassName = L"DiskBootWindow";
        klass.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        klass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        win_check(RegisterClassW(&klass) != 0, "Register interface");
        auto hwnd = CreateWindowExW(0, klass.lpszClassName, L"DiskBoot", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                    CW_USEDEFAULT, 900, 680, nullptr, nullptr, instance, this);
        win_check(hwnd != nullptr, "Create DiskBoot window");
        ShowWindow(hwnd, show);
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0)
            if (!IsDialogMessageW(hwnd, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        return static_cast<int>(message.wParam);
    }
};
} // namespace
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    try {
        Window window;
        return window.run(instance, show);
    } catch (const std::exception &e) {
        MessageBoxW(nullptr, wide(e.what()).c_str(), L"DiskBoot", MB_OK | MB_ICONERROR);
        return 1;
    }
}
