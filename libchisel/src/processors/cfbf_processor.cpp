//
// Created by Giuseppe Francione on 24/03/26.
//

#include "../../include/cfbf_processor.hpp"
#include "../../include/logger.hpp"
#include <stdexcept>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#include <objbase.h>
#include <shlwapi.h>
#include <vector>


#pragma comment(lib, "Ole32.lib")

namespace {

    struct CfbfStream {
        UINT64 size;
        IStream* source;
        IStream* dest;
        IStorage* sourceParent;
        IStorage* destParent;
    };

    // mtime gets bumped back to "now" by writes, so defer SetElementTimes until just before commit
    struct PendingStorageTimes {
        IStorage* storage;
        FILETIME ctime;
        FILETIME atime;
        FILETIME mtime;
    };

    // recursively copy topology from one storage to another
    void copyTopology(IStorage& source,
                      IStorage& dest,
                      std::vector<CfbfStream>& streamsToCopy,
                      std::vector<IStorage*>& storagesToClose,
                      std::vector<PendingStorageTimes>& pendingTimes) {
        IEnumSTATSTG* childs;
        if (FAILED(source.EnumElements(0, nullptr, 0, &childs))) {
            throw std::runtime_error("cfbf: cannot enumerate storage elements");
        }

        ULONG childrenActuallyFetched;
        STATSTG child;
        while (S_OK == childs->Next(1, &child, &childrenActuallyFetched)) {
            if (STGTY_STREAM == child.type) {
                CfbfStream stream;
                stream.size = child.cbSize.QuadPart;

                // open the stream for exclusive reading (prevents snapshots)
                if (FAILED(source.OpenStream(child.pwcsName, nullptr, STGM_DIRECT | STGM_READ | STGM_SHARE_EXCLUSIVE, 0, &stream.source))) {
                    CoTaskMemFree(child.pwcsName);
                    throw std::runtime_error("cfbf: cannot open stream");
                }
                stream.sourceParent = &source;
                source.AddRef();

                // create a new stream with exclusive access
                if (FAILED(dest.CreateStream(child.pwcsName, STGM_DIRECT | STGM_CREATE | STGM_READWRITE | STGM_SHARE_EXCLUSIVE, 0, 0, &stream.dest))) {
                    CoTaskMemFree(child.pwcsName);
                    throw std::runtime_error("cfbf: cannot create stream");
                }
                stream.destParent = &dest;
                dest.AddRef();

                // streams have no SetElementTimes support (IStream lacks it, parent call returns STG_E_ACCESSDENIED)

                streamsToCopy.push_back(stream);

            } else if (STGTY_STORAGE == child.type) {
                IStorage* sourceStorage;
                if (FAILED(source.OpenStorage(child.pwcsName, nullptr, STGM_DIRECT | STGM_READ | STGM_SHARE_EXCLUSIVE, nullptr, 0, &sourceStorage))) {
                    CoTaskMemFree(child.pwcsName);
                    throw std::runtime_error("cfbf: cannot open storage");
                }

                IStorage* destinationStorage;
                if (FAILED(dest.CreateStorage(child.pwcsName, STGM_DIRECT | STGM_CREATE | STGM_READWRITE | STGM_SHARE_EXCLUSIVE, 0, 0, &destinationStorage))) {
                    sourceStorage->Release();
                    CoTaskMemFree(child.pwcsName);
                    throw std::runtime_error("cfbf: cannot create storage");
                }

                // preserve the sub-storage's class id; its times are queued in pendingTimes for later
                if (FAILED(destinationStorage->SetClass(child.clsid))) {
                    sourceStorage->Release();
                    destinationStorage->Release();
                    CoTaskMemFree(child.pwcsName);
                    throw std::runtime_error("cfbf: cannot set storage class id");
                }
                pendingTimes.push_back({destinationStorage, child.ctime, child.atime, child.mtime});

                copyTopology(*sourceStorage, *destinationStorage, streamsToCopy, storagesToClose, pendingTimes);

                // keep the storage open until all data is written
                storagesToClose.push_back(destinationStorage);
                storagesToClose.push_back(sourceStorage);
            }

            // free COM string allocation
            CoTaskMemFree(child.pwcsName);
        }
        childs->Release();
    }

