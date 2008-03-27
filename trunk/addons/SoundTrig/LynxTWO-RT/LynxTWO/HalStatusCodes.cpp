#include "HalStatusCodes.h"

const char *HalStatusString(unsigned hal_status_code)
{
    switch (hal_status_code) {
        case HSTATUS_OK:
            return "Ok";
        case HSTATUS_CANNOT_FIND_ADAPTER:
            return "Cannot find adapter";
        case HSTATUS_CANNOT_MAP_ADAPTER:
            return "Cannot map adapter";
        case HSTATUS_CANNOT_UNMAP_ADAPTER:
            return "Cannot unmap adampter";
        case HSTATUS_ADAPTER_NOT_OPEN:
            return "Adapter not open";
        case HSTATUS_ADAPTER_NOT_FOUND:
            return "Adapter not found";
        case HSTATUS_BAD_ADAPTER_RAM:
            return "Bad adapter RAM";
        case HSTATUS_INCORRECT_FIRMWARE:
            return "Incorrect firmware";
        case HSTATUS_DOWNLOAD_FAILED:
            return "Download failed";
        case HSTATUS_HW_NOT_RESPONDING:
            return "Hardware not responding";
        case HSTATUS_INVALID_PARAMETER:
            return "Invalid parameter";
        case HSTATUS_INVALID_MODE:
            return "Invalid mode";
        case HSTATUS_INVALID_FORMAT:
            return "Invalid format";
        case HSTATUS_INVALID_ADDRESS:
            return "Invalid address";
        case HSTATUS_INVALID_CLOCK_SOURCE:
            return "Invalid clock source";
        case HSTATUS_INVALID_SAMPLERATE:
            return "Invalid sample rate";
        case HSTATUS_INVALID_MIXER_LINE:
            return "Invalid mixer line";
        case HSTATUS_INVALID_MIXER_CONTROL:
            return "Invalid mixer control";
        case HSTATUS_INVALID_MIXER_VALUE:
            return "Invalid mixer value";
        case HSTATUS_INSUFFICIENT_RESOURCES:
            return "Insufficient resources";
        case HSTATUS_BUFFER_FULL:
            return "Buffer full";
        case HSTATUS_ALREADY_IN_USE:
            return "Already in use";
        case HSTATUS_TIMEOUT:
            return "Timeout";
        case HSTATUS_MIXER_LOCKED:
            return "Mixer locked";
        case HSTATUS_SERVICE_NOT_REQUIRED:
            return "Service not required";
        case HSTATUS_MIDI1_SERVICE_REQUIRED:
            return "MIDI 1 service required";
        case HSTATUS_MIDI2_SERVICE_REQUIRED:	// ADAT MIDI SYNCIN
            return "MIDI 2 service required";
        default:
            return "Unknown status code";
    }
    return 0; // not reached
}
