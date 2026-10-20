package com.newoether.agora.model

enum class Participant {
    USER, MODEL, ERROR
}

enum class MessageStatus {
    TRANSCRIBING, SENDING, THINKING, TOOL_CALLING, SUCCESS, STOPPED, ERROR
}

enum class RunStatus {
    ACTIVE,
    STOPPING,
    COMPLETED,
    STOPPED,
    FAILED;

    val isTerminal: Boolean
        get() = this == COMPLETED || this == STOPPED || this == FAILED
}

enum class RunEndReason {
    MODEL_COMPLETED,
    USER_STOPPED,
    PROCESS_RECOVERED,
    PROVIDER_ERROR,
}
