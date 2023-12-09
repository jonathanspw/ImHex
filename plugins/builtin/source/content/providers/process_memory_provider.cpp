#if defined(OS_WINDOWS) || defined (OS_LINUX)

#include <content/providers/process_memory_provider.hpp>

#if defined(OS_WINDOWS)
    #include <windows.h>
    #include <psapi.h>
    #include <shellapi.h>
#elif defined(OS_LINUX)
    #include <sys/uio.h>
#endif

#include <imgui.h>
#include <hex/ui/imgui_imhex_extensions.h>
#include <hex/helpers/utils.hpp>
#include <hex/helpers/fmt.hpp>
#include <hex/ui/view.hpp>

#include <wolv/io/fs.hpp>
#include <wolv/io/file.hpp>
#include <wolv/utils/guards.hpp>

namespace hex::plugin::builtin {

    bool ProcessMemoryProvider::open() {
        #if defined(OS_WINDOWS)
            this->m_processHandle = OpenProcess(PROCESS_ALL_ACCESS, FALSE, this->m_selectedProcess->id);
            if (this->m_processHandle == nullptr)
                return false;
        #elif defined(OS_LINUX)
            this->m_processId = pid_t(this->m_selectedProcess->id);
        #endif

        this->reloadProcessModules();

        return true;
    }

    void ProcessMemoryProvider::close() {
        #if defined(OS_WINDOWS)
            CloseHandle(this->m_processHandle);
            this->m_processHandle = nullptr;
        #elif defined(OS_LINUX)
            this->m_processId = -1;
        #endif
    }

    void ProcessMemoryProvider::readRaw(u64 address, void *buffer, size_t size) {
        #if defined(OS_WINDOWS)
            ReadProcessMemory(this->m_processHandle, reinterpret_cast<LPCVOID>(address), buffer, size, nullptr);
        #elif defined(OS_LINUX)
            const iovec local {
                .iov_base = buffer,
                .iov_len = size,
            };
            const iovec remote = {
                .iov_base = reinterpret_cast<void*>(address),
                .iov_len = size,
            };

            auto read = process_vm_readv(this->m_processId, &local, 1, &remote, 1, 0);

            if (read == -1) {
                // TODO error handling strerror(errno)
            }
        #endif
    }
    void ProcessMemoryProvider::writeRaw(u64 address, const void *buffer, size_t size) {
        #if defined(OS_WINDOWS)
            WriteProcessMemory(this->m_processHandle, reinterpret_cast<LPVOID>(address), buffer, size, nullptr);
        #elif defined(OS_LINUX)
            const iovec local {
                .iov_base = const_cast<void*>(buffer),
                .iov_len = size,
            };
            const iovec remote = {
                .iov_base = reinterpret_cast<void*>(address),
                .iov_len = size,
            };

            auto read = process_vm_writev(this->m_processId, &local, 1, &remote, 1, 0);
            if (read == -1) {
                // TODO error handling strerror(errno)
            }
        #endif
    }

    std::pair<Region, bool> ProcessMemoryProvider::getRegionValidity(u64 address) const {
        for (const auto &memoryRegion : this->m_memoryRegions) {
            if (memoryRegion.region.overlaps({ address, 1 }))
                return { memoryRegion.region, true };
        }

        Region lastRegion = Region::Invalid();
        for (const auto &memoryRegion : this->m_memoryRegions) {

            if (address < memoryRegion.region.getStartAddress())
                return { Region { lastRegion.getEndAddress() + 1, memoryRegion.region.getStartAddress() - lastRegion.getEndAddress() }, false };

            lastRegion = memoryRegion.region;
        }

        return { Region::Invalid(), false };
    }

