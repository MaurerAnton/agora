// Port of model/MessageGenerationBoundary.kt and model/RunUiProjection.kt.
//
// The boundary resolver groups the visible rows into generations (a real USER row is a hard
// boundary, rows with a durable Run id stay together) and the projection derives the Regenerate /
// Edit affordances from the selected path: edited USER rows are siblings under the preceding
// message, regenerated MODEL roots are siblings under one shared USER input.
#include "agora/core/run_ui_projection.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace agora::core::run_ui {

namespace prep = agora::core::prep;

namespace {

constexpr std::string_view kToolPrefix = "tool_";
constexpr std::string_view kResultPrefix = "result_";

bool starts_with(const std::string& text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

/** MessageGenerationBoundaryResolver.isProtocolRow. */
bool is_protocol_row(const prep::ChatMessage& message) {
    return starts_with(message.id, kToolPrefix) || starts_with(message.id, kResultPrefix);
}

bool is_blank(const std::string& text) {
    return std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c) != 0; });
}

}  // namespace

bool is_real_user(const prep::ChatMessage& message) {
    return message.participant == prep::Participant::User && !is_protocol_row(message) &&
           !message.is_context_compact();
}

bool is_ordinary_assistant(const prep::ChatMessage& message) {
    return message.participant == prep::Participant::Model && !is_protocol_row(message) &&
           !message.is_context_compact();
}

std::vector<GenerationBoundary> resolve_boundaries(
    const std::vector<prep::ChatMessage>& visible_messages) {
    std::vector<GenerationBoundary> boundaries;
    std::vector<prep::ChatMessage> current;
    std::optional<std::string> current_run_id;

    const auto finish_boundary = [&]() {
        if (current.empty()) return;
        GenerationBoundary boundary;
        boundary.messages = current;
        // The pointers must address the boundary's own copy, not the scratch vector.
        for (std::size_t index = 0; index < boundary.messages.size(); ++index) {
            const prep::ChatMessage& message = boundary.messages[index];
            if (boundary.input == nullptr && is_real_user(message)) {
                boundary.input = &boundary.messages[index];
            }
            if (is_ordinary_assistant(message)) {
                if (boundary.first_assistant == nullptr) boundary.first_assistant = &boundary.messages[index];
                boundary.last_assistant = &boundary.messages[index];
            }
        }
        boundaries.push_back(std::move(boundary));
        current.clear();
        current_run_id.reset();
    };

    std::vector<const prep::ChatMessage*> unique;
    for (const prep::ChatMessage& message : visible_messages) {
        bool duplicate = false;
        for (const prep::ChatMessage* seen : unique) {
            if (seen->id == message.id) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) unique.push_back(&message);
    }

    for (const prep::ChatMessage* message : unique) {
        std::optional<std::string> message_run_id;
        if (message->run_id.has_value() && !is_blank(*message->run_id)) {
            message_run_id = *message->run_id;
        }
        if (is_real_user(*message)) {
            finish_boundary();
        } else if (!current.empty() && current_run_id.has_value() && message_run_id.has_value() &&
                   *current_run_id != *message_run_id) {
            finish_boundary();
        }
        current.push_back(*message);
        if (!current_run_id.has_value()) current_run_id = message_run_id;
    }
    finish_boundary();
    return boundaries;
}

