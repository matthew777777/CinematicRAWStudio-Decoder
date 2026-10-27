// SPDX-License-Identifier: Apache-2.0
import CrawFSCore
import Foundation
import FSKit

/// One entry of the flat volume: the root directory, a frame DNG or the WAV.
final class CrawItem: FSItem {
    enum Kind { case root, frame(UInt64), wav }

    let kind: Kind
    let name: FSFileName
    let attributes = FSItem.Attributes()

    init(kind: Kind, name: String, id: FSItem.Identifier, size: UInt64, mtime: timespec) {
        self.kind = kind
        self.name = FSFileName(string: name)
        super.init()
        let a = attributes
        a.fileID = id
        a.parentID = { if case .root = kind { return .parentOfRoot } else { return .rootDirectory } }()
        a.uid = getuid()
        a.gid = getgid()
        a.flags = 0
        a.size = size
        a.allocSize = size
        if case .root = kind {
            a.type = .directory
            a.mode = UInt32(S_IFDIR | 0o555)
            a.linkCount = 2
        } else {
            a.type = .file
            a.mode = UInt32(S_IFREG | 0o444)
            a.linkCount = 1
        }
        a.addedTime = mtime
        a.birthTime = mtime
        a.changeTime = mtime
        a.modifyTime = mtime
        a.accessTime = mtime
    }
}

final class CrawVolume: FSVolume {
    private let url: URL
    private let clip: OpaquePointer
    private let root: CrawItem
    private let files: [CrawItem]          // frames in order, then the WAV
    private let byName: [String: CrawItem]

    init(url: URL) throws {
        self.url = url
        _ = url.startAccessingSecurityScopedResource()
        var err = [CChar](repeating: 0, count: 512)
        guard let handle = url.withUnsafeFileSystemRepresentation({ path in
            crawfs_open(path, &err, err.count)
        }) else {
            url.stopAccessingSecurityScopedResource()
            throw fs_errorForPOSIXError(POSIXError.EFTYPE.rawValue)
        }
        clip = handle
        let stem = url.deletingPathExtension().lastPathComponent
        let mtime = timespec(tv_sec: Int(crawfs_mtime(handle)), tv_nsec: 0)
        root = CrawItem(kind: .root, name: "/", id: .rootDirectory, size: 0, mtime: mtime)
        var items: [CrawItem] = []
        let dngSize = crawfs_dng_size(handle)
        for i in 0..<crawfs_frame_count(handle) {
            let name = String(format: "%@_%06llu.dng", stem, i)
            items.append(CrawItem(kind: .frame(i), name: name,
                                  id: FSItem.Identifier(rawValue: 1000 + i) ?? .invalid, size: dngSize, mtime: mtime))
        }
        let wavSize = crawfs_wav_size(handle)
        if wavSize > 0 {
            items.append(CrawItem(kind: .wav, name: "\(stem).wav",
                                  id: FSItem.Identifier(rawValue: 999) ?? .invalid, size: wavSize, mtime: mtime))
        }
        files = items
        byName = Dictionary(uniqueKeysWithValues: items.map { ($0.name.string ?? "", $0) })
        super.init(volumeID: FSVolume.Identifier(uuid: UUID()), volumeName: FSFileName(string: stem))
    }

    deinit {
        crawfs_close(clip)
        url.stopAccessingSecurityScopedResource()
    }
}

extension CrawVolume: FSVolume.PathConfOperations {
    var maximumLinkCount: Int { 1 }
    var maximumNameLength: Int { 255 }
    var restrictsOwnershipChanges: Bool { true }
    var truncatesLongNames: Bool { false }
    var maximumXattrSize: Int { 0 }
    var maximumFileSize: UInt64 { UInt64(Int64.max) }
}

extension CrawVolume: FSVolume.Operations {
    var supportedVolumeCapabilities: FSVolume.SupportedCapabilities {
        let c = FSVolume.SupportedCapabilities()
        c.supportsHardLinks = false
        c.supportsSymbolicLinks = false
        c.supportsPersistentObjectIDs = true
        c.doesNotSupportVolumeSizes = true
        c.supportsHiddenFiles = false
        c.supports64BitObjectIDs = true
        c.caseFormat = .insensitiveCasePreserving
        return c
    }

    var volumeStatistics: FSStatFSResult {
        let r = FSStatFSResult(fileSystemTypeName: "crawfs")
        r.blockSize = 4096
        r.ioSize = 1 << 20
        let bytes = files.reduce(UInt64(0)) { $0 + $1.attributes.size }
        r.totalBlocks = bytes / 4096 + 1
        r.availableBlocks = 0
        r.freeBlocks = 0
        r.totalFiles = UInt64(files.count + 1)
        r.freeFiles = 0
        return r
    }

    func activate(options: FSTaskOptions) async throws -> FSItem { root }
    func deactivate(options: FSDeactivateOptions = []) async throws {}
    func mount(options: FSTaskOptions) async throws {}
    func unmount() async {}
    func synchronize(flags: FSSyncFlags) async throws {}