    bool verifyIdentical(IStorage& l, IStorage& r) {
        IEnumSTATSTG* childs;
        if (FAILED(l.EnumElements(0, nullptr, 0, &childs))) {
            throw std::runtime_error("cfbf: cannot enumerate elements for verification");
        }

        ULONG childrenActuallyFetched;
        STATSTG child;
        while (S_OK == childs->Next(1, &child, &childrenActuallyFetched)) {
            if (STGTY_STREAM == child.type) {
                IStream* leftStream;
                if (FAILED(l.OpenStream(child.pwcsName, nullptr, STGM_DIRECT | STGM_READ | STGM_SHARE_EXCLUSIVE, 0, &leftStream))) {
                    CoTaskMemFree(child.pwcsName);
                    return false;
                }

                IStream* rightStream;
                if (FAILED(r.OpenStream(child.pwcsName, nullptr, STGM_DIRECT | STGM_READ | STGM_SHARE_EXCLUSIVE, 0, &rightStream))) {
                    leftStream->Release();
                    CoTaskMemFree(child.pwcsName);
                    return false;
                }

                STATSTG rightStat;
                if (FAILED(rightStream->Stat(&rightStat, STATFLAG_NONAME))) {
                    leftStream->Release();
                    rightStream->Release();
                    CoTaskMemFree(child.pwcsName);
                    return false;
                }

                if (child.cbSize.QuadPart != rightStat.cbSize.QuadPart ||
                    child.clsid != rightStat.clsid ||
                    child.ctime.dwHighDateTime != rightStat.ctime.dwHighDateTime ||
                    child.ctime.dwLowDateTime != rightStat.ctime.dwLowDateTime ||
                    child.mtime.dwHighDateTime != rightStat.mtime.dwHighDateTime ||
                    child.mtime.dwLowDateTime != rightStat.mtime.dwLowDateTime) {
                    leftStream->Release();
                    rightStream->Release();
                    CoTaskMemFree(child.pwcsName);
                    return false;
                }

                leftStream->Release();
                rightStream->Release();

            } else if (STGTY_STORAGE == child.type) {
                IStorage* leftStorage;
                if (FAILED(l.OpenStorage(child.pwcsName, nullptr, STGM_DIRECT | STGM_READ | STGM_SHARE_EXCLUSIVE, nullptr, 0, &leftStorage))) {
                    CoTaskMemFree(child.pwcsName);
                    return false;
                }

                IStorage* rightStorage;
                if (FAILED(r.OpenStorage(child.pwcsName, nullptr, STGM_DIRECT | STGM_READ | STGM_SHARE_EXCLUSIVE, nullptr, 0, &rightStorage))) {
                    leftStorage->Release();
                    CoTaskMemFree(child.pwcsName);
                    return false;
                }

                STATSTG rightStat;
                if (FAILED(rightStorage->Stat(&rightStat, STATFLAG_NONAME))) {
                    leftStorage->Release();
                    rightStorage->Release();
                    CoTaskMemFree(child.pwcsName);
                    return false;
                }

                if (child.clsid != rightStat.clsid ||
                    child.ctime.dwHighDateTime != rightStat.ctime.dwHighDateTime ||
                    child.ctime.dwLowDateTime != rightStat.ctime.dwLowDateTime ||
                    child.mtime.dwHighDateTime != rightStat.mtime.dwHighDateTime ||
                    child.mtime.dwLowDateTime != rightStat.mtime.dwLowDateTime) {
                    leftStorage->Release();
                    rightStorage->Release();
                    CoTaskMemFree(child.pwcsName);
                    return false;
                }

                if (!verifyIdentical(*leftStorage, *rightStorage)) {
                    leftStorage->Release();
                    rightStorage->Release();
                    CoTaskMemFree(child.pwcsName);
                    return false;
                }

                leftStorage->Release();
                rightStorage->Release();
            }

            CoTaskMemFree(child.pwcsName);
        }
        childs->Release();
        return true;
    }

