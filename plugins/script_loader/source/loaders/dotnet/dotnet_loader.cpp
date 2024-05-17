#include <loaders/dotnet/dotnet_loader.hpp>

#include <stdexcept>

#if defined(OS_WINDOWS)
    #define STRING(str) L##str
#else
    #define STRING(str) str
#endif

#include <array>

#include <nethost.h>
#include <coreclr_delegates.h>
#include <hostfxr.h>
#include <imgui.h>
#include <hex/api/plugin_manager.hpp>

#include <hex/helpers/fs.hpp>
#include <wolv/io/fs.hpp>
#include <wolv/utils/guards.hpp>
#include <wolv/utils/string.hpp>
#include <hex/helpers/fmt.hpp>
#include <hex/helpers/logger.hpp>
#include <hex/helpers/utils.hpp>

extern "C" void igSetCurrentContext(ImGuiContext* ctx);

namespace hex::script::loader {

    namespace {

        using get_hostfxr_path_fn = int(*)(char_t * buffer, size_t * buffer_size, const get_hostfxr_parameters *parameters);

        hostfxr_initialize_for_runtime_config_fn hostfxr_initialize_for_runtime_config  = nullptr;
        hostfxr_get_runtime_delegate_fn hostfxr_get_runtime_delegate = nullptr;
        hostfxr_close_fn hostfxr_close = nullptr;
        hostfxr_set_runtime_property_value_fn hostfxr_set_runtime_property_value = nullptr;
        hostfxr_set_error_writer_fn hostfxr_set_error_writer = nullptr;

        void* pInvokeOverride(const char *libraryName, const char *symbolName) {
            auto library = std::string_view(libraryName);
            if (library == "cimgui") {
                return getExport<void*>(ImHexApi::System::getLibImHexModuleHandle(), symbolName);
            } else if (library == "ImHex") {
                return getExport<void*>(hex::getContainingModule((void*)&pInvokeOverride), symbolName);
            }

            return nullptr;
        }

        bool loadHostfxr() {
            #if defined(OS_WINDOWS)
                auto netHostLibrary = loadLibrary(L"nethost.dll");
            #elif defined(OS_LINUX)
                auto netHostLibrary = loadLibrary("libnethost.so");
            #elif defined(OS_MACOS)
                void *netHostLibrary = nullptr;
                for (const auto &pluginPath : fs::getDefaultPaths(fs::ImHexPath::Plugins)) {
                    auto frameworksPath = pluginPath.parent_path().parent_path() / "Frameworks";

                    netHostLibrary = loadLibrary((frameworksPath / "libnethost.dylib").c_str());
                    if (netHostLibrary != nullptr)
                        break;
                }
            #endif

            if (netHostLibrary == nullptr) {
                log::error("Could not load libnethost!");
                return false;
            }

            auto get_hostfxr_path_ptr = getExport<get_hostfxr_path_fn>(netHostLibrary, "get_hostfxr_path");

            std::array<char_t, 300> buffer = { };
            size_t bufferSize = buffer.size();

            auto result = get_hostfxr_path_ptr(buffer.data(), &bufferSize, nullptr);
            if (result != 0) {
                log::error(hex::format("Could not get hostfxr path! 0x{:X}", result));
                return false;
            }

            void *hostfxrLibrary = loadLibrary(buffer.data());
            if (hostfxrLibrary == nullptr) {
                log::error("Could not load hostfxr library!");
                return false;
            }

            {
                hostfxr_initialize_for_runtime_config
                        = getExport<hostfxr_initialize_for_runtime_config_fn>(hostfxrLibrary, "hostfxr_initialize_for_runtime_config");
                hostfxr_get_runtime_delegate
                        = getExport<hostfxr_get_runtime_delegate_fn>(hostfxrLibrary, "hostfxr_get_runtime_delegate");
                hostfxr_close
                        = getExport<hostfxr_close_fn>(hostfxrLibrary, "hostfxr_close");
                hostfxr_set_runtime_property_value
                        = getExport<hostfxr_set_runtime_property_value_fn>(hostfxrLibrary, "hostfxr_set_runtime_property_value");
                hostfxr_set_error_writer
                        = getExport<hostfxr_set_error_writer_fn>(hostfxrLibrary, "hostfxr_set_error_writer");
            }

            hostfxr_set_error_writer([] HOSTFXR_CALLTYPE (const char_t *message) {
                #if defined(OS_WINDOWS)
                    log::error("{}", utf16ToUtf8(message));
                #else
                    log::error("{}", message);
                #endif
            });

            return
                hostfxr_initialize_for_runtime_config != nullptr &&
                hostfxr_get_runtime_delegate != nullptr &&
                hostfxr_close != nullptr &&
                hostfxr_set_runtime_property_value != nullptr &&
                hostfxr_set_error_writer != nullptr;
        }

