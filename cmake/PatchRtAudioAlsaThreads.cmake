# Applied to the fetched RtAudio after cmake/PatchRtAudioAlsaXruns.cmake (the toolkit's PATCH_COMMAND for it, run with
# -DSRC=<its source directory> and -DVERSION, below): its ALSA back end's stream state and run flag shared safely between
# the thread that opens, starts, stops and closes a stream and the stream's callback thread.
#
# The release reads stream_.state on the callback thread without the stream's mutex - at the top of each pass of
# callbackEvent, to decide whether to wait for a start - while RtApi::openStream, after probeDeviceOpen has started the
# thread, writes it, and stopStream and abortStream write it just before they take the mutex; and closeStream clears the
# flag the thread loops on (callbackInfo.isRunning) as a plain bool the thread reads unlocked. ThreadSanitizer reports
# the first as a data race. The changes, each marked in the code:
#
#   - callbackEvent reads the state under the mutex every pass, waiting on the condition variable as before when the
#     stream is stopped; it unlocks before the program's callback runs, as the release does.
#   - openStream, stopStream and abortStream write the state with the mutex held. startStream already does; closeStream
#     writes it after joining the thread; probeDeviceOpen writes it before starting the thread.
#   - closeStream clears the run flag with an atomic store, and the thread's loop reads it with an atomic load.
#
# Only openStream is common to every back end: its write takes a mutex every back end initialises in RtApi's
# constructor, before any stream is opened. The rest is the ALSA back end's alone. The replaced text is RtAudio 6.0.1's
# as cmake/PatchRtAudioAlsaXruns.cmake leaves it, the anchors exact - a release that moves them fails here, to be read
# and the patch updated or dropped.
#
# Versioned by this file's own SHA-256, as cmake/PatchRtAudioAlsaXruns.cmake is: the version changing changes the
# PATCH_COMMAND, so ExternalProject runs the patch step again over a kept tree, and the script puts back the source it
# first patched (kept beside it) and applies itself afresh. When the xruns patch applies itself afresh first, this
# patch's work is gone with the source it restores, and this one keeps the new source and applies itself to it.
if(NOT DEFINED SRC OR NOT DEFINED VERSION)
    message(FATAL_ERROR "PatchRtAudioAlsaThreads.cmake: pass -DSRC=<the RtAudio source directory> and "
                        "-DVERSION=<this script's SHA-256>")
endif()
set(source "${SRC}/RtAudio.cpp")
set(unpatched "${SRC}/RtAudio.cpp.before-alsa-threads")
set(marker_prefix "// Patched by the build: cmake/PatchRtAudioAlsaThreads.cmake")
set(marker "${marker_prefix} ${VERSION}")
if(NOT EXISTS "${source}")
    message(FATAL_ERROR "PatchRtAudioAlsaThreads.cmake: ${source} is not there - RtAudio has moved its source")
endif()
file(READ "${source}" text)
string(FIND "${text}" "${marker}" current)
if(NOT current EQUAL -1)
    return()
endif()
string(FIND "${text}" "${marker_prefix}" older)
if(older EQUAL -1)
    file(WRITE "${unpatched}" "${text}")
elseif(EXISTS "${unpatched}")
    file(READ "${unpatched}" text)
else()
    message(FATAL_ERROR "PatchRtAudioAlsaThreads.cmake: another version of this patch is applied to ${source} and the "
                        "source it was applied to is not beside it: delete ${SRC} and the subbuild beside it, and "
                        "configure again")
endif()

# RtApi::openStream: the state written under the mutex.
set(open_before [==[
  if ( options ) options->numberOfBuffers = stream_.nBuffers;
  stream_.state = STREAM_STOPPED;
  return RTAUDIO_NO_ERROR;
}
]==])
set(open_after [==[
  if ( options ) options->numberOfBuffers = stream_.nBuffers;
  // Patched by the build: the state written under the stream's mutex - ALSA's callback thread, started by
  // probeDeviceOpen, reads it there.
  MUTEX_LOCK( &stream_.mutex );
  stream_.state = STREAM_STOPPED;
  MUTEX_UNLOCK( &stream_.mutex );
  return RTAUDIO_NO_ERROR;
}
]==])

# stopStream: the state written once the mutex is held.
set(stop_before [==[
errorText_ = "RtApiAlsa::stopStream(): the stream is closed!";
    return error( RTAUDIO_WARNING );
  }

  stream_.state = STREAM_STOPPED;
  MUTEX_LOCK( &stream_.mutex );
]==])
set(stop_after [==[
errorText_ = "RtApiAlsa::stopStream(): the stream is closed!";
    return error( RTAUDIO_WARNING );
  }

  // Patched by the build: the state written under the stream's mutex, where the callback thread reads it.
  MUTEX_LOCK( &stream_.mutex );
  stream_.state = STREAM_STOPPED;
]==])

