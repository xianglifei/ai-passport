# 卡卡应用:基线 demo 源码保留在树中供参考,但不参与本应用构建。
idf_component_register(
    SRCS "main.c"
         "pet_model.c"
         "pet_store.c"
         "pet_nvs.c"
         "pet_ui.c"
         "pet_scenes.c"
         "pet_audio.c"
         "pet_font.c"
         "pet_art.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_timer nvs_flash
)

# 中文字体子集(生成方式见 assets/fonts/gen_app_fonts.sh;勿 #include 其 .c)
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/app_font_16.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/app_font_20.c")