        load_assembly_and_get_function_pointer_fn getLoadAssemblyFunction(const std::fs::path &path) {
            load_assembly_and_get_function_pointer_fn loadAssemblyFunction = nullptr;

            hostfxr_handle ctx = nullptr;

            u32 result = hostfxr_initialize_for_runtime_config(path.c_str(), nullptr, &ctx);
            ON_SCOPE_EXIT {
                hostfxr_close(ctx);
            };

            if (result > 2 || ctx == nullptr) {
                throw std::runtime_error(hex::format("Failed to initialize command line 0x{:X}", result));
            }

            #if defined (OS_WINDOWS)
                hostfxr_set_runtime_property_value(ctx, STRING("PINVOKE_OVERRIDE"), utf8ToUtf16(hex::format("{}", (void*)pInvokeOverride)).c_str());
            #else
                hostfxr_set_runtime_property_value(ctx, STRING("PINVOKE_OVERRIDE"), hex::format("{}", (void*)pInvokeOverride).c_str());
            #endif

            result = hostfxr_get_runtime_delegate(
                ctx,
                hostfxr_delegate_type::hdt_load_assembly_and_get_function_pointer,
                reinterpret_cast<void**>(&loadAssemblyFunction)
            );

            if (result != 0 || loadAssemblyFunction == nullptr) {
                throw std::runtime_error(hex::format("Failed to get load_assembly_and_get_function_pointer delegate 0x{:X}", result));
            }

            return loadAssemblyFunction;
        }
    }


    bool DotNetLoader::initialize() {
        if (!loadHostfxr()) {
            log::error("Failed to initialize dotnet loader, could not load hostfxr");
            return false;
        }

        for (const auto& path : hex::fs::getDefaultPaths(hex::fs::ImHexPath::Plugins)) {
            auto assemblyLoader = path / "AssemblyLoader.dll";
            if (!wolv::io::fs::exists(assemblyLoader))
                continue;

            auto loadAssembly = getLoadAssemblyFunction(std::fs::absolute(path) / "AssemblyLoader.runtimeconfig.json");

            auto dotnetType = STRING("ImHex.EntryPoint, AssemblyLoader");

            const char_t *dotnetTypeMethod = STRING("ExecuteScript");
            this-> m_assemblyLoaderPathString = assemblyLoader.native();

            component_entry_point_fn entryPoint = nullptr;
            u32 result = loadAssembly(
                    m_assemblyLoaderPathString.c_str(),
                    dotnetType,
                    dotnetTypeMethod,
                    nullptr,
                    nullptr,
                    reinterpret_cast<void**>(&entryPoint)
            );

            if (result != 0 || entryPoint == nullptr) {
                log::error("Failed to load assembly loader '{}'! 0x{:X}", assemblyLoader.string(), result);
                continue;
            }

            m_runMethod = [entryPoint](const std::string &methodName, bool keepLoaded, const std::fs::path &path) -> int {
                auto pathString = wolv::util::toUTF8String(path);

                auto string = hex::format("{}||{}||{}", keepLoaded ? "LOAD" : "EXEC", methodName, pathString);
                auto result = entryPoint(string.data(), string.size());

                return result;
            };

            m_methodExists = [entryPoint](const std::string &methodName, const std::fs::path &path) -> bool {
                auto pathString = wolv::util::toUTF8String(path);

                auto string = hex::format("CHECK||{}||{}", methodName, pathString);
                auto result = entryPoint(string.data(), string.size());

                return result == 0;
            };

            return true;
        }

        return false;
    }

    bool DotNetLoader::loadAll() {
        this->clearScripts();

        for (const auto &imhexPath : hex::fs::getDefaultPaths(hex::fs::ImHexPath::Scripts)) {
            auto directoryPath = imhexPath / "custom" / "dotnet";
            if (!wolv::io::fs::exists(directoryPath))
                wolv::io::fs::createDirectories(directoryPath);

            if (!wolv::io::fs::exists(directoryPath))
                continue;

            for (const auto &entry : std::fs::directory_iterator(directoryPath)) {
                if (!entry.is_directory())
                    continue;

                const auto &scriptPath = entry.path() / "Main.dll";
                if (!std::fs::exists(scriptPath))
                    continue;

                const bool hasMain = m_methodExists("Main", scriptPath);
                const bool hasOnLoad = m_methodExists("OnLoad", scriptPath);

                if (hasMain) {
                    this->addScript(entry.path().stem().string(), false, [this, scriptPath] {
                        hex::unused(m_runMethod("Main", false, scriptPath));
                    });
                } else if (hasOnLoad) {
                    this->addScript(entry.path().stem().string(), true, [] {});
                }

                if (hasOnLoad) {
                    hex::unused(m_runMethod("OnLoad", true, scriptPath));
                }

            }
        }

        return true;
    }

}
