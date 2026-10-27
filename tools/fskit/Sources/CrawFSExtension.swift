// SPDX-License-Identifier: Apache-2.0
//
// FSKit module (macOS 26+) that mounts a .CRAW take as a CinemaDNG folder:
//   mount -F -t crawfs /path/take.craw /path/mountpoint
// The volume lists <take>_000000.dng ... and <take>.wav; DNGs are built on
// demand by CrawFSCore (lossless, cached, read-ahead).
import Foundation
import FSKit

@main
struct CrawFSExtension: UnaryFileSystemExtension {
    var fileSystem: FSUnaryFileSystem & FSUnaryFileSystemOperations { CrawFS() }
}

final class CrawFS: FSUnaryFileSystem, FSUnaryFileSystemOperations {
    func probeResource(resource: FSResource,
                       replyHandler: @escaping (FSProbeResult?, (any Error)?) -> Void) {
        guard let url = (resource as? FSPathURLResource)?.url, url.pathExtension.lowercased() == "craw" else {
            replyHandler(FSProbeResult.notRecognized, nil)
            return
        }
        replyHandler(FSProbeResult.usable(name: url.deletingPathExtension().lastPathComponent,
                                          containerID: FSContainerIdentifier(uuid: UUID())), nil)
    }

    func loadResource(resource: FSResource, options: FSTaskOptions,
                      replyHandler: @escaping (FSVolume?, (any Error)?) -> Void) {
        guard let url = (resource as? FSPathURLResource)?.url else {
            replyHandler(nil, fs_errorForPOSIXError(POSIXError.EINVAL.rawValue))
            return
        }
        do {
            let volume = try CrawVolume(url: url)
            containerStatus = .ready
            replyHandler(volume, nil)
        } catch {
            replyHandler(nil, error)
        }
    }

    func unloadResource(resource: FSResource, options: FSTaskOptions,
                        replyHandler reply: @escaping ((any Error)?) -> Void) {
        reply(nil)
    }
}
