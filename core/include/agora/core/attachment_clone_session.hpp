#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/backup_media_policy.hpp"

namespace agora::core::attachment_clone {

/**
 * Port of data/MessageAttachmentCloneSession.kt: the copies created while a persisted message is
 * cloned.
 *
 * Copies are isolated by owner key: repeated references inside one message resolve to one file,
 * while two cloned messages never share a backing file that the deletion of one could remove from
 * the other. `commit` may only be called after the database graph commits; `rollback` removes every
 * file this session created.
 *
 * The file system is a boundary: the port talks to a FileStore, so the caller decides where the
 * clones live. The random file name is entropy — the port takes the name generator as a parameter
 * instead of reading the system random source behind the caller's back.
 */

/** The file operations the session needs. */
class FileStore {
public:
    virtual ~FileStore() = default;

    /** True when the path exists and is a regular file. */
    virtual bool is_file(const std::string& path) const = 0;

    /** True when the directory exists. */
    virtual bool directory_exists(const std::string& path) const = 0;

    /** Creates the directory (and its parents); false when it could not be created. */
    virtual bool create_directories(const std::string& path) = 0;

    /** Copies `source` to `destination`; false on failure. */
    virtual bool copy_file(const std::string& source, const std::string& destination) = 0;

    /** Deletes a file; false when it could not be deleted. */
    virtual bool delete_file(const std::string& path) = 0;

    /** The absolute path of a (possibly relative) path. */
    virtual std::string absolute_path(const std::string& path) const = 0;

    /** The extension of a path, without the dot. */
    static std::string extension_of(const std::string& path);
};

/** The subset of MessageEntity the clone policy reads. */
struct CloneableMessage {
    std::string id;
    std::vector<std::string> images;
    std::optional<std::string> attachment_meta;
};

/** Generates the random file name of a clone (the Kotlin source uses UUID.randomUUID()). */
using CloneNameGenerator = std::function<std::string()>;

/** MessageAttachmentCloneSession. */
class CloneSession {
public:
    CloneSession(FileStore& files, std::string destination_dir, CloneNameGenerator name_generator);

    /** cloneMessage(message, ownerKey): the cloned image paths and the rewritten metadata. */
    CloneableMessage clone_message(const CloneableMessage& message,
                                   const std::optional<std::string>& owner_key = std::nullopt);

    /**
     * cloneBackingPath(ownerKey, path): the copy of `path` for that owner. A path that is not a
     * file is returned unchanged; a repeated reference of the same owner resolves to the same copy.
     */
    std::string clone_backing_path(const std::string& owner_key, const std::string& path);

    /** commit: the created files are kept. */
    void commit();

    /** rollback: the created files are deleted (unless the session was committed). */
    void rollback();

    /** The files this session created. */
    const std::vector<std::string>& created_files() const { return created_files_; }

private:
    FileStore& files_;
    std::string destination_dir_;
    CloneNameGenerator name_generator_;
    /** The copies this session made: the owner, the source path and the copy. */
    struct CopiedFile {
        std::string owner;
        std::string source;
        std::string copied;
    };
    std::vector<CopiedFile> copied_by_owner_and_source_;
    std::vector<std::string> created_files_;
    bool committed_ = false;
};

/**
 * cloneAttachmentMeta(raw, cloneBackingPath): the metadata with every `file://` uri pointing at
 * the owner's copy.
 */
std::optional<std::string> clone_attachment_meta(
    const std::string& raw, const std::function<std::string(const std::string&)>& clone_backing_path);

}  // namespace agora::core::attachment_clone
