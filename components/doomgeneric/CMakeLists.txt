file(GLOB DOOMGENERIC_SRCS "src/*.c")
list(APPEND DOOMGENERIC_SRCS "${CMAKE_CURRENT_LIST_DIR}/../../main/audio_doom.c")

list(FILTER DOOMGENERIC_SRCS EXCLUDE REGEX ".*/doomgeneric_(allegro|emscripten|linuxvt|sdl|soso|sosox|win|xlib)\\.c$")
list(FILTER DOOMGENERIC_SRCS EXCLUDE REGEX ".*/i_(allegro|sdl).*\\.c$")

idf_component_register(SRCS ${DOOMGENERIC_SRCS}
                       INCLUDE_DIRS "src"
                       PRIV_REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_codec_dev nvs_flash)

target_compile_definitions(${COMPONENT_LIB} PUBLIC
    FEATURE_SOUND=1
    DOOMGENERIC_RESX=320
    DOOMGENERIC_RESY=200)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-variable
    -Wno-unused-function
    -Wno-unused-but-set-parameter
    -Wno-format
    -Wno-char-subscripts
    -Wno-dangling-pointer
    -Wno-absolute-value
    -Wno-incompatible-pointer-types
    -Wno-int-conversion)
