# Every pin the toolkit fetches by, named once: the fetches in the toolkit's CMakeLists.txt and skia/CMakeLists.txt are
# built from these, and a consumer's third-party notices name every one (vst-plugins' tools/third_party_notices.py reads
# this file, and its common test is handed the values, which the toolkit's CMakeLists.txt passes up to the directory
# that added it). Each is a release's tag or version, or a commit, with the archives' hashes beside the fetches.

# GoogleTest, for the suites: the toolkit's and a consumer's.
set(RECKLESS_GOOGLETEST_TAG "v1.17.0")

# Steinberg's ASIO SDK, which the audio layer compiles its ASIO back end on (Windows): its versioned name on
# Steinberg's download server and the date in that name.
set(RECKLESS_ASIO_SDK_VERSION "2.3.4")
set(RECKLESS_ASIO_SDK_RELEASED "2025-10-15")

# The audio file decoders: dr_flac and dr_mp3 from one commit of dr_libs, stb_vorbis (and stb_image, which a consumer
# may compile from the same tree) from one commit of stb.
set(RECKLESS_DR_LIBS_COMMIT "dfe8377631000664666519fdb83da193fd8037f4")
set(RECKLESS_STB_COMMIT "1ee679ca2ef753a528db5ba6801e1067b40481b8")

# Monocypher, whose Ed25519 checks a licence's signature.
set(RECKLESS_MONOCYPHER_VERSION "4.0.3")

# The skia-pack release whose prebuilt Skia the panels draw through (skia/CMakeLists.txt fetches its libraries for the
# platform, each archive pinned by its hash there); the Skia commit that release was built from, whose LICENSE - which
# the pack does not carry - is fetched beside it; and the Chromium revision the zlib commit Skia's DEPS names was copied
# from (that commit's GitOrigin-RevId), whose LICENSE governs the SIMD routines the Chromium authors added to that zlib.
set(RECKLESS_SKIA_TAG "m144-e2e6623374-4")
set(RECKLESS_SKIA_COMMIT "e2e66233743d048dc880203b6598fd34d94d9765")
set(RECKLESS_CHROMIUM_COMMIT "2f39ac8d0a414dd65c0e1d5aae38c8f97aa06ae9")

# The names above, for the toolkit's CMakeLists.txt to pass up to a consumer.
set(RECKLESS_TOOLKIT_PINS
    RECKLESS_GOOGLETEST_TAG RECKLESS_ASIO_SDK_VERSION RECKLESS_ASIO_SDK_RELEASED RECKLESS_DR_LIBS_COMMIT
    RECKLESS_STB_COMMIT RECKLESS_MONOCYPHER_VERSION RECKLESS_SKIA_TAG RECKLESS_SKIA_COMMIT RECKLESS_CHROMIUM_COMMIT)