# abortStream: likewise.
set(abort_before [==[
errorText_ = "RtApiAlsa::abortStream(): the stream is stopping or closed!";
    return error( RTAUDIO_WARNING );
  }

  stream_.state = STREAM_STOPPED;
  MUTEX_LOCK( &stream_.mutex );
]==])
set(abort_after [==[
errorText_ = "RtApiAlsa::abortStream(): the stream is stopping or closed!";
    return error( RTAUDIO_WARNING );
  }

  // Patched by the build: the state written under the stream's mutex, where the callback thread reads it.
  MUTEX_LOCK( &stream_.mutex );
  stream_.state = STREAM_STOPPED;
]==])

# callbackEvent: the state read under the mutex; the marker before it.
set(callback_before [==[
void RtApiAlsa :: callbackEvent()
{
  AlsaHandle *apiInfo = (AlsaHandle *) stream_.apiHandle;
  if ( stream_.state == STREAM_STOPPED ) {
    MUTEX_LOCK( &stream_.mutex );
    while ( !apiInfo->runnable )
      pthread_cond_wait( &apiInfo->runnable_cv, &stream_.mutex );

    if ( stream_.state != STREAM_RUNNING ) {
      MUTEX_UNLOCK( &stream_.mutex );
      return;
    }
    MUTEX_UNLOCK( &stream_.mutex );
  }

  if ( stream_.state == STREAM_CLOSED ) {
]==])
set(callback_after [==[
@MARKER@
void RtApiAlsa :: callbackEvent()
{
  AlsaHandle *apiInfo = (AlsaHandle *) stream_.apiHandle;
  // Patched by the build: the stream's state read under its mutex, which every write of it holds while this thread
  // runs; the release read it unlocked, racing openStream's, stopStream's and abortStream's writes.
  MUTEX_LOCK( &stream_.mutex );
  if ( stream_.state == STREAM_STOPPED ) {
    while ( !apiInfo->runnable )
      pthread_cond_wait( &apiInfo->runnable_cv, &stream_.mutex );

    if ( stream_.state != STREAM_RUNNING ) {
      MUTEX_UNLOCK( &stream_.mutex );
      return;
    }
  }
  const bool isClosed = stream_.state == STREAM_CLOSED;
  MUTEX_UNLOCK( &stream_.mutex );

  if ( isClosed ) {
]==])

# closeStream: the run flag cleared atomically.
set(close_before [==[
  AlsaHandle *apiInfo = (AlsaHandle *) stream_.apiHandle;
  stream_.callbackInfo.isRunning = false;
]==])
set(close_after [==[
  AlsaHandle *apiInfo = (AlsaHandle *) stream_.apiHandle;
  // Patched by the build: the flag the callback thread loops on, cleared atomically.
  __atomic_store_n( &stream_.callbackInfo.isRunning, false, __ATOMIC_RELEASE );
]==])

# alsaCallbackHandler: the run flag read atomically.
set(loop_before [==[
    std::cerr << "RtAudio alsa: " << 
             (sched_getscheduler(0) == SCHED_RR ? "" : "_NOT_ ") << 
             "running realtime scheduling" << std::endl;
  }
#endif

  while ( *isRunning == true ) {
]==])
set(loop_after [==[
    std::cerr << "RtAudio alsa: " << 
             (sched_getscheduler(0) == SCHED_RR ? "" : "_NOT_ ") << 
             "running realtime scheduling" << std::endl;
  }
#endif

  // Patched by the build: the flag closeStream clears, read atomically.
  while ( __atomic_load_n( isRunning, __ATOMIC_ACQUIRE ) ) {
]==])

string(REPLACE "@MARKER@" "${marker}" callback_after "${callback_after}")

foreach(part IN ITEMS open stop abort callback close loop)
    string(FIND "${text}" "${${part}_before}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "PatchRtAudioAlsaThreads.cmake: the ALSA back end no longer reads as this patch expects at "
                            "its ${part} change - read ${source} and update the patch, or drop it if RtAudio now "
                            "guards its stream's state itself")
    endif()
    string(LENGTH "${${part}_before}" length)
    math(EXPR after_end "${at} + ${length}")
    string(SUBSTRING "${text}" ${after_end} -1 rest)
    string(FIND "${rest}" "${${part}_before}" again)
    if(NOT again EQUAL -1)
        message(FATAL_ERROR "PatchRtAudioAlsaThreads.cmake: the ${part} change's text appears twice in ${source}")
    endif()
endforeach()
foreach(part IN ITEMS open stop abort callback close loop)
    string(REPLACE "${${part}_before}" "${${part}_after}" text "${text}")
endforeach()
file(WRITE "${source}" "${text}")
message(STATUS "RtAudio: the ALSA back end's stream state read and written under its mutex, its run flag atomic "
               "(${source})")
