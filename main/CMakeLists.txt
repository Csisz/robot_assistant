set(SOURCES
    "./main.c"
    "./robot_face.c"
    "./hardeware_driver/bsp_board.c"
    "./audio_play_driver/audio_driver.c"
    "./tca9555_driver/tca9555_driver.c"
    "./lcd_driver/lcd_driver.c"
    "./lvgl_driver/lvgl_driver.c"
    "./robot_state.c"
    "./button_driver/button_driver.c"
    "./camera_driver/camera_driver.c"
)

set(INCLUDE_DIRS
    "."
    "./hardeware_driver"
    "./audio_play_driver"
    "./tca9555_driver"
    "./lcd_driver"
    "./lvgl_driver"
    "./button_driver"
    "./camera_driver"
)

idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS ${INCLUDE_DIRS}
)
