# Applied to the fetched RtAudio, after cmake/PatchRtAudio.cmake (the toolkit's PATCH_COMMAND for it, run
# with -DSRC=<its source directory> and -DVERSION, below): its ALSA back end made to stop a device on an xrun, tell the
# callback, and play on - what ALSA does for any program that leaves its stop threshold where ALSA puts it.
#
# The release opens every ALSA PCM with its stop threshold at ULONG_MAX, past the boundary: a playback buffer that runs
# dry plays the silence RtAudio has ALSA fill it with, and a capture buffer that fills is written over, and the device
# runs on as if nothing had happened. So RtAudio's own handling of an xrun - the -EPIPE branches of its read and write,
# which flag RTAUDIO_INPUT_OVERFLOW or RTAUDIO_OUTPUT_UNDERFLOW to the next callback and prepare the device again - never
# runs, and a dropout reaches no callback: a callback stalled for 30 ms, past the whole 21 ms of a 4 x 256-frame buffer
# at 48 kHz, is reported as nothing. The changes, each marked in the code:
#
#   - The stop threshold is the buffer's size, ALSA's default and the one aplay keeps: a playback buffer that runs dry,
#     or a capture buffer that fills, stops the device in the XRUN state; the next write or read fails with -EPIPE; and
#     RtAudio's existing branches flag the xrun and prepare the device. A playback starts again at its next write - the
#     start threshold is one period - and refills, its writes not waiting until it is full again; a capture starts again
#     at its next read. The silence filling stays: it keeps what a playback has played silent behind it, and an xrun now
#     stops the device, which plays nothing more.
#   - A duplex stream's playback is primed with a buffer of silence before it starts, and again after an xrun has
#     stopped and prepared it. A stream that reads and writes is paced by its capture - each pass reads a period, which
#     waits for one, and writes a period, which finds room - so its playback holds what it held when it started: started
#     on its first write's one period, it would run dry at the next period's end and stop. Primed, it starts a buffer
#     ahead of the device, the latency the release's free-running playback had (its late writes landed a buffer on).
#   - With the priming, a capture linked to its playback (snd_pcm_link: one card's two directions started and stopped as
#     one) finds the pair running at its first read. Its read would otherwise start the pair with nothing to play, which
#     a stop threshold under the boundary refuses with -EPIPE.
#   - An overrun is named, as an underrun already was: the warning RtAudio raises for it carried whatever error came
#     before, which a program reading the text for a disconnection ("No such device") could take for one.
#
# Only a Linux build with ALSA compiles the code; nothing else of the dependency is touched. The replaced text is RtAudio
# 6.0.1's, the anchors exact - a release that moves them fails here, to be read and the patch updated or dropped.
#
# Versioned by this file's own SHA-256, which the PATCH_COMMAND passes as VERSION and writes into the patched code:
# the version changing changes the PATCH_COMMAND, so ExternalProject runs the patch step again over a kept tree - a CI
# runner's, a developer's - and the script puts back the source it first patched (kept beside it) and applies itself
# afresh. The same version again leaves the tree exactly as it is, so its timestamp does not move.
if(NOT DEFINED SRC OR NOT DEFINED VERSION)
    message(FATAL_ERROR "PatchRtAudioAlsaXruns.cmake: pass -DSRC=<the RtAudio source directory> and "
                        "-DVERSION=<this script's SHA-256>")
endif()
set(source "${SRC}/RtAudio.cpp")
set(unpatched "${SRC}/RtAudio.cpp.before-alsa-xruns")
set(marker "// Patched by the build: cmake/PatchRtAudioAlsaXruns.cmake ${VERSION}")
if(NOT EXISTS "${source}")
    message(FATAL_ERROR "PatchRtAudioAlsaXruns.cmake: ${source} is not there - RtAudio has moved its source")
endif()
file(READ "${source}" text)
string(FIND "${text}" "${marker}" current)
if(NOT current EQUAL -1)
    return()
endif()
string(FIND "${text}" "static int alsaPrimePlayback(" older)
if(older EQUAL -1)
    file(WRITE "${unpatched}" "${text}")
elseif(EXISTS "${unpatched}")
    file(READ "${unpatched}" text)
else()
    message(FATAL_ERROR "PatchRtAudioAlsaXruns.cmake: another version of this patch is applied to ${source} and the "
                        "source it was applied to is not beside it: delete ${SRC} and the subbuild beside it, and "
                        "configure again")
