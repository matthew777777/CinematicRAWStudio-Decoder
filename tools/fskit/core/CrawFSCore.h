// SPDX-License-Identifier: Apache-2.0
//
// C API over a .CRAW take presented as a CinemaDNG folder, for the FSKit
// extension (Swift imports this header as module CrawFSCore). Thread-safe;
// negative returns are -errno.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CrawFsClip CrawFsClip;

// Opens a take; on failure returns NULL and writes a message into `err`.
CrawFsClip* crawfs_open(const char* path, char* err, size_t errLen);
void crawfs_close(CrawFsClip* clip);

uint64_t crawfs_frame_count(const CrawFsClip* clip);
// Every frame's DNG has this size (pixels start at a fixed padded offset).
uint64_t crawfs_dng_size(const CrawFsClip* clip);
// Modification time of the .craw file (seconds since the epoch).
int64_t crawfs_mtime(const CrawFsClip* clip);

// Reads DNG bytes of `frame`; returns bytes read (0 at EOF) or -errno.
// Reading offset 0 also schedules read-ahead of the following frames.
int64_t crawfs_read_dng(CrawFsClip* clip, uint64_t frame, uint64_t offset, void* dst, uint64_t len);

// WAV of the take's audio; size 0 when the take has no audio.
uint64_t crawfs_wav_size(const CrawFsClip* clip);
int64_t crawfs_read_wav(CrawFsClip* clip, uint64_t offset, void* dst, uint64_t len);

#ifdef __cplusplus
}
#endif