    // deep byte-for-byte comparison for raw_equal(), unlike verifyIdentical()'s fast metadata-only check
    bool compareContentIdentical(IStorage& l, IStorage& r) {
        IEnumSTATSTG* childs;
        if (FAILED(l.EnumElements(0, nullptr, 0, &childs))) {
            return false;
        }

        bool identical = true;
        ULONG childrenActuallyFetched;
        STATSTG child;
        while (identical && S_OK == childs->Next(1, &child, &childrenActuallyFetched)) {
            if (STGTY_STREAM == child.type) {
                IStream* leftStream;
                IStream* rightStream;
                if (FAILED(l.OpenStream(child.pwcsName, nullptr, STGM_DIRECT | STGM_READ | STGM_SHARE_EXCLUSIVE, 0, &leftStream))) {
                    identical = false;
                } else if (FAILED(r.OpenStream(child.pwcsName, nullptr, STGM_DIRECT | STGM_READ | STGM_SHARE_EXCLUSIVE, 0, &rightStream))) {
                    leftStream->Release();
                    identical = false;
                } else {
                    STATSTG rightStat;
                    std::vector<uint8_t> leftData(static_cast<size_t>(child.cbSize.QuadPart));
                    std::vector<uint8_t> rightData;
                    ULONG leftRead = 0, rightRead = 0;
                    if (FAILED(rightStream->Stat(&rightStat, STATFLAG_NONAME)) || rightStat.cbSize.QuadPart != child.cbSize.QuadPart) {
                        identical = false;
                    } else {
                        rightData.resize(static_cast<size_t>(rightStat.cbSize.QuadPart));
                        identical = SUCCEEDED(leftStream->Read(leftData.data(), static_cast<ULONG>(leftData.size()), &leftRead)) && leftRead == leftData.size() &&
                                    SUCCEEDED(rightStream->Read(rightData.data(), static_cast<ULONG>(rightData.size()), &rightRead)) && rightRead == rightData.size() &&
                                    leftData == rightData;
                    }
                    leftStream->Release();
                    rightStream->Release();
                }
            } else if (STGTY_STORAGE == child.type) {
                IStorage* leftStorage;
                IStorage* rightStorage;
                if (FAILED(l.OpenStorage(child.pwcsName, nullptr, STGM_DIRECT | STGM_READ | STGM_SHARE_EXCLUSIVE, nullptr, 0, &leftStorage))) {
                    identical = false;
                } else if (FAILED(r.OpenStorage(child.pwcsName, nullptr, STGM_DIRECT | STGM_READ | STGM_SHARE_EXCLUSIVE, nullptr, 0, &rightStorage))) {
                    leftStorage->Release();
                    identical = false;
                } else {
                    STATSTG rightStat;
                    identical = SUCCEEDED(rightStorage->Stat(&rightStat, STATFLAG_NONAME)) &&
                                child.clsid == rightStat.clsid &&
                                compareContentIdentical(*leftStorage, *rightStorage);
                    leftStorage->Release();
                    rightStorage->Release();
                }
            }
            CoTaskMemFree(child.pwcsName);
        }
        childs->Release();
        return identical;
    }

    // helper to get a long-path compatible wstring for Windows
    std::wstring getLongPath(const std::filesystem::path& p) {
        std::error_code ec;
        auto abs_path = std::filesystem::absolute(p, ec);
        if (ec) return p.wstring();
        return L"\\\\?\\" + abs_path.wstring();
    }