    bool ProcessMemoryProvider::drawLoadInterface() {
        if (this->m_processes.empty() && !this->m_enumerationFailed) {
            #if defined(OS_WINDOWS)
                DWORD numProcesses = 0;
                std::vector<DWORD> processIds;

                do {
                    processIds.resize(processIds.size() + 1024);
                    if (EnumProcesses(processIds.data(), processIds.size() * sizeof(DWORD), &numProcesses) == FALSE) {
                        processIds.clear();
                        this->m_enumerationFailed = true;
                        break;
                    }
                } while (numProcesses == processIds.size() * sizeof(DWORD));

                processIds.resize(numProcesses / sizeof(DWORD));

                auto dc = GetDC(nullptr);
                ON_SCOPE_EXIT { ReleaseDC(nullptr, dc); };
                for (auto processId : processIds) {
                    HANDLE processHandle = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, processId);
                    if (processHandle == nullptr)
                        continue;

                    ON_SCOPE_EXIT { CloseHandle(processHandle); };

                    char processName[MAX_PATH];
                    if (GetModuleBaseNameA(processHandle, nullptr, processName, MAX_PATH) == 0)
                        continue;

                    ImGuiExt::Texture texture;
                    {
                        HMODULE moduleHandle = nullptr;
                        DWORD numModules = 0;
                        if (EnumProcessModules(processHandle, &moduleHandle, sizeof(HMODULE), &numModules) != FALSE) {
                            char modulePath[MAX_PATH];
                            if (GetModuleFileNameExA(processHandle, moduleHandle, modulePath, MAX_PATH) != FALSE) {
                                SHFILEINFOA fileInfo;
                                if (SHGetFileInfoA(modulePath, 0, &fileInfo, sizeof(SHFILEINFOA), SHGFI_ICON | SHGFI_SMALLICON) > 0) {
                                    ON_SCOPE_EXIT { DestroyIcon(fileInfo.hIcon); };

                                    ICONINFO iconInfo;
                                    if (GetIconInfo(fileInfo.hIcon, &iconInfo) != FALSE) {
                                        ON_SCOPE_EXIT { DeleteObject(iconInfo.hbmColor); DeleteObject(iconInfo.hbmMask); };

                                        BITMAP bitmap;
                                        if (GetObject(iconInfo.hbmColor, sizeof(BITMAP), &bitmap) > 0) {
                                            BITMAPINFO bitmapInfo = { };
                                            bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                                            bitmapInfo.bmiHeader.biWidth = bitmap.bmWidth;
                                            bitmapInfo.bmiHeader.biHeight = -bitmap.bmHeight;
                                            bitmapInfo.bmiHeader.biPlanes = 1;
                                            bitmapInfo.bmiHeader.biBitCount = 32;
                                            bitmapInfo.bmiHeader.biCompression = BI_RGB;

                                            std::vector<u32> pixels(bitmap.bmWidth * bitmap.bmHeight * 4);
                                            if (GetDIBits(dc, iconInfo.hbmColor, 0, bitmap.bmHeight, pixels.data(), &bitmapInfo, DIB_RGB_COLORS) > 0) {
                                                for (auto &pixel : pixels)
                                                    pixel = (pixel & 0xFF00FF00) | ((pixel & 0xFF) << 16) | ((pixel & 0xFF0000) >> 16);

                                                texture = ImGuiExt::Texture(reinterpret_cast<const u8*>(pixels.data()), pixels.size(), ImGuiExt::Texture::Filter::Nearest, bitmap.bmWidth, bitmap.bmHeight);
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    this->m_processes.push_back({ u32(processId), processName, std::move(texture) });
                }
            #elif defined(OS_LINUX)
                for (const auto& entry : std::fs::directory_iterator("/proc")) {
                    if (!std::fs::is_directory(entry)) continue;

                    const auto &path = entry.path();
                    u32 processId = 0;
                    try {
                        processId = std::stoi(path.filename());
                    } catch (...) {
                        continue; // not a PID
                    }

                    wolv::io::File file(path /"cmdline", wolv::io::File::Mode::Read);
                    if (!file.isValid())
                        continue;

                    std::string processName = file.readString(0xF'FFFF);

                    this->m_processes.emplace_back(processId, processName, ImGuiExt::Texture());
                }
            #endif
        }

        if (this->m_enumerationFailed) {
            ImGui::TextUnformatted("hex.builtin.provider.process_memory.enumeration_failed"_lang);
        } else {
            ImGui::PushItemWidth(500_scaled);
            const auto &filtered = this->m_processSearchWidget.draw(this->m_processes);
            ImGui::PopItemWidth();
            if (ImGui::BeginTable("##process_table", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY, ImVec2(500_scaled, 500_scaled))) {
                ImGui::TableSetupColumn("##icon");
                ImGui::TableSetupColumn("hex.builtin.provider.process_memory.process_id"_lang);
                ImGui::TableSetupColumn("hex.builtin.provider.process_memory.process_name"_lang);
                ImGui::TableSetupScrollFreeze(0, 1);

                ImGui::TableHeadersRow();

                for (auto &process : filtered) {
                    ImGui::PushID(process);

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Image(process->icon, process->icon.getSize());

                    ImGui::TableNextColumn();
                    ImGui::Text("%d", process->id);

                    ImGui::TableNextColumn();
                    if (ImGui::Selectable(process->name.c_str(), this->m_selectedProcess != nullptr && process->id == this->m_selectedProcess->id, ImGuiSelectableFlags_SpanAllColumns, ImVec2(0, process->icon.getSize().y)))
                        this->m_selectedProcess = process;

                    ImGui::PopID();
                }

                ImGui::EndTable();
            }

        }

        return this->m_selectedProcess != nullptr;
    }

    void ProcessMemoryProvider::drawInterface() {
        ImGuiExt::Header("hex.builtin.provider.process_memory.memory_regions"_lang, true);

        auto availableX = ImGui::GetContentRegionAvail().x;
        ImGui::PushItemWidth(availableX);
        const auto &filtered = this->m_regionSearchWidget.draw(this->m_memoryRegions);
        ImGui::PopItemWidth();

        #if defined(OS_WINDOWS)
            auto availableY = 400_scaled;
        #else
            // Take up full height on Linux since there are no DLL injection controls
            auto availableY = ImGui::GetContentRegionAvail().y;
        #endif

        if (ImGui::BeginTable("##module_table", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY, ImVec2(availableX, availableY))) {
            ImGui::TableSetupColumn("hex.builtin.common.region"_lang);
            ImGui::TableSetupColumn("hex.builtin.common.size"_lang);
            ImGui::TableSetupColumn("hex.builtin.common.name"_lang);
            ImGui::TableSetupScrollFreeze(0, 1);

            ImGui::TableHeadersRow();

            for (const auto &memoryRegion : filtered) {
                ImGui::PushID(&memoryRegion);

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGuiExt::TextFormatted("0x{0:016X} - 0x{1:016X}", memoryRegion->region.getStartAddress(), memoryRegion->region.getEndAddress());

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(hex::toByteString(memoryRegion->region.getSize()).c_str());


                ImGui::TableNextColumn();
                if (ImGui::Selectable(memoryRegion->name.c_str(), false, ImGuiSelectableFlags_SpanAllColumns))
                    ImHexApi::HexEditor::setSelection(memoryRegion->region);

                ImGui::PopID();
            }

            ImGui::EndTable();
        }

        #if defined(OS_WINDOWS)
            ImGuiExt::Header("hex.builtin.provider.process_memory.utils"_lang);

            if (ImGui::Button("hex.builtin.provider.process_memory.utils.inject_dll"_lang)) {
                hex::fs::openFileBrowser(fs::DialogMode::Open, { { "DLL File", "dll" } }, [this](const std::fs::path &path) {
                    const auto &dllPath = path.native();
                    const auto dllPathLength = (dllPath.length() + 1) * sizeof(std::fs::path::value_type);

                    if (auto pathAddress = VirtualAllocEx(this->m_processHandle, nullptr, dllPathLength, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); pathAddress != nullptr) {
                        if (WriteProcessMemory(this->m_processHandle, pathAddress, dllPath.c_str(), dllPathLength, nullptr) != FALSE) {
                            auto loadLibraryW = reinterpret_cast<LPTHREAD_START_ROUTINE>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryW")));
                            if (loadLibraryW != nullptr) {
                                if (auto threadHandle = CreateRemoteThread(this->m_processHandle, nullptr, 0, loadLibraryW, pathAddress, 0, nullptr); threadHandle != nullptr) {
                                    WaitForSingleObject(threadHandle, INFINITE);
                                    RequestOpenErrorPopup::post(hex::format("hex.builtin.provider.process_memory.utils.inject_dll.success"_lang, path.filename().string()));
                                    this->reloadProcessModules();
                                    CloseHandle(threadHandle);
                                    return;
                                }
                            }
                        }
                    }

                    RequestOpenErrorPopup::post(hex::format("hex.builtin.provider.process_memory.utils.inject_dll.failure"_lang, path.filename().string()));
                });
            }
        #endif
    }

    void ProcessMemoryProvider::reloadProcessModules() {
        this->m_memoryRegions.clear();

        #if defined(OS_WINDOWS)
            DWORD numModules = 0;
            std::vector<HMODULE> modules;

            do {
                modules.resize(modules.size() + 1024);
                if (EnumProcessModules(this->m_processHandle, modules.data(), modules.size() * sizeof(HMODULE), &numModules) == FALSE) {
                    modules.clear();
                    break;
                }
            } while (numModules == modules.size() * sizeof(HMODULE));

            modules.resize(numModules / sizeof(HMODULE));

            for (auto &module : modules) {
                MODULEINFO moduleInfo;
                if (GetModuleInformation(this->m_processHandle, module, &moduleInfo, sizeof(MODULEINFO)) == FALSE)
                    continue;

                char moduleName[MAX_PATH];
                if (GetModuleFileNameExA(this->m_processHandle, module, moduleName, MAX_PATH) == FALSE)
                    continue;

                this->m_memoryRegions.insert({ { u64(moduleInfo.lpBaseOfDll), size_t(moduleInfo.SizeOfImage) }, std::fs::path(moduleName).filename().string() });
            }

            MEMORY_BASIC_INFORMATION memoryInfo;
            for (u64 address = 0; address < this->getActualSize(); address += memoryInfo.RegionSize) {
                if (VirtualQueryEx(this->m_processHandle, reinterpret_cast<LPCVOID>(address), &memoryInfo, sizeof(MEMORY_BASIC_INFORMATION)) == 0)
                    break;

                std::string name;
                if (memoryInfo.State & MEM_IMAGE)   continue;
                if (memoryInfo.State & MEM_FREE)    continue;
                if (memoryInfo.State & MEM_COMMIT)  name += hex::format("{} ", "hex.builtin.provider.process_memory.region.commit"_lang);
                if (memoryInfo.State & MEM_RESERVE) name += hex::format("{} ", "hex.builtin.provider.process_memory.region.reserve"_lang);
                if (memoryInfo.State & MEM_PRIVATE) name += hex::format("{} ", "hex.builtin.provider.process_memory.region.private"_lang);
                if (memoryInfo.State & MEM_MAPPED)  name += hex::format("{} ", "hex.builtin.provider.process_memory.region.mapped"_lang);

                this->m_memoryRegions.insert({ { reinterpret_cast<u64>(memoryInfo.BaseAddress), reinterpret_cast<u64>(memoryInfo.BaseAddress) + memoryInfo.RegionSize }, name });
            }

        #elif defined(OS_LINUX)

            wolv::io::File file(std::fs::path("/proc") / std::to_string(this->m_processId) / "maps", wolv::io::File::Mode::Read);

            if (!file.isValid())
                return;

            for (const auto &line : wolv::util::splitString(file.readString(0xF'FFFF), "\n")) {
                const auto &split = wolv::util::splitString(line, " ");
                if (split.size() < 6)
                    continue;

                const u64 start = std::stoull(split[0].substr(0, split[0].find('-')), nullptr, 16);
                const u64 end   = std::stoull(split[0].substr(split[0].find('-') + 1), nullptr, 16);
                const auto &name = split[5];

                this->m_memoryRegions.insert({ { start, end - start }, name });
            }
        #endif
    }


    std::variant<std::string, i128> ProcessMemoryProvider::queryInformation(const std::string &category, const std::string &argument) {
        auto findRegionByName = [this](const std::string &name) {
            return std::find_if(this->m_memoryRegions.begin(), this->m_memoryRegions.end(),
                [name](const auto &region) {
                    return region.name == name;
                });
        };

        if (category == "region_address") {
            if (auto iter = findRegionByName(argument); iter != this->m_memoryRegions.end())
                return iter->region.getStartAddress();
            else
                return 0;
        } else if (category == "region_size") {
            if (auto iter = findRegionByName(argument); iter != this->m_memoryRegions.end())
                return iter->region.getSize();
            else
                return 0;
        } else if (category == "process_id") {
            return this->m_selectedProcess->id;
        } else if (category == "process_name") {
            return this->m_selectedProcess->name;
        } else
            return Provider::queryInformation(category, argument);
    }

}

#endif