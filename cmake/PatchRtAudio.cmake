# Applied once to the fetched RtAudio (the toolkit's PATCH_COMMAND for it, run with -DSRC=<its source
# directory>): the ASIO back end hands the driver a null bufferSwitchTimeInfo callback, having told it the host does not
# take time info - and some drivers call that callback whatever the host answers, through the null pointer, taking the
# process down the moment the stream starts. The fix is upstream's, made after the release the suite pins: a
# bufferSwitchTimeInfo that hands the work to bufferSwitch, since the time info is unused, given to the driver in the
# null's place. Nothing else of the dependency is touched; only a Windows build with the ASIO SDK compiles the code.
# Idempotent: a tree already patched is left exactly as it is, so its timestamp does not move.
if(NOT DEFINED SRC)
    message(FATAL_ERROR "PatchRtAudio.cmake: pass -DSRC=<the RtAudio source directory>")
endif()
set(source "${SRC}/RtAudio.cpp")
if(NOT EXISTS "${source}")
    message(FATAL_ERROR "PatchRtAudio.cmake: ${source} is not there - RtAudio has moved its source")
endif()
file(READ "${source}" text)
string(FIND "${text}" "static ASIOTime *bufferSwitchTimeInfo(" already)
if(NOT already EQUAL -1)
    return()
endif()
set(callback_before "  object->callbackEvent( index );\n}\n\nbool RtApiAsio :: probeDeviceOpen")
string(CONCAT callback_after
       "  object->callbackEvent( index );\n}\n\n"
       "// A driver may call bufferSwitchTimeInfo() whatever the host answers to kAsioSupportsTimeInfo, so it is never\n"
       "// left null; the time info is unused, and the work is bufferSwitch()'s.\n"
       "static ASIOTime *bufferSwitchTimeInfo( ASIOTime * /*timeInfo*/, long index, ASIOBool processNow )\n"
       "{\n  bufferSwitch( index, processNow );\n  return 0L;\n}\n\n"
       "bool RtApiAsio :: probeDeviceOpen")
set(assign_before "asioCallbacks.bufferSwitchTimeInfo = NULL;")
set(assign_after "asioCallbacks.bufferSwitchTimeInfo = &bufferSwitchTimeInfo;")
string(FIND "${text}" "${callback_before}" callback_at)
string(FIND "${text}" "${assign_before}" assign_at)
if(callback_at EQUAL -1 OR assign_at EQUAL -1)
    message(FATAL_ERROR "PatchRtAudio.cmake: the ASIO back end no longer sets up its callbacks as this patch expects - "
                        "read ${source} and update the patch, or drop it if bufferSwitchTimeInfo is no longer null")
endif()
string(REPLACE "${callback_before}" "${callback_after}" text "${text}")
string(REPLACE "${assign_before}" "${assign_after}" text "${text}")
file(WRITE "${source}" "${text}")
message(STATUS "RtAudio: the ASIO back end's bufferSwitchTimeInfo given to the driver, not a null (${source})")