endif()

# The stop threshold: the buffer's size, read back from the hardware configuration just installed.
set(threshold_before [==[
  // Set the software configuration to fill buffers with zeros and prevent device stopping on xruns.
  snd_pcm_sw_params_t *sw_params = NULL;
  snd_pcm_sw_params_alloca( &sw_params );
  snd_pcm_sw_params_current( phandle, sw_params );
  snd_pcm_sw_params_set_start_threshold( phandle, sw_params, *bufferSize );
  snd_pcm_sw_params_set_stop_threshold( phandle, sw_params, ULONG_MAX );
  snd_pcm_sw_params_set_silence_threshold( phandle, sw_params, 0 );
]==])
set(threshold_after [==[
  // Set the software configuration to fill buffers with zeros, and to stop the device on an xrun - the stop threshold
  // the buffer's size, below - so the callback is told of it and the stream plays on (patched by the build: the
  // release's ULONG_MAX let a device run on through an xrun no callback heard of).
  snd_pcm_sw_params_t *sw_params = NULL;
  snd_pcm_sw_params_alloca( &sw_params );
  snd_pcm_sw_params_current( phandle, sw_params );
  snd_pcm_sw_params_set_start_threshold( phandle, sw_params, *bufferSize );
  snd_pcm_sw_params_set_silence_threshold( phandle, sw_params, 0 );
]==])
set(silence_before [==[
  snd_pcm_sw_params_get_boundary( sw_params, &val );
  snd_pcm_sw_params_set_silence_size( phandle, sw_params, val );
]==])
set(silence_after [==[
  snd_pcm_sw_params_get_boundary( sw_params, &val );
  snd_pcm_sw_params_set_silence_size( phandle, sw_params, val );
  if ( snd_pcm_hw_params_get_buffer_size( hw_params, &val ) == 0 )
    snd_pcm_sw_params_set_stop_threshold( phandle, sw_params, val );
]==])

# The priming, a function of its own ahead of the stream's start.
set(prime_before [==[
RtAudioErrorType RtApiAlsa :: startStream()
{
]==])
set(prime_after [==[
@MARKER@
// Patched by the build: a duplex stream's playback primed with a buffer of silence. The stream reads a period and writes
// a period each pass, its capture setting the pace, so its playback holds what it held when it started; primed full, the
// write starting it (and a capture linked to it), it runs a buffer ahead of the device. On a prepared playback, before
// the stream starts and after an xrun has stopped it: the zeros are silence in every format RtAudio opens.
static int alsaPrimePlayback( snd_pcm_t *handle, bool interleaved, unsigned int channels, unsigned int bytes )
{
  snd_pcm_sframes_t frames = snd_pcm_avail_update( handle );
  if ( frames <= 0 ) return (int) frames;
  std::vector<char> silence( (size_t) frames * ( interleaved ? channels : 1 ) * bytes, 0 );
  snd_pcm_sframes_t written;
  if ( interleaved )
    written = snd_pcm_writei( handle, silence.data(), (snd_pcm_uframes_t) frames );
  else {
    std::vector<void *> buffers( channels, silence.data() );
    written = snd_pcm_writen( handle, buffers.data(), (snd_pcm_uframes_t) frames );
  }
  return written < 0 ? (int) written : 0;
}

RtAudioErrorType RtApiAlsa :: startStream()
{
]==])

# The stream's start: a duplex stream's playback primed once both directions are prepared.
set(start_before [==[
        errorStream_ << "RtApiAlsa::startStream: error preparing input pcm device, " << snd_strerror( result ) << ".";
        errorText_ = errorStream_.str();
        goto unlock;
      }
    }
  }

  stream_.state = STREAM_RUNNING;
]==])
set(start_after [==[
        errorStream_ << "RtApiAlsa::startStream: error preparing input pcm device, " << snd_strerror( result ) << ".";
        errorText_ = errorStream_.str();
        goto unlock;
      }
    }
  }

  // Patched by the build: a duplex stream's playback primed, which starts it.
  if ( stream_.mode == DUPLEX ) {
    result = alsaPrimePlayback( handle[0], stream_.deviceInterleaved[0], stream_.nDeviceChannels[0],
                                formatBytes( stream_.deviceFormat[0] ) );
    if ( result < 0 ) {
      errorStream_ << "RtApiAlsa::startStream: error priming output pcm device, " << snd_strerror( result ) << ".";
      errorText_ = errorStream_.str();
      goto unlock;
    }
  }

  stream_.state = STREAM_RUNNING;
]==])

