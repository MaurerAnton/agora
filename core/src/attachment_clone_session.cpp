// Port of data/MessageAttachmentCloneSession.kt.
//
// The session copies the attachment files of a cloned message, isolating the copies per owner key
// and rolling them back when the database graph does not commit. The metadata rewrite keeps the
// `file://` uris pointing at the owner's copy.
#include "agora/core/attachment_clone_session.hpp"

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/json.hpp"

namespace agora::core::attachment_clone {

namespace {

namespace json = agora::core::json;

constexpr std::string_view kFileScheme = "file://";

std::string file_extension(const std::string& path) {
    const std::size_t slash = path.find_last_of('/');
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return std::string();
    if (slash != std::string::npos && dot < slash) return std::string();
    return path.substr(dot + 1);
}

}  // namespace

std::string FileStore::extension_of(const std::string& path) { return file_extension(path); }

CloneSession::CloneSession(FileStore& files, std::string destination_dir,
                           CloneNameGenerator name_generator)
    : files_(files),
      destination_dir_(std::move(destination_dir)),
      name_generator_(std::move(name_generator)) {}

std::string CloneSession::clone_backing_path(const std::string& owner_key, const std::string& path) {
    for (const auto& entry : copied_by_owner_and_source_) {
        if (entry.owner == owner_key && entry.source == path) return entry.copied;
    }
    if (!files_.is_file(path)) return path;
    if (!files_.directory_exists(destination_dir_) && !files_.create_directories(destination_dir_)) {
        throw std::runtime_error("Cannot create attachment clone directory");
    }
    const std::string extension = file_extension(path);
    const std::string suffix = extension.empty() ? std::string() : "." + extension;
    const std::string name = name_generator_() + suffix;
    const std::string destination = destination_dir_ + "/" + name;
    if (!files_.copy_file(path, destination)) {
        throw std::runtime_error("Cannot copy attachment backing file");
    }
    created_files_.push_back(destination);
    // The cache keeps the absolute path, so a repeated reference resolves to the same copy.
    const std::string absolute = files_.absolute_path(destination);
    copied_by_owner_and_source_.push_back(CopiedFile{owner_key, path, absolute});
    return absolute;
}

CloneableMessage CloneSession::clone_message(const CloneableMessage& message,
                                            const std::optional<std::string>& owner_key) {
    const std::string owner = owner_key.value_or(message.id);
    CloneableMessage clone;
    clone.id = message.id;
    for (const std::string& image : message.images) {
        clone.images.push_back(clone_backing_path(owner, image));
    }
    if (message.attachment_meta.has_value()) {
        clone.attachment_meta = clone_attachment_meta(
            *message.attachment_meta,
            [this, &owner](const std::string& path) { return clone_backing_path(owner, path); });
    }
    return clone;
}

void CloneSession::commit() {
    committed_ = true;
    created_files_.clear();
    copied_by_owner_and_source_.clear();
}

void CloneSession::rollback() {
    if (committed_) return;
    for (auto it = created_files_.rbegin(); it != created_files_.rend(); ++it) {
        files_.delete_file(*it);
    }
    created_files_.clear();
    copied_by_owner_and_source_.clear();
}

namespace {

/** The declared fields of model/AttachmentMeta.kt::AttachmentItem, in declaration order. */
struct ItemField {
    const char* key;
};

std::optional<std::string> optional_string_field(const json::Value& object, const char* key) {
    for (const json::Member& member : object.members) {
        if (member.key != key) continue;
        if (member.value.kind == json::Kind::Literal && member.value.text == "null") {
            return std::nullopt;
        }
        if (member.value.kind != json::Kind::String) {
            throw std::runtime_error(std::string("Attachment metadata field ") + key +
                                     " is not a string");
        }
        return member.value.text;
    }
    return std::nullopt;
}

std::optional<int> optional_int_field(const json::Value& object, const char* key) {
    for (const json::Member& member : object.members) {
        if (member.key != key) continue;
        if (member.value.kind == json::Kind::Literal && member.value.text == "null") {
            return std::nullopt;
        }
        if (member.value.kind != json::Kind::Literal) {
            throw std::runtime_error(std::string("Attachment metadata field ") + key +
                                     " is not a number");
        }
        try {
            return std::stoi(member.value.text);
        } catch (const std::exception&) {
            throw std::runtime_error(std::string("Attachment metadata field ") + key +
                                     " is not a number");
        }
    }
    return std::nullopt;
}

std::optional<std::int64_t> optional_long_field(const json::Value& object, const char* key) {
    for (const json::Member& member : object.members) {
        if (member.key != key) continue;
        if (member.value.kind == json::Kind::Literal && member.value.text == "null") {
            return std::nullopt;
        }
        if (member.value.kind != json::Kind::Literal) {
            throw std::runtime_error(std::string("Attachment metadata field ") + key +
                                     " is not a number");
        }
        try {
            return std::stoll(member.value.text);
        } catch (const std::exception&) {
            throw std::runtime_error(std::string("Attachment metadata field ") + key +
                                     " is not a number");
        }
    }
    return std::nullopt;
}

/** The AttachmentStorage wire values (model/AttachmentMeta.kt). */
backup::AttachmentStorage storage_from_wire(const std::string& value) {
    if (value == "local_sandbox_pending") return backup::AttachmentStorage::local_sandbox_pending;
    if (value == "local_sandbox_runtime") return backup::AttachmentStorage::local_sandbox_runtime;
    return backup::AttachmentStorage::app_private;
}

const char* const kKnownItemFields[] = {
    "originalUri", "type", "file_name", "mime_type", "image_index", "page_count",
    "pixel_width", "pixel_height", "warning", "text_content", "transcription", "storage",
    "sandbox_path", "file_size", "unavailable",
};

}  // namespace

std::optional<std::string> clone_attachment_meta(
    const std::string& raw, const std::function<std::string(const std::string&)>& clone_backing_path) {
    const json::ParseResult parsed = json::parse(raw);
    if (!parsed.value.has_value() || parsed.value->kind != json::Kind::Object) {
        throw std::runtime_error("Attachment metadata is not a JSON object");
    }
    const json::Value& root = *parsed.value;
    for (const json::Member& member : root.members) {
        if (member.key != "items") {
            throw std::runtime_error("Attachment metadata has an unknown field " + member.key);
        }
    }
    std::vector<backup::AttachmentItem> items;
    for (const json::Member& member : root.members) {
        if (member.key != "items") continue;
        if (member.value.kind != json::Kind::Array) {
            throw std::runtime_error("Attachment metadata items are not an array");
        }
        for (const json::Value& element : member.value.elements) {
            if (element.kind != json::Kind::Object) {
                throw std::runtime_error("Attachment metadata item is not an object");
            }
            for (const json::Member& item_member : element.members) {
                bool known = false;
                for (const char* candidate : kKnownItemFields) {
                    if (item_member.key == candidate) {
                        known = true;
                        break;
                    }
                }
                if (!known) {
                    throw std::runtime_error("Attachment metadata item has an unknown field " +
                                             item_member.key);
                }
            }
            backup::AttachmentItem item;
            item.original_uri = optional_string_field(element, "originalUri");
            item.type = optional_string_field(element, "type").value_or("");
            item.file_name = optional_string_field(element, "file_name");
            item.mime_type = optional_string_field(element, "mime_type");
            item.image_index = optional_int_field(element, "image_index");
            item.page_count = optional_int_field(element, "page_count");
            item.pixel_width = optional_int_field(element, "pixel_width");
            item.pixel_height = optional_int_field(element, "pixel_height");
            item.warning = optional_string_field(element, "warning");
            item.text_content = optional_string_field(element, "text_content");
            item.transcription = optional_string_field(element, "transcription");
            if (const std::optional<std::string> storage = optional_string_field(element, "storage");
                storage.has_value()) {
                item.storage = storage_from_wire(*storage);
            }
            item.sandbox_path = optional_string_field(element, "sandbox_path");
            item.file_size = optional_long_field(element, "file_size");
            for (const json::Member& item_member : element.members) {
                if (item_member.key != "unavailable") continue;
                if (item_member.value.kind == json::Kind::Literal &&
                    item_member.value.text == "true") {
                    item.unavailable = true;
                } else if (item_member.value.kind == json::Kind::Literal &&
                           item_member.value.text == "false") {
                    item.unavailable = false;
                } else {
                    throw std::runtime_error("Attachment metadata field unavailable is not a boolean");
                }
            }
            if (item.original_uri.has_value() &&
                item.original_uri->rfind(kFileScheme, 0) == 0) {
                const std::string path = item.original_uri->substr(kFileScheme.size());
                item.original_uri = std::string(kFileScheme) + clone_backing_path(path);
            }
            items.push_back(std::move(item));
        }
    }
    return backup::encode_attachment_meta(backup::AttachmentMeta{std::move(items)});
}

}  // namespace agora::core::attachment_clone
