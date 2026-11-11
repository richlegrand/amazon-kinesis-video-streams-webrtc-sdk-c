# media_stream's interface headers on their own: the capture and player
# function tables, their config structs and handle types. app_webrtc and
# kvs_webrtc only pass these tables through, so they depend on this instead of
# media_stream, and an application that brings its own media (or none) does
# not build media_stream's encoders, camera driver and codec libraries, or see
# its menuconfig.
#
# The headers stay in media_stream/include, where upstream has them;
# media_stream itself is unchanged and still exports the same directory.
idf_component_register(
    INCLUDE_DIRS "../media_stream/include"
    REQUIRES esp_common
)