std::vector<std::pair<std::string, MessagePresentation>> project(
    const std::vector<prep::ChatMessage>& visible_messages,
    const std::vector<prep::ChatMessage>& all_messages) {
    std::vector<std::pair<std::string, MessagePresentation>> result;
    if (visible_messages.empty()) return result;

    // The id is the structural identity: a transient race must not look like two branches.
    std::vector<const prep::ChatMessage*> unique_all;
    for (const prep::ChatMessage& message : all_messages) {
        bool duplicate = false;
        for (const prep::ChatMessage* seen : unique_all) {
            if (seen->id == message.id) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) unique_all.push_back(&message);
    }
    std::vector<const prep::ChatMessage*> unique_visible;
    for (const prep::ChatMessage& message : visible_messages) {
        bool duplicate = false;
        for (const prep::ChatMessage* seen : unique_visible) {
            if (seen->id == message.id) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) unique_visible.push_back(&message);
    }

    std::vector<const prep::ChatMessage*> boundary_inputs;
    for (const prep::ChatMessage* message : unique_all) {
        if (is_real_user(*message)) boundary_inputs.push_back(message);
    }

    // The edited USER rows are siblings under the preceding message.
    std::vector<std::pair<std::optional<std::string>, std::vector<const prep::ChatMessage*>>>
        edit_siblings;
    auto siblings_for = [&edit_siblings](const std::optional<std::string>& parent)
        -> std::vector<const prep::ChatMessage*>* {
        for (auto& entry : edit_siblings) {
            if (entry.first == parent) return &entry.second;
        }
        return nullptr;
    };
    for (const prep::ChatMessage* message : boundary_inputs) {
        std::vector<const prep::ChatMessage*>* bucket = siblings_for(message->parent_id);
        if (bucket == nullptr) {
            edit_siblings.emplace_back(message->parent_id, std::vector<const prep::ChatMessage*>{});
            bucket = &edit_siblings.back().second;
        }
        bucket->push_back(message);
    }
    const auto branch_order = [](const prep::ChatMessage* left, const prep::ChatMessage* right) {
        if (left->timestamp != right->timestamp) return left->timestamp < right->timestamp;
        return left->id < right->id;
    };
    for (auto& entry : edit_siblings) {
        std::sort(entry.second.begin(), entry.second.end(), branch_order);
    }

    // The regenerated MODEL roots are siblings under one shared USER input.
    std::vector<const prep::ChatMessage*> root_outputs;
    for (const prep::ChatMessage* message : unique_all) {
        if (!is_ordinary_assistant(*message)) continue;
        for (const prep::ChatMessage* input : boundary_inputs) {
            if (message->parent_id.has_value() && *message->parent_id == input->id) {
                root_outputs.push_back(message);
                break;
            }
        }
    }
    std::vector<std::pair<std::optional<std::string>, std::vector<const prep::ChatMessage*>>>
        regeneration_siblings;
    for (const prep::ChatMessage* message : root_outputs) {
        std::vector<const prep::ChatMessage*>* bucket = nullptr;
        for (auto& entry : regeneration_siblings) {
            if (entry.first == message->parent_id) {
                bucket = &entry.second;
                break;
            }
        }
        if (bucket == nullptr) {
            regeneration_siblings.emplace_back(message->parent_id,
                                             std::vector<const prep::ChatMessage*>{});
            bucket = &regeneration_siblings.back().second;
        }
        bucket->push_back(message);
    }
    for (auto& entry : regeneration_siblings) {
        std::sort(entry.second.begin(), entry.second.end(), branch_order);
    }

    // The presentation map starts with every visible message.
    std::vector<std::pair<std::string, MessagePresentation>> presentations;
    for (const prep::ChatMessage* message : unique_visible) {
        presentations.emplace_back(message->id, MessagePresentation{});
    }
    const auto presentation_for = [&presentations](const std::string& id) -> MessagePresentation* {
        for (auto& entry : presentations) {
            if (entry.first == id) return &entry.second;
        }
        return nullptr;
    };

    for (const prep::ChatMessage* user_boundary : unique_visible) {
        if (!is_real_user(*user_boundary)) continue;
        const std::vector<const prep::ChatMessage*>* siblings =
            siblings_for(user_boundary->parent_id);
        const std::size_t sibling_count = siblings != nullptr ? siblings->size() : 0;
        std::size_t index = 0;
        bool found = false;
        if (siblings != nullptr) {
            for (std::size_t position = 0; position < siblings->size(); ++position) {
                if ((*siblings)[position]->id == user_boundary->id) {
                    index = position;
                    found = true;
                    break;
                }
            }
        }
        MessagePresentation presentation;
        presentation.show_actions = true;
        if (!is_blank(user_boundary->text)) presentation.copy_text = user_boundary->text;
        presentation.delete_target_message_id = user_boundary->id;
        presentation.show_branch_selector = sibling_count > 1;
        presentation.branch_index = found ? static_cast<int>(index) : 0;
        presentation.total_branches =
            static_cast<int>(sibling_count > 0 ? sibling_count : 1);
        presentation.branch_anchor_parent_id = user_boundary->parent_id;
        presentation.branch_anchor_message_id = user_boundary->id;
        MessagePresentation* target = presentation_for(user_boundary->id);
        if (target != nullptr) *target = presentation;
    }

    for (const GenerationBoundary& boundary : resolve_boundaries(visible_messages)) {
        if (boundary.last_assistant == nullptr) continue;
        const prep::ChatMessage* output_boundary = boundary.last_assistant;
        const prep::ChatMessage* root_output = boundary.first_assistant != nullptr
                                                   ? boundary.first_assistant
                                                   : output_boundary;
        const prep::ChatMessage* structural_input = nullptr;
        if (boundary.input != nullptr && root_output->parent_id.has_value() &&
            *root_output->parent_id == boundary.input->id) {
            structural_input = boundary.input;
        } else {
            for (const prep::ChatMessage* candidate : unique_all) {
                if (candidate->id == root_output->parent_id.value_or("") &&
                    is_real_user(*candidate)) {
                    structural_input = candidate;
                    break;
                }
            }
        }
        const std::vector<const prep::ChatMessage*>* siblings = nullptr;
        if (structural_input != nullptr) {
            for (const auto& entry : regeneration_siblings) {
                if (entry.first.has_value() && *entry.first == structural_input->id) {
                    siblings = &entry.second;
                    break;
                }
            }
        }
        const std::size_t sibling_count = siblings != nullptr ? siblings->size() : 0;
        std::size_t index = 0;
        bool found = false;
        if (siblings != nullptr) {
            for (std::size_t position = 0; position < siblings->size(); ++position) {
                if ((*siblings)[position]->id == root_output->id) {
                    index = position;
                    found = true;
                    break;
                }
            }
        }
        MessagePresentation presentation;
        presentation.show_actions = true;
        if (!is_blank(output_boundary->text)) presentation.copy_text = output_boundary->text;
        presentation.delete_target_message_id = root_output->id;
        presentation.show_branch_selector = sibling_count > 1;
        presentation.branch_index = found ? static_cast<int>(index) : 0;
        presentation.total_branches = static_cast<int>(sibling_count > 0 ? sibling_count : 1);
        presentation.branch_anchor_parent_id = root_output->parent_id;
        presentation.branch_anchor_message_id = root_output->id;
        MessagePresentation* target = presentation_for(output_boundary->id);
        if (target != nullptr) *target = presentation;
    }

    return presentations;
}

}  // namespace agora::core::run_ui