    func attributes(_ desired: FSItem.GetAttributesRequest, of item: FSItem) async throws -> FSItem.Attributes {
        guard let item = item as? CrawItem else { throw fs_errorForPOSIXError(POSIXError.EIO.rawValue) }
        return item.attributes
    }

    func setAttributes(_ newAttributes: FSItem.SetAttributesRequest, on item: FSItem) async throws -> FSItem.Attributes {
        throw fs_errorForPOSIXError(POSIXError.EROFS.rawValue)
    }

    func lookupItem(named name: FSFileName, inDirectory directory: FSItem) async throws -> (FSItem, FSFileName) {
        guard directory === root, let key = name.string,
              let item = byName[key] ?? byName.first(where: { $0.key.caseInsensitiveCompare(key) == .orderedSame })?.value
        else { throw fs_errorForPOSIXError(POSIXError.ENOENT.rawValue) }
        return (item, item.name)
    }

    func reclaimItem(_ item: FSItem) async throws {}

    func readSymbolicLink(_ item: FSItem) async throws -> FSFileName {
        throw fs_errorForPOSIXError(POSIXError.EINVAL.rawValue)
    }

    func createItem(named name: FSFileName, type: FSItem.ItemType, inDirectory directory: FSItem,
                    attributes newAttributes: FSItem.SetAttributesRequest) async throws -> (FSItem, FSFileName) {
        throw fs_errorForPOSIXError(POSIXError.EROFS.rawValue)
    }

    func createSymbolicLink(named name: FSFileName, inDirectory directory: FSItem,
                            attributes newAttributes: FSItem.SetAttributesRequest,
                            linkContents contents: FSFileName) async throws -> (FSItem, FSFileName) {
        throw fs_errorForPOSIXError(POSIXError.EROFS.rawValue)
    }

    func createLink(to item: FSItem, named name: FSFileName, inDirectory directory: FSItem) async throws -> FSFileName {
        throw fs_errorForPOSIXError(POSIXError.EROFS.rawValue)
    }

    func removeItem(_ item: FSItem, named name: FSFileName, fromDirectory directory: FSItem) async throws {
        throw fs_errorForPOSIXError(POSIXError.EROFS.rawValue)
    }

    func renameItem(_ item: FSItem, inDirectory sourceDirectory: FSItem, named sourceName: FSFileName,
                    to destinationName: FSFileName, inDirectory destinationDirectory: FSItem,
                    overItem: FSItem?) async throws -> FSFileName {
        throw fs_errorForPOSIXError(POSIXError.EROFS.rawValue)
    }

    /// Cookies: 0 = start, 1 = after ".", 2 = after "..", 3 + i = after files[i].
    func enumerateDirectory(_ directory: FSItem, startingAt cookie: FSDirectoryCookie, verifier: FSDirectoryVerifier,
                            attributes: FSItem.GetAttributesRequest?,
                            packer: FSDirectoryEntryPacker) async throws -> FSDirectoryVerifier {
        guard directory === root else { throw fs_errorForPOSIXError(POSIXError.ENOTDIR.rawValue) }
        var next = cookie.rawValue
        if next == 0 {
            guard packer.packEntry(name: FSFileName(string: "."), itemType: .directory, itemID: .rootDirectory,
                                   nextCookie: FSDirectoryCookie(1), attributes: attributes != nil ? root.attributes : nil)
            else { return FSDirectoryVerifier(1) }
            next = 1
        }
        if next == 1 {
            guard packer.packEntry(name: FSFileName(string: ".."), itemType: .directory, itemID: .parentOfRoot,
                                   nextCookie: FSDirectoryCookie(2), attributes: attributes != nil ? root.attributes : nil)
            else { return FSDirectoryVerifier(1) }
            next = 2
        }
        var i = Int(next) - 2
        while i < files.count {
            let f = files[i]
            guard packer.packEntry(name: f.name, itemType: .file, itemID: f.attributes.fileID,
                                   nextCookie: FSDirectoryCookie(UInt64(i + 3)),
                                   attributes: attributes != nil ? f.attributes : nil)
            else { break }
            i += 1
        }
        return FSDirectoryVerifier(1)
    }
}

extension CrawVolume: FSVolume.ReadWriteOperations {
    func read(from item: FSItem, at offset: off_t, length: Int, into buffer: FSMutableFileDataBuffer) async throws -> Int {
        guard let item = item as? CrawItem, offset >= 0 else { throw fs_errorForPOSIXError(POSIXError.EINVAL.rawValue) }
        let n: Int64 = buffer.withUnsafeMutableBytes { dst in
            let len = UInt64(min(length, dst.count))
            switch item.kind {
            case .frame(let f): return crawfs_read_dng(clip, f, UInt64(offset), dst.baseAddress, len)
            case .wav: return crawfs_read_wav(clip, UInt64(offset), dst.baseAddress, len)
            case .root: return -Int64(EISDIR)
            }
        }
        if n < 0 { throw fs_errorForPOSIXError(Int32(-n)) }
        return Int(n)
    }

    func write(contents: Data, to item: FSItem, at offset: off_t) async throws -> Int {
        throw fs_errorForPOSIXError(POSIXError.EROFS.rawValue)
    }
}