    void optimizeCFBF(const std::filesystem::path& source_path, const std::filesystem::path& dest_path, bool largeSectors) {
        std::wstring sourcePath = getLongPath(source_path);
        std::wstring destPath = getLongPath(dest_path);

        IStorage* source;
        if (FAILED(StgOpenStorage(sourcePath.c_str(), nullptr, STGM_DIRECT | STGM_READ | STGM_SHARE_DENY_WRITE, nullptr, 0, &source))) {
            throw std::runtime_error("cfbf: cannot open source stream");
        }


        STGOPTIONS opt = { 1, 0, 4096 };
        IStorage* destination;
        if (FAILED(StgCreateStorageEx(destPath.c_str(), STGM_CREATE | STGM_READWRITE | STGM_SHARE_EXCLUSIVE, STGFMT_DOCFILE, 0, largeSectors ? &opt : nullptr, nullptr, IID_IStorage, (void**)&destination))) {
            source->Release();
            throw std::runtime_error("cfbf: cannot create target file");
        }

        // copy class id (element times are queued and applied later, see PendingStorageTimes)
        STATSTG statStg;
        if (FAILED(source->Stat(&statStg, 0))) {
            destination->Release();
            source->Release();
            throw std::runtime_error("cfbf: cannot stat source root storage");
        }
        if (FAILED(destination->SetClass(statStg.clsid))) {
            destination->Release();
            source->Release();
            throw std::runtime_error("cfbf: cannot set root class id");
        }

        std::vector<CfbfStream> streams;
        std::vector<IStorage*> storagesToClose;
        std::vector<PendingStorageTimes> pendingTimes;

        try {
            // 1. copy topology
            copyTopology(*source, *destination, streams, storagesToClose, pendingTimes);

            // 2. copy small streams first (less than 4096 B)
            for (auto& stream : streams) {
                if (stream.size < 4096) {
                    ULARGE_INTEGER nread, nwritten;
                    ULARGE_INTEGER size; size.QuadPart = stream.size;
                    if (FAILED(stream.source->CopyTo(stream.dest, size, &nread, &nwritten)) || stream.size != nread.QuadPart || stream.size != nwritten.QuadPart) {
                        throw std::runtime_error("cfbf: cannot copy small stream");
                    }
                }
            }

            // 3. copy large streams
            for (auto& stream : streams) {
                if (stream.size >= 4096) {
                    ULARGE_INTEGER nread, nwritten;
                    ULARGE_INTEGER size; size.QuadPart = stream.size;
                    if (FAILED(stream.source->CopyTo(stream.dest, size, &nread, &nwritten)) || stream.size != nread.QuadPart || stream.size != nwritten.QuadPart) {
                        throw std::runtime_error("cfbf: cannot copy large stream");
                    }
                }
            }

            // 4. commit first: Commit() itself stamps mtime, which would overwrite an earlier SetElementTimes
            if (FAILED(destination->Commit(STGC_OVERWRITE | STGC_CONSOLIDATE))) {
                throw std::runtime_error("cfbf: cannot commit target storage");
            }

            // 5. apply element times now that commit won't touch mtime again
            for (auto& pt : pendingTimes) {
                if (FAILED(pt.storage->SetElementTimes(nullptr, &pt.ctime, &pt.atime, &pt.mtime))) {
                    throw std::runtime_error("cfbf: cannot set storage element times");
                }
            }
            if (FAILED(destination->SetElementTimes(nullptr, &statStg.ctime, &statStg.atime, &statStg.mtime))) {
                throw std::runtime_error("cfbf: cannot set root element times");
            }

        } catch (...) {
            // cleanup on failure
            for (auto& stream : streams) {
                if (stream.source) stream.source->Release();
                if (stream.dest) stream.dest->Release();
                if (stream.sourceParent) stream.sourceParent->Release();
                if (stream.destParent) stream.destParent->Release();
            }
            for (auto* storage : storagesToClose) {
                if (storage) storage->Release();
            }
            destination->Release();
            source->Release();
            throw;
        }

        // cleanup on success
        for (auto& stream : streams) {
            stream.source->Release();
            stream.dest->Release();
            stream.sourceParent->Release();
            stream.destParent->Release();
        }
        for (auto* storage : storagesToClose) {
            storage->Release();
        }

        destination->Release();
        source->Release();

        // 6. verify identical
        IStorage* l;
        if (FAILED(StgOpenStorage(sourcePath.c_str(), nullptr, STGM_DIRECT | STGM_READ | STGM_SHARE_DENY_WRITE, nullptr, 0, &l))) {
            throw std::runtime_error("cfbf: cannot re-open source stream for verification");
        }
        IStorage* r;
        if (FAILED(StgOpenStorage(destPath.c_str(), nullptr, STGM_DIRECT | STGM_READ | STGM_SHARE_DENY_WRITE, nullptr, 0, &r))) {
            l->Release();
            throw std::runtime_error("cfbf: cannot re-open target stream for verification");
        }


        // root storage has no parent entry to compare against, so check its own class id/times first
        STATSTG lStat, rStat;
        bool rootIdentical = SUCCEEDED(l->Stat(&lStat, STATFLAG_NONAME)) &&
                              SUCCEEDED(r->Stat(&rStat, STATFLAG_NONAME)) &&
                              lStat.clsid == rStat.clsid &&
                              lStat.ctime.dwHighDateTime == rStat.ctime.dwHighDateTime &&
                              lStat.ctime.dwLowDateTime == rStat.ctime.dwLowDateTime &&
                              lStat.mtime.dwHighDateTime == rStat.mtime.dwHighDateTime &&
                              lStat.mtime.dwLowDateTime == rStat.mtime.dwLowDateTime;

        bool isIdentical = rootIdentical && verifyIdentical(*l, *r);
        l->Release();
        r->Release();

        if (!isIdentical) {
            throw std::runtime_error("cfbf: output verification failed (result is broken)");
        }
    }

} // anonymous namespace
#endif

