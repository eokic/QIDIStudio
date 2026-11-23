// Public stand-ins for parts of QIDI's unpublished src/slic3r/QIDI/ cloud module (QIDI account, P2P
// transfer to cloud printers, model list). Used when QDT_RELEASE_TO_PUBLIC=0, where there is no QIDI
// account and therefore no cloud printer: every call reports failure and nothing is sent anywhere.
// The declarations follow SimplyPrint's slicer-builds stubs (AGPL-3.0).
#ifndef slic3r_QIDIPublic_QIDICloudStubs_hpp_
#define slic3r_QIDIPublic_QIDICloudStubs_hpp_

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace Slic3r {

// P2PManager.hpp: file transfer to printers reached through the QIDI cloud.
class P2PManager
{
public:
    struct FileTransferOptions
    {
        std::atomic<bool>                    *cancel { nullptr };
        std::function<void(int64_t, int64_t)> progress;
    };
    struct FileTransferResult
    {
        bool ok { false };
        bool cancelled { false };
    };

    static P2PManager &instance()
    {
        static P2PManager manager;
        return manager;
    }
    FileTransferResult uploadFile(const std::string &, const std::string &, const FileTransferOptions &) { return {}; }
};

// QIDINetwork.hpp: QIDI account web API.
class MakerHttpHandle
{
public:
    static MakerHttpHandle &getInstance()
    {
        static MakerHttpHandle handle;
        return handle;
    }
    void get_qidi_model_list(std::function<void(std::string, unsigned)>, std::function<void(std::string, std::string, unsigned)>) {}
};

} // namespace Slic3r

#endif