# An overrun: a playback the capture's preparing prepared with it (a linked pair) primed again.
set(overrun_before [==[
          result = snd_pcm_prepare( handle[1] );
          if ( result < 0 ) {
            errorStream_ << "RtApiAlsa::callbackEvent: error preparing device after overrun, " << snd_strerror( result ) << ".";
            errorText_ = errorStream_.str();
          }
]==])
set(overrun_after [==[
          result = snd_pcm_prepare( handle[1] );
          if ( result < 0 ) {
            errorStream_ << "RtApiAlsa::callbackEvent: error preparing device after overrun, " << snd_strerror( result ) << ".";
            errorText_ = errorStream_.str();
          }
          else {
            // Patched by the build: the overrun named, as the underrun is - the warning below reported whatever error
            // came last - and the playback linked to this capture, prepared with it, primed again.
            errorText_ = "RtApiAlsa::callbackEvent: audio read error, overrun.";
            if ( stream_.mode == DUPLEX && snd_pcm_state( handle[0] ) == SND_PCM_STATE_PREPARED ) {
              result = alsaPrimePlayback( handle[0], stream_.deviceInterleaved[0], stream_.nDeviceChannels[0],
                                          formatBytes( stream_.deviceFormat[0] ) );
              if ( result < 0 ) {
                errorStream_ << "RtApiAlsa::callbackEvent: error priming device after overrun, " << snd_strerror( result ) << ".";
                errorText_ = errorStream_.str();
              }
            }
          }
]==])

# An underrun: a duplex stream's playback primed again once prepared.
set(underrun_before [==[
          result = snd_pcm_prepare( handle[0] );
          if ( result < 0 ) {
            errorStream_ << "RtApiAlsa::callbackEvent: error preparing device after underrun, " << snd_strerror( result ) << ".";
            errorText_ = errorStream_.str();
          }
          else
            errorText_ =  "RtApiAlsa::callbackEvent: audio write error, underrun.";
]==])
set(underrun_after [==[
          result = snd_pcm_prepare( handle[0] );
          if ( result < 0 ) {
            errorStream_ << "RtApiAlsa::callbackEvent: error preparing device after underrun, " << snd_strerror( result ) << ".";
            errorText_ = errorStream_.str();
          }
          else
            errorText_ =  "RtApiAlsa::callbackEvent: audio write error, underrun.";
          if ( result >= 0 && stream_.mode == DUPLEX ) {
            // Patched by the build: a duplex stream's playback, prepared, primed again.
            result = alsaPrimePlayback( handle[0], stream_.deviceInterleaved[0], stream_.nDeviceChannels[0],
                                        formatBytes( stream_.deviceFormat[0] ) );
            if ( result < 0 ) {
              errorStream_ << "RtApiAlsa::callbackEvent: error priming device after underrun, " << snd_strerror( result ) << ".";
              errorText_ = errorStream_.str();
            }
          }
]==])

string(REPLACE "@MARKER@" "${marker}" prime_after "${prime_after}")

foreach(part IN ITEMS threshold silence prime start overrun underrun)
    string(FIND "${text}" "${${part}_before}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "PatchRtAudioAlsaXruns.cmake: the ALSA back end no longer reads as this patch expects at "
                            "its ${part} change - read ${source} and update the patch, or drop it if RtAudio now stops "
                            "a device on an xrun itself")
    endif()
    string(LENGTH "${${part}_before}" length)
    math(EXPR after_end "${at} + ${length}")
    string(SUBSTRING "${text}" ${after_end} -1 rest)
    string(FIND "${rest}" "${${part}_before}" again)
    if(NOT again EQUAL -1)
        message(FATAL_ERROR "PatchRtAudioAlsaXruns.cmake: the ${part} change's text appears twice in ${source}")
    endif()
endforeach()
foreach(part IN ITEMS threshold silence prime start overrun underrun)
    string(REPLACE "${${part}_before}" "${${part}_after}" text "${text}")
endforeach()
file(WRITE "${source}" "${text}")
message(STATUS "RtAudio: the ALSA back end stops a device on an xrun, tells the callback, and primes a duplex stream's "
               "playback (${source})")