namespace chisel {

void CfbfProcessor::recompress(const std::filesystem::path& input,
                               const std::filesystem::path& output,
                               const ProcessingOptions &options) {
#ifdef _WIN32
    Logger::log(LogLevel::Debug, "entering recompress for CFBF optimization", get_name());

    // initialize COM
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool coInit = SUCCEEDED(hr) || hr == S_FALSE; // S_FALSE means it was already initialized, which is fine

    try {
        // usually v4 (4096B sectors) is best used only on very large files,
        // we default to false (512B) to preserve maximum compatibility
        optimizeCFBF(input, output, false);
    } catch (const std::exception& e) {
        if (coInit) CoUninitialize();
        Logger::log(LogLevel::Warning, std::string("cfbf optimization failed (likely corrupt): ") + e.what(), get_name());
        std::filesystem::copy_file(input, output, std::filesystem::copy_options::overwrite_existing);
        return;
    }

    if (coInit) CoUninitialize();
    Logger::log(LogLevel::Debug, "exiting recompress for CFBF optimization", get_name());
#else
    Logger::log(LogLevel::Warning, "cfbf optimization is only supported on windows. acting as passthrough.", get_name());
    std::filesystem::copy_file(input, output, std::filesystem::copy_options::overwrite_existing);
#endif
}

std::optional<ExtractedContent> CfbfProcessor::prepare_extraction(const std::filesystem::path& input_path) {
    // we only support container recompression for CFBF
    return std::nullopt;
}

std::filesystem::path CfbfProcessor::finalize_extraction(const ExtractedContent &content, const ProcessingOptions &options) {
    return {};
}

bool CfbfProcessor::raw_equal(const std::filesystem::path& a, const std::filesystem::path& b) const {
#ifdef _WIN32
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool coInit = SUCCEEDED(hr) || hr == S_FALSE;

    bool result = false;
    try {
        std::wstring pathA = getLongPath(a);
        std::wstring pathB = getLongPath(b);

        IStorage* storageA;
        if (FAILED(StgOpenStorage(pathA.c_str(), nullptr, STGM_DIRECT | STGM_READ | STGM_SHARE_DENY_WRITE, nullptr, 0, &storageA)))
            throw std::runtime_error("cfbf: cannot open first file for verification");

        IStorage* storageB;
        if (FAILED(StgOpenStorage(pathB.c_str(), nullptr, STGM_DIRECT | STGM_READ | STGM_SHARE_DENY_WRITE, nullptr, 0, &storageB))) {
            storageA->Release();
            throw std::runtime_error("cfbf: cannot open second file for verification");
        }

        STATSTG statA, statB;
        result = SUCCEEDED(storageA->Stat(&statA, STATFLAG_NONAME)) &&
                 SUCCEEDED(storageB->Stat(&statB, STATFLAG_NONAME)) &&
                 statA.clsid == statB.clsid &&
                 compareContentIdentical(*storageA, *storageB);

        storageA->Release();
        storageB->Release();
    } catch (const std::exception&) {
        result = false;
    }

    if (coInit) CoUninitialize();
    return result;
#else
    return true;
#endif
}

} // namespace chisel