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

    // recursively copy topology from one storage to another
    void copyTopology(IStorage& source,
                      IStorage& dest,
                      std::vector<CfbfStream>& streamsToCopy,
                      std::vector<IStorage*>& storagesToClose) {
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

                copyTopology(*sourceStorage, *destinationStorage, streamsToCopy, storagesToClose);

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

    void optimizeCFBF(const std::wstring& sourcePath, const std::wstring& destPath, bool largeSectors) {
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

        // copy class id
        STATSTG statStg;
        source->Stat(&statStg, 0);
        destination->SetClass(statStg.clsid);

        std::vector<CfbfStream> streams;
        std::vector<IStorage*> storagesToClose;

        try {
            // 1. copy topology
            copyTopology(*source, *destination, streams, storagesToClose);

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

        // 4. commit changes with STGC_OVERWRITE | STGC_CONSOLIDATE
        destination->Commit(STGC_OVERWRITE | STGC_CONSOLIDATE);
        destination->Release();
        source->Release();

        // 5. verify identical
        IStorage* l;
        if (FAILED(StgOpenStorage(sourcePath.c_str(), nullptr, STGM_DIRECT | STGM_READ | STGM_SHARE_DENY_WRITE, nullptr, 0, &l))) {
            throw std::runtime_error("cfbf: cannot re-open source stream for verification");
        }
        IStorage* r;
        if (FAILED(StgOpenStorage(destPath.c_str(), nullptr, STGM_DIRECT | STGM_READ | STGM_SHARE_DENY_WRITE, nullptr, 0, &r))) {
            l->Release();
            throw std::runtime_error("cfbf: cannot re-open target stream for verification");
        }

        bool isIdentical = verifyIdentical(*l, *r);
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
        optimizeCFBF(input.wstring(), output.wstring(), false);
    } catch (const std::exception& e) {
        if (coInit) CoUninitialize();
        Logger::log(LogLevel::Error, std::string("cfbf optimization failed: ") + e.what(), get_name());
        throw;
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

} // namespace chisel